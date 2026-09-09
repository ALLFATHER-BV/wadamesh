// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>

#define HAS_TDECK_PRO 1
#include "helpers/input/PagerKeyboardState.h"

int main() {
  PagerKeyboardState keyboard;

  assert(keyboard.event(0, true, 10) == 'p');
  keyboard.event(0, false, 20);
  assert(keyboard.event(9, true, 30) == 'q');
  keyboard.event(9, false, 40);
  assert(keyboard.event(20, true, 50) == '\r');
  keyboard.event(20, false, 60);

  assert(keyboard.event(PagerKeyboardState::BACKSPACE_POS, true, 70) == '\b');
  assert(keyboard.backspaceHeld());
  keyboard.event(PagerKeyboardState::BACKSPACE_POS, false, 80);
  assert(!keyboard.backspaceHeld());

  keyboard.event(PagerKeyboardState::SHIFT_POS, true, 90);
  assert(keyboard.event(9, true, 100) == 'Q');
  keyboard.event(9, false, 110);
  keyboard.event(PagerKeyboardState::SHIFT_POS, false, 120);

  keyboard.event(PagerKeyboardState::SHIFT_POS_2, true, 130);
  assert(keyboard.event(0, true, 140) == 'P');
  keyboard.event(0, false, 150);
  keyboard.event(PagerKeyboardState::SHIFT_POS_2, false, 160);

  keyboard.event(PagerKeyboardState::SYMBOL_POS, true, 170);
  keyboard.event(PagerKeyboardState::SYMBOL_POS, false, 180);
  assert(keyboard.event(0, true, 190) == '@');
  keyboard.event(0, false, 200);

  keyboard.event(PagerKeyboardState::ALT_POS, true, 210);
  keyboard.event(PagerKeyboardState::ALT_POS, false, 220);
  assert(keyboard.event(8, true, 230) == '1');
  keyboard.event(8, false, 240);

  keyboard.event(PagerKeyboardState::SYMBOL_POS, true, 250);
  keyboard.event(PagerKeyboardState::SYMBOL_POS, false, 260);
  assert(keyboard.event(33, true, 270) == '0');
  keyboard.event(33, false, 280);

  assert(keyboard.event(PagerKeyboardState::SPACE_POS, true, 290) == ' ');
  assert(keyboard.spaceHeld());
  keyboard.event(PagerKeyboardState::SPACE_POS, false, 300);
  assert(!keyboard.spaceHeld());
  return 0;
}