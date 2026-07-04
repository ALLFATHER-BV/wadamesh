// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Native Game Boy / Game Boy Color player built on the gnuboy core vendored
// under ui-touch/gnuboy/ (GPL-2.0-or-later; folds forward into wadamesh's GPL-3).
//
// A full-screen overlay on lv_layer_top, single instance (one game at a time):
//   1. ROM picker — lists *.gb / *.gbc from microSD (/meshcomod/gb, legacy /gb).
//   2. Play — the 160x144 RGB565 frame renders 1x into an LVGL canvas; an
//      lv_timer paces gnuboy_run() to ~59.73 Hz (the outer loop() keeps
//      servicing the mesh between ticks, so the companion link stays up);
//      on-screen touch buttons drive the GB pad (the T-Deck keyboard/trackball
//      report no key-release, so they can't hold a D-pad direction). Battery
//      saves live next to the ROM as <name>.sav. Audio is muted in this first
//      cut. T-Deck only (needs SD + touch controls); the V4 build stubs it out.
class GameBoy {
public:
  static void launch();               // open the picker (no-op if already open)
  static bool isOpen();               // UITask: gate tab bar + trackball nav
  static void steer(int dx, int dy);  // UITask: trackball motion -> D-pad pulse
  static void close();                // tear down, save, return to UI
};
