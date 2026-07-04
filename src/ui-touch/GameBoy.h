// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Native Game Boy / Game Boy Color player built on the gnuboy core vendored
// under ui-touch/gnuboy/ (GPL-2.0-or-later; folds forward into wadamesh's GPL-3).
//
// A full-screen overlay on lv_layer_top, single instance (one game at a time):
//   1. ROM picker — lists *.gb / *.gbc from microSD (/meshcomod/gb, legacy /gb).
//   2. Play — the 160x144 RGB565 frame renders into an LVGL canvas (1x with
//      on-screen controls, or aspect-scaled full-height in immersive mode); an
//      lv_timer paces gnuboy_run() to ~59.73 Hz (the outer loop() keeps
//      servicing the mesh between ticks). Input: on-screen touch pad, plus the
//      trackball (D-pad + click=A) and keyboard (WASD + keys). Audio is muted by
//      default (toggle in-game); battery saves live next to the ROM as <name>.sav.
//      T-Deck only (needs SD + input); the V4 build stubs it out.
class GameBoy {
public:
  static void launch();               // open the picker (no-op if already open)
  static bool isOpen();               // UITask: gate tab bar + trackball nav
  static void steer(int dx, int dy);  // UITask: trackball motion -> D-pad pulse
  static void setA(bool down);        // UITask: trackball click -> A (held)
  static void keyChar(char c);        // UITask: physical keyboard -> GB pad pulse
  static void close();                // tear down, save, return to UI
};
