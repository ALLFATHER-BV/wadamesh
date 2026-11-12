// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Tap-vs-shortcut state for the Bluetooth keyboard's layout switch.
//
// Ctrl+Shift is only a layout switch when it is pressed and released on its
// own. If a letter goes down while both are held (Ctrl+Shift+C) it is a
// shortcut, so the chord is disarmed and nothing is typed or switched.
// The switch fires on release, once per chord, so holding the keys does not
// race through every layout.

#include <stdint.h>

struct BleCtrlShiftToggle {
  bool down = false;    // both modifiers were down in the previous report
  bool armed = false;   // both went down with no other key
  bool fired = false;   // this chord already switched; wait for both to lift

  // Returns true exactly once for a clean Ctrl+Shift tap.
  bool update(bool ctrl, bool shift, bool alt, bool gui, bool any_key) {
    const bool chord = ctrl && shift && !alt && !gui;
    bool fire = false;
    if (chord && !down) armed = !any_key;
    if (!chord && down && armed && !fired) {
      fire = true;
      fired = true;
    }
    if (any_key) armed = false;
    if (!ctrl && !shift) fired = false;
    down = chord;
    return fire;
  }

  void reset() { down = armed = fired = false; }
};
