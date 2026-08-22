// SPDX-License-Identifier: GPL-3.0-or-later
#include "ConsoleUI.h"

#if CAP_CONSOLE

#include <Arduino.h>
#include <stdarg.h>
#include <string.h>
#include <helpers/ui/DisplayDriver.h>
#if defined(ESP32)
  #include <esp_heap_caps.h>
#endif

// The command backend. Both of these already exist and are what the LVGL
// Terminal app uses, so console mode inherits the full command set rather than
// growing a private one.
#include "../MyMesh.h"   // declares the_mesh itself (a reference on PSRAM boards)
#if defined(ESP32)
  #include "../helpers/esp32/TouchPrefsStore.h"
#endif

// Contacts, channels and the send path, all from UITask so the console reuses
// exactly what the UI and the Lua host use. In particular the channel send
// matches BY NAME at transmit time; a cached slot index is how messages once
// went out encrypted to the wrong channel.
extern int  luaHostContactAt(int idx, char* name, size_t name_cap, int* type, uint32_t* secs_ago,
                             double* lat, double* lon, char* pk_hex, size_t pk_cap,
                             int32_t* lat_e6, int32_t* lon_e6);
extern int  luaHostMeshChannelNames(char out[][32], int max_n);
extern bool luaHostMeshSendChannel(const char* chan_name, const char* text);
extern bool luaHostMeshSendDM(const char* to_name, const char* text, bool* was_room);

#if CAP_TOUCH
// Board touch driver, read directly. lvglTouchRead() is only an adapter that
// feeds LVGL, so there is nothing to unpick here: the driver polls either way.
extern bool heltecV4CapTouchGetLive(uint16_t* x, uint16_t* y);
#endif

