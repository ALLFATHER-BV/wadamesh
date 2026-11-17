// SPDX-License-Identifier: GPL-3.0-or-later
// System side of the 4G modem (T-Deck Max), on the UI thread:
//   - drains CellularService events every loop pass
//   - shows the firmware's own call sheet (UITask.cpp) unless the Phone app is in front
//   - rings with the notification chime while a call is ringing
//   - keeps the SMS inbox on the app storage root, so no message is lost when no app is open
//   - forwards every event to the running Lua app (app.on_cell)
#if HAS_CELLULAR
#include <Arduino.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "LuaAppHost.h"
#include "CellularService.h"

extern void luaHostToast(const char* msg, int ms);
extern bool luaHostBeep();
extern fs::FS* luaHostAppFs();
extern void luaHostAppPath(char* out, size_t cap, const char* rel);
extern void uiCellCallSheet(bool show);   // UITask.cpp

static const char* kPhoneAppId = "phone";
static const int   kInboxKeep  = 50;      // messages kept on the card

static bool phoneAppInFront() {
  return luaAppIsOpen() && strcmp(luaAppOpenId(), kPhoneAppId) == 0;
}

// The apps folder is normally created by the first Store install. Create it as
// soon as storage is up, so the Phone app can be side-loaded over USB Files
// (which may write app files there but may not create folders under the data root).
static void ensureAppsDir() {
  static bool done = false;
  if (done) return;
  fs::FS* fs = luaHostAppFs();
  if (!fs) return;            // storage not mounted yet; try again next pass
  // Same for the language folder: a .lang file can then be side-loaded too.
  for (const char* rel : { "/apps", "/lang" }) {
    char d[64];
    luaHostAppPath(d, sizeof d, rel);
    if (!fs->exists(d)) {
      const bool ok = fs->mkdir(d);
      Serial.printf("[CELL] created %s %s\n", d, ok ? "ok" : "FAILED");
    }
  }
  done = true;
}

// ---- SMS inbox: "<when>\t<from>\t<text>\n" per message, oldest first ----------
static void inboxPath(char* out, size_t cap) { luaHostAppPath(out, cap, "/sms_inbox.txt"); }

static void sanitize(char* s) {   // the file format is one line per message
  for (; *s; ++s) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static bool parseLine(char* line, CellInboxEntry* e) {
  char* t1 = strchr(line, '\t');
  if (!t1) return false;
  char* t2 = strchr(t1 + 1, '\t');
  if (!t2) return false;
  *t1 = '\0'; *t2 = '\0';
  snprintf(e->when, sizeof e->when, "%s", line);
  snprintf(e->from, sizeof e->from, "%s", t1 + 1);
  snprintf(e->text, sizeof e->text, "%s", t2 + 1);
  return true;
}

int cellInboxLoad(CellInboxEntry* out, int max) {
  if (!out || max <= 0) return 0;
  fs::FS* fs = luaHostAppFs();
  if (!fs) return 0;
  char path[80];
  inboxPath(path, sizeof path);
  File f = fs->open(path, "r");
  if (!f) return 0;
  // Ring of the newest `max`, filled oldest-first, then reversed.
  int n = 0, head = 0;
  static char line[400];
  while (f.available()) {
    const size_t got = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[got] = '\0';
    CellInboxEntry e;
    if (!parseLine(line, &e)) continue;
    out[head] = e;
    head = (head + 1) % max;
    if (n < max) n++;
  }
  f.close();
  // Reorder newest first.
  CellInboxEntry* tmp = (CellInboxEntry*)heap_caps_malloc(sizeof(CellInboxEntry) * n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!tmp) return 0;
  for (int i = 0; i < n; i++) tmp[i] = out[(head - 1 - i + max) % max];
  memcpy(out, tmp, sizeof(CellInboxEntry) * n);
  heap_caps_free(tmp);
  return n;
}

bool cellInboxClear() {
  fs::FS* fs = luaHostAppFs();
  if (!fs) return false;
  char path[80];
  inboxPath(path, sizeof path);
  return !fs->exists(path) || fs->remove(path);
}

static void inboxAppend(const char* when, const char* from, const char* text) {
  fs::FS* fs = luaHostAppFs();
  if (!fs) return;
  char path[80];
  inboxPath(path, sizeof path);
  static char w[24], fr[32], tx[284];
  snprintf(w, sizeof w, "%s", when); sanitize(w);
  snprintf(fr, sizeof fr, "%s", from); sanitize(fr);
  snprintf(tx, sizeof tx, "%s", text); sanitize(tx);
  File f = fs->open(path, "a");
  if (!f) { Serial.println("[CELL] inbox: open failed"); return; }
  f.printf("%s\t%s\t%s\n", w, fr, tx);
  f.close();

  // Trim to the newest kInboxKeep once it has grown well past that.
  static int s_appends = 0;
  if (++s_appends % 10 != 0) return;
  CellInboxEntry* all = (CellInboxEntry*)heap_caps_malloc(sizeof(CellInboxEntry) * kInboxKeep,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!all) return;
  const int n = cellInboxLoad(all, kInboxKeep);
  File o = fs->open(path, "w");
  if (o) {
    for (int i = n - 1; i >= 0; i--) o.printf("%s\t%s\t%s\n", all[i].when, all[i].from, all[i].text);
    o.close();
  }
  heap_caps_free(all);
}

void cellUiPump() {
  ensureAppsDir();
  const bool phone = phoneAppInFront();
  uiCellCallSheet(!phone);   // the Phone app draws its own call screen

  CellEvent ev;
  int budget = 4;   // a burst of events must not hold up the UI loop
  while (budget-- > 0 && cellPollEvent(&ev)) {
    char msg[200];
    switch (ev.type) {
      case CELL_EV_RING:
        luaHostBeep();   // the ring tone: one chime per network RING (~3 s)
        break;
      case CELL_EV_SMS:
        inboxAppend(ev.when, ev.text, ev.body);
        luaHostBeep();
        if (!phone) {
          snprintf(msg, sizeof msg, "SMS %s: %.140s", ev.text, ev.body);
          luaHostToast(msg, 5000);
        }
        break;
      case CELL_EV_SMS_SENT:
        if (!phone) luaHostToast(ev.code ? "SMS sent" : "SMS failed", 2000);
        break;
      default:
        break;
    }
#if CAP_LUA_APPS
    luaAppCellEvent(&ev);
#endif
  }
}
#endif  // HAS_CELLULAR
