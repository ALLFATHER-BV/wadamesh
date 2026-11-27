// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for src/ui-touch/PaperTapHold.h: when the e-paper boards hold a
// frame back instead of committing it to the panel (T-Deck Pro and Max).
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_paper_tap_hold.cpp \
//       -o /tmp/test_paper_tap_hold && /tmp/test_paper_tap_hold
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "ui-touch/PaperTapHold.h"

using namespace PaperTapHold;

int main() {
  // ---- nothing pressed, nothing undrawn: never holds ----
  {
    State s;
    for (uint32_t now = 0; now < 2000; now += 15) assert(!hold(s, now, 0, false));
  }

  // ---- a tap: held while pressed, free as soon as LVGL has released and drawn ----
  {
    State s;
    uint32_t now = 10000;
    for (uint32_t pressed = 15; pressed <= 120; pressed += 15, now += 15) assert(hold(s, now, pressed, false));
    assert(!hold(s, now, 0, false));
  }

  // ---- the screen a tap opens is not drawn yet on the pass that released ----
  {
    State s;
    assert(hold(s, 1000, 20, true));     // pressed look invalid, pointer pressed
    assert(hold(s, 1015, 0, true));      // released, the new screen still undrawn
    assert(!hold(s, 1030, 0, false));    // drawn: commit
  }

  // ---- a long hold shows its pressed look once the limit passes ----
  {
    State s;
    assert(hold(s, 500, kPressMaxMs - 1, false));
    assert(!hold(s, 501, kPressMaxMs, false));
    assert(!hold(s, 1000, kPressMaxMs + 499, false));
    assert(!hold(s, 1015, 0, false));
  }

  // ---- constant redrawing cannot starve the panel, and re-arms once drawn ----
  {
    State s;
    uint32_t now = 7000;
    for (; now < 7000 + kUndrawnMaxMs; now += 15) assert(hold(s, now, 0, true));
    for (uint32_t i = 0; i < 100; ++i, now += 15) assert(!hold(s, now, 0, true));
    assert(!hold(s, now, 0, false));        // LVGL caught up
    assert(hold(s, now + 15, 0, true));     // a new change is held again
  }

  // ---- a press does not reset the undrawn limit that already ran out ----
  {
    State s;
    assert(hold(s, 100, 0, true));
    assert(!hold(s, 100 + kUndrawnMaxMs, 0, true));
    assert(hold(s, 120 + kUndrawnMaxMs, 30, true));      // pressed: held for the press
    assert(!hold(s, 140 + kUndrawnMaxMs, 0, true));      // released, still the same undrawn run
  }

  // ---- works across the millis() wrap ----
  {
    State s;
    const uint32_t near = 0xFFFFFFF0u;
    assert(hold(s, near, 0, true));
    assert(hold(s, near + kUndrawnMaxMs - 1, 0, true));
    assert(!hold(s, near + kUndrawnMaxMs, 0, true));
  }

  printf("test_paper_tap_hold: all passed\n");
  return 0;
}