namespace {

// ---- scrollback -------------------------------------------------------------
// Flat ring in PSRAM: no per-line allocation, so a busy command cannot fragment
// the heap the way a strdup-per-line log would. Lines are fixed width and long
// output is wrapped into several of them by consoleWriteLine.
constexpr int  kMaxLines = 160;
constexpr int  kLineCap  = 96;      // chars per stored line, excluding the NUL
constexpr int  kInputCap = 128;

char*    s_ring       = nullptr;    // kMaxLines * (kLineCap + 1)
int      s_head       = 0;          // next write slot
int      s_count      = 0;
int      s_scroll     = 0;          // lines scrolled back from the newest
// Two levels of dirt, because startFrame() is a full fillScreen() and endFrame()
// is a no-op: drawing is direct to the panel with no buffer. A full redraw for
// every cursor blink flashed the whole screen twice a second, and one per
// keystroke did the same while typing. Only the scrollback changing needs the
// full clear; the input line and the cursor repaint their own few pixels.
bool     s_dirty      = true;      // scrollback changed -> full redraw
bool     s_dirty_in   = false;     // only the input line / cursor changed
bool     s_active     = false;
int      s_cur_x = 0, s_cur_y = 0; // cursor cell, set by the last input-line draw

DisplayDriver* s_disp = nullptr;
int      s_char_w = 6, s_line_h = 8, s_cols = 40, s_rows = 10;

char     s_input[kInputCap] = {0};
int      s_input_len = 0;
uint32_t s_blink_ms  = 0;
bool     s_blink_on  = true;

inline char* lineAt(int i) { return s_ring + (size_t)i * (kLineCap + 1); }

void ringPush(const char* s, int len) {
  if (!s_ring) return;
  if (len > kLineCap) len = kLineCap;
  char* dst = lineAt(s_head);
  memcpy(dst, s, len);
  dst[len] = '\0';
  s_head = (s_head + 1) % kMaxLines;
  if (s_count < kMaxLines) s_count++;
  s_scroll = 0;              // any new output jumps back to the live tail
  s_dirty = true;
}

// Paint just the input row: clear it to the background, then prompt + text.
// Never touches the scrollback above it or the keypad below, so no flash.
void drawInputLine(bool cursor_on);

// i = 0 is the OLDEST retained line.
const char* ringGet(int i) {
  if (i < 0 || i >= s_count) return nullptr;
  const int start = (s_head - s_count + kMaxLines * 2) % kMaxLines;
  return lineAt((start + i) % kMaxLines);
}

#if CAP_TOUCH && !CAP_KEYBOARD
// ---- on-screen keypad -------------------------------------------------------
// Touch boards have no hardware keyboard and the firmware's own on-screen one is
// LVGL, so console mode draws its own. Deliberate: the V4 is the board this
// feature exists for, so it has to be usable there without a keyboard.
//
// Three layers rather than a shift key that changes every glyph: lower, upper,
// and symbols. Fewer states to get wrong, and the label on a key is always what
// that key types.
constexpr int kRows = 4;
const char* const kLayer[3][kRows] = {
  { "qwertyuiop", "asdfghjkl",  "\x01zxcvbnm\x08", "\x02 \n" },   // lower
  { "QWERTYUIOP", "ASDFGHJKL",  "\x01ZXCVBNM\x08", "\x02 \n" },   // upper
  { "1234567890", "-/:;()$&@\"", "\x01.,?!'#\x08",  "\x02 \n" },   // symbols
};
// \x01 = layer cycle, \x02 = scroll-back toggle, \x08 = backspace, \n = enter.
int  s_layer = 0;
int  s_kb_top = 0;          // y where the keypad starts; scrollback ends here
int  s_key_h  = 0;

void keypadLayout() {
  if (!s_disp) return;
  s_key_h  = s_disp->height() / 12;          // proportional, so it fits 240x320 and bigger
  if (s_key_h < 14) s_key_h = 14;
  s_kb_top = s_disp->height() - kRows * s_key_h;
}

void keypadDraw() {
  if (!s_disp) return;
  for (int r = 0; r < kRows; r++) {
    const char* row = kLayer[s_layer][r];
    const int n = (int)strlen(row);
    if (n <= 0) continue;
    const int kw = s_disp->width() / n;
    const int y  = s_kb_top + r * s_key_h;
    for (int c = 0; c < n; c++) {
      const int x = c * kw;
      s_disp->setColor(UIColor::secondary_txt);
      s_disp->drawRect(x, y, kw - 1, s_key_h - 1);
      char lbl[8];
      switch (row[c]) {
        case '\x01': snprintf(lbl, sizeof lbl, "%s", s_layer == 2 ? "ab" : (s_layer == 1 ? "12" : "AB")); break;
        case '\x02': snprintf(lbl, sizeof lbl, "%s", "^v"); break;
        case '\x08': snprintf(lbl, sizeof lbl, "%s", "<-"); break;
        case '\n':   snprintf(lbl, sizeof lbl, "%s", "ret"); break;
        case ' ':    snprintf(lbl, sizeof lbl, "%s", "spc"); break;
        default:     lbl[0] = row[c]; lbl[1] = '\0'; break;
      }
      s_disp->setColor(UIColor::primary_txt);
      s_disp->drawTextCentered(x + kw / 2, y + (s_key_h - s_line_h) / 2, lbl);
    }
  }
}

// Which key is under (x, y)? Returns 0 when the tap missed the keypad.
char keypadHit(int x, int y) {
  if (!s_disp || y < s_kb_top) return 0;
  int r = (y - s_kb_top) / (s_key_h ? s_key_h : 1);
  if (r < 0) r = 0;
  if (r >= kRows) r = kRows - 1;
  const char* row = kLayer[s_layer][r];
  const int n = (int)strlen(row);
  if (n <= 0) return 0;
  const int kw = s_disp->width() / n;
  int c = kw ? (x / kw) : 0;
  if (c < 0) c = 0;
  if (c >= n) c = n - 1;
  return row[c];
}

uint32_t s_touch_start = 0;
uint16_t s_touch_x = 0, s_touch_y = 0;
bool     s_scroll_mode = false;   // ^v pressed: taps above the keypad scroll

void touchTick() {
  uint16_t tx, ty;
  const bool pressed = heltecV4CapTouchGetLive(&tx, &ty);
  const uint32_t now = millis();
  if (pressed && !s_touch_start) {
    s_touch_start = now;
    s_touch_x = tx; s_touch_y = ty;
    return;
  }
  if (pressed || !s_touch_start) return;
  const uint32_t held = now - s_touch_start;
  s_touch_start = 0;
  if (!s_disp) return;

  const char k = keypadHit(s_touch_x, s_touch_y);
  if (k) {
    switch (k) {
      case '\x01': s_layer = (s_layer + 1) % 3; s_dirty = true; break;
      case '\x02': s_scroll_mode = !s_scroll_mode; s_dirty = true; break;
      case '\x08': consoleKey('\b'); break;
      case '\n':   consoleKey('\n'); break;
      default:     consoleKey(k); break;
    }
    return;
  }
  // Above the keypad. In scroll mode the halves page the scrollback; otherwise a
  // long press there is the way out, mirroring how remote mode is left.
  if (s_scroll_mode) {
    if (s_touch_y < s_kb_top / 2) { if (s_scroll < s_count - s_rows) { s_scroll++; s_dirty = true; } }
    else                          { if (s_scroll > 0)                { s_scroll--; s_dirty = true; } }
  } else if (held >= 1200) {
    consoleWriteLine("(hold registered - use the 'ui' command to leave console mode)");
  }
}
#endif  // CAP_TOUCH && !CAP_KEYBOARD

// ---- metrics ----------------------------------------------------------------
// DisplayDriver has getTextWidth but no text height, and the concrete drivers
// scale a fixed 6x8 cell, so derive the height from the measured width instead
// of hardcoding a number that would be wrong at another scale.
void measure() {
  if (!s_disp) return;
  s_disp->setTextSize(1);
  const int w = s_disp->getTextWidth("M");
  s_char_w = w > 0 ? w : 6;
  s_line_h = (s_char_w * 8) / 6;
  if (s_line_h < 8) s_line_h = 8;
  s_cols = s_disp->width() / s_char_w;
  if (s_cols < 8)  s_cols = 8;
  if (s_cols > kLineCap) s_cols = kLineCap;
  // Rows available for scrollback: everything except the input line.
  s_rows = (s_disp->height() / s_line_h) - 1;
#if CAP_TOUCH && !CAP_KEYBOARD
  keypadLayout();
  // The keypad owns the bottom of the panel, so the scrollback and the input
  // line have to live above it rather than under it.
  s_rows = (s_kb_top / s_line_h) - 1;
#endif
  if (s_rows < 2) s_rows = 2;
}

// ---- render -----------------------------------------------------------------
void render() {
  if (!s_disp || !s_active) return;
  s_disp->startFrame(UIColor::window_bkg);
  s_disp->setTextSize(1);

  // Oldest-first from the scroll position, newest at the bottom.
  const int first = s_count - s_rows - s_scroll;
  int y = 0;
  for (int r = 0; r < s_rows; r++) {
    const int idx = first + r;
    const char* l = (idx >= 0) ? ringGet(idx) : nullptr;
    if (l && *l) {
      s_disp->setColor(UIColor::primary_txt);
      s_disp->setCursor(0, y);
      s_disp->print(l);
    }
    y += s_line_h;
  }

  drawInputLine(s_blink_on);

#if CAP_TOUCH && !CAP_KEYBOARD
  keypadDraw();
#endif
  s_disp->endFrame();
  s_dirty = false;
}

// Current recipient, set by `to`. A name rather than an index or a slot, for the
// same reason the send path matches by name: an index goes stale the moment the
// contact list changes underneath it.
char s_to[40] = {0};

void cmdContacts() {
  char name[36], pk[12]; int type; uint32_t ago; double lat, lon; int32_t la6, lo6;
  int shown = 0;
  for (int i = 0; i < 200 && shown < 40; i++) {
    if (!luaHostContactAt(i, name, sizeof name, &type, &ago, &lat, &lon, pk, sizeof pk, &la6, &lo6)) break;
    static const char* kType[5] = { "?", "chat", "repeater", "room", "sensor" };
    char line[kLineCap];
    snprintf(line, sizeof line, "%-16s %-8s %s", name,
             (type >= 1 && type <= 4) ? kType[type] : "?", pk);
    consoleWriteLine(line);
    shown++;
  }
  if (!shown) consoleWriteLine("(no contacts yet)");
}

void cmdChannels() {
  static char names[8][32];
  const int n = luaHostMeshChannelNames(names, 8);
  for (int i = 0; i < n; i++) consoleWriteLine(names[i]);
  if (!n) consoleWriteLine("(no channels)");
}

void drawInputLine(bool cursor_on) {
  if (!s_disp) return;
#if CAP_TOUCH && !CAP_KEYBOARD
  const int iy = s_kb_top - s_line_h;
#else
  const int iy = s_disp->height() - s_line_h;
#endif
  // Clear only this row. fillScreen would take the scrollback and keypad with it.
  s_disp->setColor(UIColor::window_bkg);
  s_disp->fillRect(0, iy, s_disp->width(), s_line_h);

  s_disp->setTextSize(1);
  s_disp->setColor(UIColor::title_txt);
  s_disp->setCursor(0, iy);
  s_disp->print(">");
  s_disp->setColor(UIColor::primary_txt);
  s_disp->setCursor(s_char_w * 2, iy);
  // Show the tail of a long line so the caret stays visible while typing.
  const int room = s_cols - 3;
  const char* shown = s_input;
  if (s_input_len > room) shown = s_input + (s_input_len - room);
  s_disp->print(shown);

  s_cur_x = s_char_w * 2 + s_disp->getTextWidth(shown);
  s_cur_y = iy;
  if (cursor_on) {
    s_disp->setColor(UIColor::primary_txt);
    s_disp->fillRect(s_cur_x, s_cur_y, s_char_w, s_line_h);
  }

  // Scrollback indicator: without it there is no way to tell you are not live.
  if (s_scroll > 0) {
    char tag[24];
    snprintf(tag, sizeof tag, "-%d", s_scroll);
    s_disp->setColor(UIColor::warning_txt);
    s_disp->drawTextRightAlign(s_disp->width() - 2, iy, tag);
  }
}

// Toggle just the cursor cell. Two fillRects, no text, no clear.
void drawCursorOnly(bool on) {
  if (!s_disp) return;
  s_disp->setColor(on ? UIColor::primary_txt : UIColor::window_bkg);
  s_disp->fillRect(s_cur_x, s_cur_y, s_char_w, s_line_h);
}

// ---- command dispatch -------------------------------------------------------
void submit() {
  if (s_input_len == 0) return;
  char cmd[kInputCap];
  snprintf(cmd, sizeof cmd, "%s", s_input);
  consoleWriteLine("");              // blank line between commands, for legibility
  char echo[kLineCap + 4];
  snprintf(echo, sizeof echo, "> %s", cmd);
  consoleWriteLine(echo);
  s_input[0] = '\0';
  s_input_len = 0;

  // Console-only commands first, then everything else to the node CLI. Keeping
  // this list short is deliberate: anything CommonCLI already answers should go
  // there rather than being reimplemented here.
  if (!strcasecmp(cmd, "clear")) {
    s_head = s_count = s_scroll = 0;
    s_dirty = true;
    return;
  }
#if defined(ESP32)
  // The way back to the graphical UI. One of three, per CONSOLE_MODE.md: this,
  // a key held at boot, and clearing the pref over serial or the flasher.
  if (!strcasecmp(cmd, "ui") || !strcasecmp(cmd, "exit")) {
    touchPrefsSetConsoleMode(false);
    consoleWriteLine("switching to the graphical UI, rebooting...");
    render();
    delay(600);            // let the line land on the panel before the reset
    ESP.restart();
    return;
  }
#endif
  if (!strcasecmp(cmd, "help")) {
    consoleWriteLine("console  clear, help, mem, ui");
    consoleWriteLine("mesh     contacts, chans");
    consoleWriteLine("         to <name>   pick a contact or channel");
    consoleWriteLine("         msg <text>  send to it");
    consoleWriteLine("node     anything the CLI answers:");
    consoleWriteLine("         advert, get name, set name <x>, time, ver");
    return;
  }
  // Free memory. This is the number console mode is justified by, so it is a
  // command rather than something you have to instrument a build to see:
  // read it here, reboot into the UI, read it there.
  if (!strcasecmp(cmd, "mem")) {
#if defined(ESP32)
    const size_t dr_f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t dr_t = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t ps_f = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t ps_t = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t ps_b = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    consolePrintf("DRAM  %u / %u KB free", (unsigned)(dr_f / 1024), (unsigned)(dr_t / 1024));
    consolePrintf("PSRAM %u / %u KB free", (unsigned)(ps_f / 1024), (unsigned)(ps_t / 1024));
    consolePrintf("PSRAM largest block %u KB", (unsigned)(ps_b / 1024));
#else
    consoleWriteLine("not available on this build");
#endif
    return;
  }
  if (!strcasecmp(cmd, "contacts")) { cmdContacts(); return; }
  if (!strcasecmp(cmd, "chans") || !strcasecmp(cmd, "channels")) { cmdChannels(); return; }
  if (!strncasecmp(cmd, "to ", 3)) {
    snprintf(s_to, sizeof s_to, "%s", cmd + 3);
    consolePrintf("sending to: %s", s_to);
    return;
  }
  if (!strncasecmp(cmd, "msg ", 4)) {
    if (!s_to[0]) { consoleWriteLine("no recipient - use 'to <name>' first"); return; }
    const char* text = cmd + 4;
    // Try a channel first, then a contact. Channels and contacts share a name
    // space here on purpose: the user typed a name, not a category.
    if (luaHostMeshSendChannel(s_to, text)) { consolePrintf("sent to #%s", s_to); return; }
    bool was_room = false;
    if (luaHostMeshSendDM(s_to, text, &was_room)) {
      consolePrintf("sent to %s%s", s_to, was_room ? " (room)" : "");
      return;
    }
    consolePrintf("no channel or contact named '%s'", s_to);
    return;
  }
  the_mesh.runLocalCli(cmd);
}

}  // namespace

