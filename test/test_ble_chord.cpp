// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the Bluetooth keyboard's Ctrl+Shift layout chord: a clean tap
// fires once, a chord with a letter is a shortcut, and holding it does not race
// through the layouts. Build from the repo root:
//
//   g++ -std=c++17 -I src test/test_ble_chord.cpp -o /tmp/test_ble_chord && /tmp/test_ble_chord
#include <assert.h>
#include <stdio.h>

#include "../src/ui-touch/BleKeyboardChord.h"

int main() {
  BleCtrlShiftToggle t;

  // Ctrl alone, then Shift joins: armed but not fired.
  assert(!t.update(true, false, false, false, false));
  assert(!t.update(true, true, false, false, false));

  // Release one modifier: exactly one switch.
  assert(t.update(false, true, false, false, false));

  // Holding/repeating the same state does not fire again.
  assert(!t.update(false, true, false, false, false));
  assert(!t.update(false, false, false, false, false));

  // Both up resets the latch, so the next tap works again.
  assert(!t.update(true, false, false, false, false));
  assert(!t.update(true, true, false, false, false));
  assert(t.update(true, false, false, false, false));
  assert(!t.update(false, false, false, false, false));

  // Ctrl+Shift+C is a shortcut, not a layout switch.
  BleCtrlShiftToggle c;
  assert(!c.update(true, true, false, false, false));   // both down
  assert(!c.update(true, true, false, false, true));    // a letter joins
  assert(!c.update(true, true, false, false, true));    // still held
  assert(!c.update(false, true, false, false, false));  // released: no switch

  // AltGr is Ctrl+Alt, not Ctrl+Shift.
  BleCtrlShiftToggle a;
  assert(!a.update(true, false, true, false, false));
  assert(!a.update(true, true, true, false, false));
  assert(!a.update(true, false, true, false, false));

  // A letter already held when Ctrl+Shift goes down is not a clean chord.
  BleCtrlShiftToggle held;
  assert(!held.update(true, true, false, false, true));
  assert(!held.update(false, false, false, false, false));

  // reset() drops a half-finished chord (disconnect, key capture).
  BleCtrlShiftToggle r;
  assert(!r.update(true, true, false, false, false));
  r.reset();
  assert(!r.update(false, false, false, false, false));

  printf("test_ble_chord: ok\n");
  return 0;
}
