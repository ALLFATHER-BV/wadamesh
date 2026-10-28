// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the USB Files screen's repaint pacing: src/ui-touch/UsbFilesPaint.h,
// what usbFilesRefresh does every 300 ms tick. An LCD repaints every tick, as before;
// e-paper (T-Deck Pro, Max) has to keep the panel quiet while a transfer runs.
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_usb_files_paint.cpp \
//       -o /tmp/test_usb_files_paint && /tmp/test_usb_files_paint
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string>

#include "ui-touch/UsbFilesPaint.h"

using namespace UsbFilesPaint;

// One upload as the session reports it on every 300 ms tick: `bytes` arriving at `bps`,
// with the page connected before and after. Returns how many times the screen repainted
// while the bytes were moving, and checks what the last frame showed.
struct Run { int paints_moving = 0; int paints_total = 0; int8_t last_bar = -2; std::string last_text; };
static Run upload(bool epaper, uint32_t bytes, uint32_t bps) {
  Run r;
  Shown shown;
  const uint32_t t_start = 3000;                                   // connected for 3 s first
  const uint32_t t_end = t_start + (uint32_t)((uint64_t)bytes * 1000u / bps);
  for (uint32_t now = 0; now <= t_end + 3000; now += 300) {
    const bool moving = now >= t_start && now < t_end;
    const uint32_t done = moving ? (uint32_t)((uint64_t)(now - t_start) * bps / 1000u) : 0;
    std::string text = moving ? "Receiving app.lua"
                     : now < t_start ? "Connected to the computer" : "Received app.lua";
    const int8_t bar = barValue(epaper, moving, done, bytes);
    if (shouldPaint(epaper, moving, text.c_str(), bar, now, shown)) {
      ++r.paints_total;
      if (moving) ++r.paints_moving;
      r.last_bar = bar;
      r.last_text = text;
    }
  }
  return r;
}

int main() {
  // ---- the bar's value ----
  assert(barValue(false, false, 10, 100) == -1);   // not moving: hidden
  assert(barValue(true, true, 10, 0) == -1);       // unknown size: hidden
  assert(barValue(false, true, 37, 100) == 37);    // LCD: every percent
  assert(barValue(true, true, 37, 100) == 25);     // e-paper: quarters
  assert(barValue(true, true, 24, 100) == 0);
  assert(barValue(true, true, 99, 100) == 75);
  assert(barValue(true, true, 100, 100) == 100);
  assert(barValue(true, true, 150, 100) == 100);   // never past full
  assert(barValue(true, true, 3000000000u, 4000000000u) == 75);   // no 32-bit overflow

  // ---- an LCD repaints every tick, exactly as before ----
  {
    const Run r = upload(false, 1u << 20, 120u * 1024u);   // 1 MB at the measured ~120 KB/s
    assert(r.paints_total > 30);
    assert(r.last_text == "Received app.lua" && r.last_bar == -1);
  }

  // ---- e-paper: a handful of panel updates per file, the end shown at once ----
  {
    const Run r = upload(true, 1u << 20, 120u * 1024u);   // ~8.5 s of transfer
    // The first frame of the transfer, then at most one per kEpaperMinGapMs.
    assert(r.paints_moving >= 1 && r.paints_moving <= (int)(1 + 8500 / kEpaperMinGapMs + 1));
    // Connected, the transfer frames, and "Received": nothing in between.
    assert(r.paints_total == r.paints_moving + 2);
    assert(r.last_text == "Received app.lua" && r.last_bar == -1);
  }
  {
    // A 40 MB map pack over 6 minutes: the bar moves in quarters, so four updates
    // plus the first frame, not one per 3 s.
    const Run r = upload(true, 40u << 20, 120u * 1024u);
    assert(r.paints_moving <= 5);
    assert(r.last_text == "Received app.lua");
  }
  {
    // A small .lua: under a second of transfer, one frame for it.
    const Run r = upload(true, 6 * 1024, 120u * 1024u);
    assert(r.paints_moving <= 1);
    assert(r.paints_total <= 3);
  }

  // ---- unchanged content never repaints e-paper; a change outside a transfer does ----
  {
    Shown s;
    assert(shouldPaint(true, false, "Waiting for the computer", -1, 0, s));
    assert(!shouldPaint(true, false, "Waiting for the computer", -1, 300, s));
    assert(!shouldPaint(true, false, "Waiting for the computer", -1, 60000, s));
    assert(shouldPaint(true, false, "Connected to the computer", -1, 60300, s));   // at once
  }
  {
    // A stopped upload is shown at once, even right after a progress frame.
    Shown s;
    assert(shouldPaint(true, true, "Receiving a.bin", 0, 1000, s));
    assert(!shouldPaint(true, true, "Receiving a.bin", 25, 1300, s));   // inside the gap
    assert(shouldPaint(true, false, "Upload of a.bin stopped", -1, 1600, s));
  }

  printf("test_usb_files_paint: all passed\n");
  return 0;
}