// ---- public -----------------------------------------------------------------
void consoleBegin(DisplayDriver* d) {
  s_disp = d;
  if (!s_disp) return;
  if (!s_ring) {
    const size_t bytes = (size_t)kMaxLines * (kLineCap + 1);
#if defined(ESP32)
    s_ring = (char*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!s_ring) s_ring = (char*)malloc(bytes);
    if (!s_ring) return;                      // no scrollback, no console
    memset(s_ring, 0, bytes);
  }
  s_active = true;
  measure();
  s_dirty = true;
  render();
}

void consoleEnd() { s_active = false; }
bool consoleActive() { return s_active; }

void consoleWriteLine(const char* line) {
  if (!s_ring) return;
  if (!line) { ringPush("", 0); return; }
  const int width = s_cols > 0 ? s_cols : 40;

  // Split on embedded newlines FIRST. The node CLI hands its reply to the sink
  // as one buffer containing '\n' (its help text is a dozen lines in a single
  // string), and storing that as one ring entry meant print() rendered the
  // breaks itself while our y-cursor still advanced by one row. Every later
  // line then landed on top of the one before it, which is the overlap in the
  // report, worsening down the screen as the error accumulated.
  //
  // Then wrap each piece to the panel width, rather than truncating: a reply
  // that runs off the edge is the same as no reply on a screen this size. This
  // also guarantees no stored line can be wider than the panel, so the text
  // renderer never wraps one on its own and desynchronises the cursor again.
  int pushed = 0;
  const char* seg = line;
  while (seg && pushed < 512) {                 // bound: a runaway reply cannot spin here
    const char* nl = strchr(seg, '\n');
    int seg_len = nl ? (int)(nl - seg) : (int)strlen(seg);
    // Strip a trailing CR so CRLF output does not leave a stray glyph.
    if (seg_len > 0 && seg[seg_len - 1] == '\r') seg_len--;
    if (seg_len == 0) {
      ringPush("", 0); pushed++;
    } else {
      for (int off = 0; off < seg_len && pushed < 512; off += width) {
        int n = seg_len - off;
        if (n > width) n = width;
        ringPush(seg + off, n); pushed++;
      }
    }
    if (!nl) break;
    seg = nl + 1;
  }
}

