// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>

#include "helpers/input/PagerKeyboardState.h"

int main() {
  PagerKeyboardState keyboard;

  assert(keyboard.event(0, true, 10) == 'q');
  keyboard.event(0, false, 20);

  keyboard.event(PagerKeyboardState::ALT_POS, true, 100);
  assert(keyboard.altHeld());
  assert(keyboard.event(0, true, 110) == '1');
  keyboard.event(0, false, 120);
  keyboard.event(PagerKeyboardState::ALT_POS, false, 130);
  assert(keyboard.event(0, true, 140) == 'q');
  keyboard.event(0, false, 150);

  keyboard.event(PagerKeyboardState::ALT_POS, true, 200);
  keyboard.event(PagerKeyboardState::ALT_POS, false, 220);
  assert(keyboard.event(1, true, 230) == '2');
  keyboard.event(1, false, 240);
  assert(keyboard.event(1, true, 250) == 'w');
  keyboard.event(1, false, 260);

  keyboard.event(PagerKeyboardState::ALT_POS, true, 300);
  keyboard.event(PagerKeyboardState::ALT_POS, false, 320);
  keyboard.event(PagerKeyboardState::ALT_POS, true, 400);
  keyboard.event(PagerKeyboardState::ALT_POS, false, 420);
  assert(keyboard.event(0, true, 430) == '1');
  keyboard.event(0, false, 440);
  assert(keyboard.event(1, true, 450) == '2');
  keyboard.event(1, false, 460);
  keyboard.event(PagerKeyboardState::ALT_POS, true, 470);
  keyboard.event(PagerKeyboardState::ALT_POS, false, 480);
  assert(keyboard.event(0, true, 490) == 'q');
  keyboard.event(0, false, 500);

  keyboard.event(PagerKeyboardState::SHIFT_POS, true, 510);
  assert(keyboard.event(0, true, 520) == 'Q');
  keyboard.event(0, false, 530);
  keyboard.event(PagerKeyboardState::SHIFT_POS, false, 540);

  keyboard.event(PagerKeyboardState::ALT_POS, true, 600);
  keyboard.event(PagerKeyboardState::SHIFT_POS, true, 610);
  assert(keyboard.consumeAltShiftChord());
  assert(!keyboard.consumeAltShiftChord());
  keyboard.toggleCaps();
  keyboard.event(PagerKeyboardState::SHIFT_POS, false, 620);
  keyboard.event(PagerKeyboardState::ALT_POS, false, 630);
  assert(keyboard.event(0, true, 640) == 'Q');
  keyboard.event(0, false, 650);
  keyboard.toggleCaps();

  keyboard.event(PagerKeyboardState::ALT_POS, true, 700);
  assert(keyboard.event(PagerKeyboardState::BACKSPACE_POS, true, 710) == 0);
  assert(keyboard.consumeAltBackspaceChord());
  assert(!keyboard.backspaceHeld());
  keyboard.event(PagerKeyboardState::BACKSPACE_POS, false, 720);
  keyboard.event(PagerKeyboardState::ALT_POS, false, 730);
  assert(keyboard.event(0, true, 740) == 'q');
  keyboard.event(0, false, 750);

  keyboard.event(PagerKeyboardState::ALT_POS, true, 800);
  keyboard.discardAlt();
  keyboard.event(PagerKeyboardState::ALT_POS, false, 810);
  assert(keyboard.event(0, true, 820) == 'q');
  keyboard.event(0, false, 830);

  keyboard.event(PagerKeyboardState::ALT_POS, true, 900);
  keyboard.event(PagerKeyboardState::ALT_POS, false, 920);
  keyboard.markAltUsed();                       // no effect without a physical hold
  assert(keyboard.event(PagerKeyboardState::SPACE_POS, true, 930) == ' ');
  assert(keyboard.spaceHeld());
  keyboard.event(PagerKeyboardState::SPACE_POS, false, 940);
  assert(!keyboard.spaceHeld());
  assert(keyboard.event(0, true, 950) == 'q');
  keyboard.event(0, false, 960);

  keyboard.event(PagerKeyboardState::ALT_POS, true, 1000);
  keyboard.markAltUsed();                       // physical Alt+encoder equivalent
  keyboard.event(PagerKeyboardState::ALT_POS, false, 1010);
  assert(keyboard.event(0, true, 1020) == 'q');
  keyboard.event(0, false, 1030);
  assert(!keyboard.letterHeld());

  // Tap or hold (the Home and Settings shortcuts): the letter's key stays held
  // until its own release, whatever else is released meanwhile.
  const uint8_t s_key = PagerKeyboardState::COLS + 1;
  assert(keyboard.event(s_key, true, 1100) == 's');
  assert(keyboard.letterHeld());
  keyboard.event(0, false, 1110);
  assert(keyboard.letterHeld());
  keyboard.event(s_key, false, 1500);
  assert(!keyboard.letterHeld());
  // A later letter takes over: the earlier key's release no longer counts.
  assert(keyboard.event(s_key, true, 1600) == 's');
  assert(keyboard.event(0, true, 1610) == 'q');
  keyboard.event(s_key, false, 1620);
  assert(keyboard.letterHeld());
  keyboard.event(0, false, 1630);
  assert(!keyboard.letterHeld());
  // Space and Backspace keep their own hold state and are not letters.
  assert(keyboard.event(PagerKeyboardState::SPACE_POS, true, 1700) == ' ');
  assert(!keyboard.letterHeld());
  keyboard.event(PagerKeyboardState::SPACE_POS, false, 1710);
  assert(keyboard.event(PagerKeyboardState::BACKSPACE_POS, true, 1720) == '\b');
  assert(!keyboard.letterHeld());
  keyboard.event(PagerKeyboardState::BACKSPACE_POS, false, 1730);
  return 0;
}