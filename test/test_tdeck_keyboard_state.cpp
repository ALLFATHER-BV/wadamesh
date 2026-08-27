// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <string.h>

#include "helpers/input/TDeckKeyboardState.h"

static size_t update(TDeckKeyboardState& keyboard, uint8_t state[5], uint32_t now,
                     uint8_t* out) {
  return keyboard.update(state, now, out, 8);
}

static void setKey(uint8_t state[5], int col, int row, bool down) {
  if (down) state[col] |= (uint8_t)(1U << row);
  else      state[col] &= (uint8_t)~(1U << row);
}

int main() {
  TDeckKeyboardState keyboard;
  uint8_t state[5] = {};
  uint8_t out[8] = {};

  setKey(state, 0, 2, true);                 // hold Sym
  assert(update(keyboard, state, 10, out) == 0);
  setKey(state, 0, 0, true);                 // q -> #
  assert(update(keyboard, state, 20, out) == 1 && out[0] == '#');
  setKey(state, 0, 0, false);
  update(keyboard, state, 30, out);
  setKey(state, 0, 2, false);
  update(keyboard, state, 40, out);
  setKey(state, 0, 0, true);                 // hold use must not latch
  assert(update(keyboard, state, 50, out) == 1 && out[0] == 'q');
  setKey(state, 0, 0, false);
  update(keyboard, state, 60, out);

  setKey(state, 0, 2, true);                 // tap Sym
  update(keyboard, state, 100, out);
  setKey(state, 0, 2, false);
  update(keyboard, state, 120, out);
  setKey(state, 0, 1, true);                 // w -> 1 once
  assert(update(keyboard, state, 130, out) == 1 && out[0] == '1');
  setKey(state, 0, 1, false);
  update(keyboard, state, 140, out);
  setKey(state, 0, 1, true);
  assert(update(keyboard, state, 150, out) == 1 && out[0] == 'w');
  setKey(state, 0, 1, false);
  update(keyboard, state, 160, out);

  setKey(state, 0, 2, true);                 // an unmapped symbol still uses the hold
  update(keyboard, state, 170, out);
  setKey(state, 0, 5, true);                 // Space has no T-Deck symbol mapping
  assert(update(keyboard, state, 175, out) == 0);
  setKey(state, 0, 5, false);
  update(keyboard, state, 180, out);
  setKey(state, 0, 2, false);
  update(keyboard, state, 185, out);
  setKey(state, 0, 0, true);
  assert(update(keyboard, state, 190, out) == 1 && out[0] == 'q');
  setKey(state, 0, 0, false);
  update(keyboard, state, 195, out);

  setKey(state, 0, 4, true);                 // held Alt preserves base layer
  update(keyboard, state, 200, out);
  setKey(state, 0, 0, true);
  assert(update(keyboard, state, 210, out) == 1 && out[0] == 'q');
  setKey(state, 0, 0, false);
  update(keyboard, state, 215, out);
  setKey(state, 3, 4, true);                 // Alt+B stays controller-only
  assert(update(keyboard, state, 216, out) == 0);
  setKey(state, 3, 4, false);
  update(keyboard, state, 217, out);
  setKey(state, 2, 5, true);                 // Alt+C keeps its legacy control code
  assert(update(keyboard, state, 218, out) == 1 && out[0] == 0x0C);
  setKey(state, 2, 5, false);
  update(keyboard, state, 219, out);
  setKey(state, 0, 4, false);
  update(keyboard, state, 220, out);

  setKey(state, 0, 4, true);                 // solo Alt is a one-shot symbol latch
  update(keyboard, state, 225, out);
  setKey(state, 0, 4, false);
  update(keyboard, state, 230, out);
  setKey(state, 0, 0, true);
  assert(update(keyboard, state, 235, out) == 1 && out[0] == '#');
  setKey(state, 0, 0, false);
  update(keyboard, state, 240, out);

  setKey(state, 0, 4, true);                 // double-tap Alt locks symbols
  update(keyboard, state, 250, out);
  setKey(state, 0, 4, false);
  update(keyboard, state, 270, out);
  setKey(state, 0, 4, true);
  update(keyboard, state, 300, out);
  setKey(state, 0, 4, false);
  update(keyboard, state, 320, out);
  setKey(state, 0, 0, true);
  assert(update(keyboard, state, 330, out) == 1 && out[0] == '#');
  setKey(state, 0, 0, false);
  update(keyboard, state, 340, out);
  setKey(state, 0, 1, true);
  assert(update(keyboard, state, 350, out) == 1 && out[0] == '1');
  setKey(state, 0, 1, false);
  update(keyboard, state, 360, out);
  setKey(state, 0, 4, true);                 // tap Alt unlocks
  update(keyboard, state, 400, out);
  setKey(state, 0, 4, false);
  update(keyboard, state, 420, out);

  setKey(state, 0, 2, true);                 // double-tap Sym locks symbols
  update(keyboard, state, 430, out);
  setKey(state, 0, 2, false);
  update(keyboard, state, 440, out);
  setKey(state, 0, 2, true);
  update(keyboard, state, 450, out);
  setKey(state, 0, 2, false);
  update(keyboard, state, 460, out);
  setKey(state, 0, 1, true);
  assert(update(keyboard, state, 470, out) == 1 && out[0] == '1');
  setKey(state, 0, 1, false);
  update(keyboard, state, 475, out);
  setKey(state, 0, 2, true);
  update(keyboard, state, 480, out);
  setKey(state, 0, 2, false);
  update(keyboard, state, 490, out);

  setKey(state, 1, 6, true);                 // held Shift still uppercases base
  update(keyboard, state, 500, out);
  setKey(state, 0, 0, true);
  assert(update(keyboard, state, 510, out) == 1 && out[0] == 'Q');

  setKey(state, 0, 0, false);
  setKey(state, 1, 6, false);
  update(keyboard, state, 520, out);
  setKey(state, 0, 2, true);                 // discarded physical hold
  update(keyboard, state, 600, out);
  keyboard.discardModifiers();
  setKey(state, 0, 2, false);
  update(keyboard, state, 620, out);
  setKey(state, 0, 0, true);
  assert(update(keyboard, state, 630, out) == 1 && out[0] == 'q');
  setKey(state, 0, 0, false);
  update(keyboard, state, 640, out);

  setKey(state, 0, 4, true);                 // discarded Alt hold cannot latch on release
  update(keyboard, state, 650, out);
  keyboard.discardModifiers();
  setKey(state, 0, 4, false);
  update(keyboard, state, 660, out);
  setKey(state, 0, 0, true);
  assert(update(keyboard, state, 670, out) == 1 && out[0] == 'q');

  setKey(state, 0, 0, false);
  update(keyboard, state, 680, out);
  setKey(state, 0, 2, true);                 // modifiers disabled: base key still routes
  update(keyboard, state, 690, out);
  setKey(state, 0, 0, true);
  assert(keyboard.update(state, 700, out, 8, false) == 1 && out[0] == 'q');

  uint8_t baseline[5] = {};
  setKey(baseline, 0, 1, true);              // transition frame produces no edge later
  keyboard.baseline(baseline);
  assert(keyboard.update(baseline, 710, out, 8) == 0);
  return 0;
}