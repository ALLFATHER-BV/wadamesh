// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// =============================================================================
// When the USB Files screen repaints its status line and progress bar
// (usbFilesRefresh in UITask.cpp). Pure, so the host test
// (test/test_usb_files_paint.cpp) runs the same rule the board does.
//
// An LCD repaints every 300 ms tick, as it always has. On e-paper (CAP_EPAPER)
// every change is a panel update that blocks the loop for ~0.65 s, and the loop
// is also what serves the transfer (UsbFilesSession::tick), so a bar creeping
// forward 1% at a time would spend the transfer refreshing the glass and wear it.
// There the bar moves in quarters, nothing repaints more than once every
// kEpaperMinGapMs while bytes are moving, and a change of state (connected,
// done, stopped) is shown at once.
// =============================================================================

#include <stdint.h>

namespace UsbFilesPaint {

static constexpr uint32_t kEpaperMinGapMs = 3000;
static constexpr uint8_t  kEpaperBarStep  = 25;   // percent

// The bar's value as shown: 0..100, or -1 when the bar is hidden.
inline int8_t barValue(bool epaper, bool moving, uint32_t done, uint32_t total) {
  if (!moving || total == 0) return -1;
  uint32_t pct = (uint32_t)((uint64_t)(done > total ? total : done) * 100u / total);
  if (epaper) pct -= pct % kEpaperBarStep;
  return (int8_t)pct;
}

// What was last put on screen.
struct Shown {
  bool     valid = false;
  uint32_t text_hash = 0;
  int8_t   bar = -1;
  uint32_t at_ms = 0;
};

inline uint32_t hashText(const char* s) {   // FNV-1a: "did the line change", not security
  uint32_t h = 2166136261u;
  for (; s && *s; ++s) { h ^= (uint8_t)*s; h *= 16777619u; }
  return h;
}

// Whether to repaint now. On true, `shown` records what is being painted.
inline bool shouldPaint(bool epaper, bool moving, const char* text, int8_t bar,
                        uint32_t now, Shown& shown) {
  const uint32_t h = hashText(text);
  if (!epaper) {
    shown.valid = true; shown.text_hash = h; shown.bar = bar; shown.at_ms = now;
    return true;
  }
  const bool changed = !shown.valid || h != shown.text_hash || bar != shown.bar;
  if (!changed) return false;
  // While a file is moving, one panel update per gap at most; the first frame of a
  // transfer and every state change outside one are shown straight away.
  const bool was_moving = shown.valid && shown.bar >= 0;
  if (moving && was_moving && (uint32_t)(now - shown.at_ms) < kEpaperMinGapMs) return false;
  shown.valid = true; shown.text_hash = h; shown.bar = bar; shown.at_ms = now;
  return true;
}

}  // namespace UsbFilesPaint
