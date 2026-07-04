// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Native Game Boy / Game Boy Color player built on the gnuboy core vendored
// under ui-touch/gnuboy/ (GPL-2.0-or-later; folds forward into wadamesh's GPL-3).
//
// SCAFFOLD — step 1 of the port. This is the class shell + build wiring only: it
// pulls the gnuboy core into the firmware and exposes the SnakeGame-shaped
// surface that UITask will later route trackball/keyboard input and the Apps
// drawer into. No ROM is loaded and no frames run yet — launch() is a no-op.
//
// Design target (see feasibility write-up): a full-screen "game mode" takeover
// on T-Deck — gnuboy_run() paced to ~59.73 Hz with the_mesh.loop() pumped in
// each frame's slack (single-thread, no SPI-bus contention), the 160x144 RGB565
// framebuffer nearest-scaled to 240x216 into an LVGL canvas, trackball + keyboard
// mapped to the GB pad, and ROM + battery saves on microSD under /meshcomod/gb/.
// T-Deck only (needs SD + gamepad-usable input); on the V4 the shell is a stub.
class GameBoy {
public:
  static void launch();               // open the player (SCAFFOLD: no-op)
  static bool isOpen();               // UITask: gate trackball + tab bar
  static void steer(int dx, int dy);  // UITask: trackball motion -> GB dpad
  static void close();                // tear down + return to UI

private:
  static bool s_open;
};