void consolePrintf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  consoleWriteLine(buf);
}

bool consoleKey(int c) {
  if (!s_active) return false;
  if (c == '\r' || c == '\n') { submit(); s_dirty = true; return true; }
  if (c == '\b' || c == 127) {
    if (s_input_len > 0) { s_input[--s_input_len] = '\0'; s_dirty_in = true; }
    return true;
  }
  if (c < 32 || c > 126) return false;
  if (s_input_len < kInputCap - 1) {
    s_input[s_input_len++] = (char)c;
    s_input[s_input_len] = '\0';
    s_dirty_in = true;       // only the input row repaints; no full-screen flash
  }
  return true;
}

void consoleLoop() {
  if (!s_active || !s_disp) return;
#if CAP_TOUCH && !CAP_KEYBOARD
  touchTick();
#endif
  const uint32_t now = millis();
  bool blink_flip = false;
  if (now - s_blink_ms >= 530) {
    s_blink_ms = now;
    s_blink_on = !s_blink_on;
    blink_flip = true;
  }
  // Cheapest repaint that covers what actually changed. An idle console paints
  // one character cell every half second; typing repaints one row; only new
  // output clears the screen.
  if (s_dirty)          { render(); s_dirty_in = false; }
  else if (s_dirty_in)  { drawInputLine(s_blink_on); s_dirty_in = false; }
  else if (blink_flip)  { drawCursorOnly(s_blink_on); }
}

#endif  // CAP_CONSOLE
