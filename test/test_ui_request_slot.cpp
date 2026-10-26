// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the touch UI's single-flight request slot expiry (cec1431c):
// src/helpers/UiRequestSlot.h, used by MyMesh::uiRequestTick (src/MyMesh.h) and the
// web build's stand-in (web/mesh/WebMesh.cpp). Run from the repo root (it also reads
// src/MyMesh.h and web/mesh/WebMesh.cpp to check every slot taker stamps the time):
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_ui_request_slot.cpp -o /tmp/test_ui_request_slot \
//       && /tmp/test_ui_request_slot
//
// The end-to-end flow (a lost telemetry reply holds the slot, the admin app's queued
// STATUS goes out after the expiry) runs in the web host build:
// web/scripts/ui-slot-expiry.wada with WADA_DEMO_SILENT_TELEMETRY=1.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "helpers/UiRequestSlot.h"

// The slot as MyMesh keeps it: the first 4 bytes of the key (0 = free), when it was
// taken, and the deferred guest login with its own stamp. tick() is uiRequestTick's
// expiry part; reply() is onContactResponse's tag match.
struct Slot {
  uint32_t key = 0, at = 0, tag = 0;
  uint32_t login_then = 0, login_then_at = 0;
  void take(uint32_t k, uint32_t t, uint32_t now) { key = k; tag = t; at = now; }
  void armLogin(uint32_t k, uint32_t now) { login_then = k; login_then_at = now; }
  void tick(uint32_t now) {
    if (key && UiRequestSlot::expired(now, at)) { key = 0; tag = 0; }
    if (login_then && UiRequestSlot::expired(now, login_then_at)) login_then = 0;
  }
  bool reply(uint32_t k, uint32_t t) {
    if (!key || key != k || t != tag) return false;
    key = 0; tag = 0; login_then = 0;
    return true;
  }
  bool pending() const { return key != 0 || login_then != 0; }   // hasUIRequestPending
};

// The admin app's pump (RepeaterAdminApp.h raPump): a queued STATUS waits while the
// slot is held, then goes out and takes it.
struct Admin {
  bool status_queued = false, status_sent = false;
  uint32_t sent_at = 0;
  void pump(Slot& s, uint32_t now) {
    if (!status_queued || s.pending()) return;
    status_queued = false; status_sent = true; sent_at = now;
    s.take(0xA1A1A1A1u, 77, now);
  }
};

static std::string slurp(const char* path) {
  std::ifstream f(path);
  if (!f) { fprintf(stderr, "cannot read %s (run from the repo root)\n", path); assert(false); }
  std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

// Every statement that takes the slot must stamp its time within the next few lines,
// or the slot carries a stale stamp and expires at once (or 45 s too late).
static int checkStamps(const char* path, const char* take, const char* stamp) {
  const std::string src = slurp(path);
  std::vector<std::string> lines;
  { std::stringstream ss(src); for (std::string l; std::getline(ss, l);) lines.push_back(l); }
  int takers = 0;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].find(take) == std::string::npos) continue;
    ++takers;
    bool ok = false;
    for (size_t j = i; j < lines.size() && j <= i + 4; ++j) if (lines[j].find(stamp) != std::string::npos) ok = true;
    if (!ok) { fprintf(stderr, "%s:%zu takes the slot without %s\n", path, i + 1, stamp); assert(false); }
  }
  return takers;
}

int main() {
  const uint32_t T = 1000;

  // ---- the boundary: free at exactly 45 s, not a millisecond before ----
  assert(UiRequestSlot::MAX_MS == 45000);
  assert(!UiRequestSlot::expired(T, T));
  assert(!UiRequestSlot::expired(T + 44999, T));
  assert(UiRequestSlot::expired(T + 45000, T));
  assert(UiRequestSlot::expired(T + 3600000, T));

  // ---- millis() wraps after ~49.7 days: the age is still right across it ----
  const uint32_t W = 0xFFFFFFFFu - 10000;   // taken 10 s before the wrap
  assert(!UiRequestSlot::expired(W + 20000, W));          // 20 s later, after the wrap
  assert(!UiRequestSlot::expired(W + 44999, W));
  assert(UiRequestSlot::expired(W + 45000, W));
  assert(UiRequestSlot::expired((uint32_t)(W + 60000), W));

  // ---- an unanswered telemetry ask holds the slot; the admin STATUS waits, then goes ----
  {
    Slot s; Admin a;
    s.take(0x11223344u, 5, T);           // contact sheet Telemetry / auto-poll / Home widget
    a.status_queued = true;              // admin logs in 5 s later
    for (uint32_t now = T; now <= T + 60000; now += 200) { s.tick(now); a.pump(s, now); }
    assert(a.status_sent);
    assert(a.sent_at >= T + 45000 && a.sent_at < T + 45200);   // first tick after the expiry
  }

  // ---- an answered request clears at once: the STATUS goes on the next pump ----
  {
    Slot s; Admin a;
    s.take(0x11223344u, 5, T);
    a.status_queued = true;
    s.tick(T + 1200); a.pump(s, T + 1200);
    assert(!a.status_sent);
    assert(!s.reply(0x11223344u, 6));    // a frame with another tag is not the answer
    assert(!s.reply(0x55667788u, 5));    // nor one from another node
    assert(s.reply(0x11223344u, 5));
    s.tick(T + 1400); a.pump(s, T + 1400);
    assert(a.status_sent && a.sent_at == T + 1400);
  }

  // ---- a reply that comes after the expiry matches nothing (the tag was dropped) ----
  {
    Slot s;
    s.take(0x11223344u, 5, T);
    s.tick(T + 45000);
    assert(!s.pending());
    assert(!s.reply(0x11223344u, 5));
  }

  // ---- the deferred guest login has its own 45 s, from when it was armed ----
  {
    Slot s;
    s.take(0x11223344u, 5, T);           // direct-first request
    s.armLogin(0x11223344u, T + 12000);  // fallback login 12 s later (no path)
    s.tick(T + 45000);
    assert(s.key == 0 && s.login_then != 0 && s.pending());
    s.tick(T + 56999);
    assert(s.pending());
    s.tick(T + 57000);
    assert(!s.pending());
  }

  // ---- every slot taker in the firmware and the web stand-in stamps the time ----
  const int fw = checkStamps("src/MyMesh.h", "memcpy(&_ui_pending_status,", "_ui_pending_at");
  const int fw_login = checkStamps("src/MyMesh.h", "memcpy(&_ui_login_then,", "_ui_login_then_at");
  const int web = checkStamps("web/mesh/WebMesh.cpp", "memcpy(_ui_pending_key,", "_ui_pending_at");
  assert(fw >= 4 && fw_login >= 1);
  assert(web >= 4);
  // Both expiries go through the shared helper (one constant, one rule).
  assert(slurp("src/MyMesh.h").find("UiRequestSlot::expired(now, _ui_pending_at)") != std::string::npos);
  assert(slurp("src/MyMesh.h").find("UiRequestSlot::expired(now, _ui_login_then_at)") != std::string::npos);
  assert(slurp("web/mesh/WebMesh.cpp").find("UiRequestSlot::expired(now, _ui_pending_at)") != std::string::npos);

  printf("test_ui_request_slot: ok (%d firmware slot takers, %d login arms checked)\n", fw, fw_login);
  return 0;
}
