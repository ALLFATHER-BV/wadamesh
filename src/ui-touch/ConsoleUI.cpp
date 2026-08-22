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
bool     s_dirty      = true;
bool     s_active     = false;

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

// i = 0 is the OLDEST retained line.
const char* ringGet(int i) {
  if (i < 0 || i >= s_count) return nullptr;
  const int start = (s_head - s_count + kMaxLines * 2) % kMaxLines;
  return lineAt((start + i) % kMaxLines);
}

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

  // Input line, pinned to the bottom with a blinking block cursor.
  const int iy = s_disp->height() - s_line_h;
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
  if (s_blink_on) {
    const int cx = s_char_w * 2 + s_disp->getTextWidth(shown);
    s_disp->fillRect(cx, iy, s_char_w, s_line_h);
  }

  // Scrollback indicator: without it there is no way to tell you are not live.
  if (s_scroll > 0) {
    char tag[24];
    snprintf(tag, sizeof tag, "-%d", s_scroll);
    s_disp->setColor(UIColor::warning_txt);
    s_disp->drawTextRightAlign(s_disp->width() - 2, iy, tag);
  }

  s_disp->endFrame();
  s_dirty = false;
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
  if (!strcasecmp(cmd, "help")) {
    consoleWriteLine("console: clear, help, ui");
    consoleWriteLine("node: everything the CLI answers, e.g.");
    consoleWriteLine("  advert, get name, set name <x>, time, ver");
    return;
  }
  the_mesh.runLocalCli(cmd);
}

#if CAP_TOUCH
// ---- on-screen keypad -------------------------------------------------------
// Touch boards have no hardware keyboard and the firmware's on-screen one is
// LVGL, so console mode draws its own. Deliberate: the V4 is the board this
// feature exists for, so it has to be usable there.
//
// Phase 1 ships the tap plumbing and a scroll/exit strip; the full key grid is
// the next step and is why kKeypadRows is a table rather than inline code.
uint32_t s_touch_start = 0;
uint16_t s_touch_x = 0, s_touch_y = 0;

void touchTick() {
  uint16_t tx, ty;
  const bool pressed = heltecV4CapTouchGetLive(&tx, &ty);
  const uint32_t now = millis();
  if (pressed && !s_touch_start) {
    s_touch_start = now;
    s_touch_x = tx; s_touch_y = ty;
  } else if (!pressed && s_touch_start) {
    const uint32_t held = now - s_touch_start;
    s_touch_start = 0;
    if (!s_disp) return;
    // Top third scrolls back, bottom third scrolls forward. A tap in the middle
    // is reserved for the key grid.
    const int h = s_disp->height();
    if (held < 700) {
      if (s_touch_y < h / 3 && s_scroll < s_count - s_rows) { s_scroll++; s_dirty = true; }
      else if (s_touch_y > (2 * h) / 3 && s_scroll > 0)     { s_scroll--; s_dirty = true; }
    }
  }
}
#endif  // CAP_TOUCH

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
  // Wrap to the panel width rather than truncating: a command reply that runs
  // off the edge is the same as no reply at all on a screen this size.
  const int width = s_cols > 0 ? s_cols : 40;
  const int len = (int)strlen(line);
  if (len == 0) { ringPush("", 0); return; }
  for (int off = 0; off < len; off += width) {
    int n = len - off;
    if (n > width) n = width;
    ringPush(line + off, n);
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
    if (s_input_len > 0) { s_input[--s_input_len] = '\0'; s_dirty = true; }
    return true;
  }
  if (c < 32 || c > 126) return false;
  if (s_input_len < kInputCap - 1) {
    s_input[s_input_len++] = (char)c;
    s_input[s_input_len] = '\0';
    s_dirty = true;
  }
  return true;
}

void consoleLoop() {
  if (!s_active || !s_disp) return;
#if CAP_TOUCH
  touchTick();
#endif
  const uint32_t now = millis();
  if (now - s_blink_ms >= 500) {
    s_blink_ms = now;
    s_blink_on = !s_blink_on;
    s_dirty = true;
  }
  // Redraw only when something changed. This is the whole point of the mode:
  // an idle console costs a millis() comparison, not a render pass.
  if (s_dirty) render();
}

#endif  // CAP_CONSOLE
