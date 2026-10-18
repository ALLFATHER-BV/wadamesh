// SPDX-License-Identifier: GPL-3.0-or-later
// The host keyboard as the UI's external keyboard. The web board declares HAS_CARDKB
// (an ASCII keyboard that can come and go), so the touch UI's own external-keyboard
// path does the work: typing into the bound field, Enter to send, Esc = Back, arrows
// drive the focus group (CAP_KEYPAD_NAV). It counts as "connected" from the first
// physical key press, so a touch-only visitor keeps the on-screen keyboard.
// Limitation of that path: ASCII only (CardKB codes); other scripts use the
// on-screen keyboard.
#include "helpers/input/CardKbKeyboard.h"
#include "WebInput.h"
#include <Arduino.h>

namespace {
uint8_t s_ring[32];
uint8_t s_head = 0, s_tail = 0;
bool s_present = false;
}
void webKeyPush(uint8_t code) {
  s_present = true;
  const uint8_t next = (uint8_t)((s_head + 1) & 31u);
  if (next != s_tail) { s_ring[s_head] = code; s_head = next; }
}
void cardKbPoll() {}
int cardKbReadKey() {
  if (s_tail == s_head) return 0;
  const uint8_t k = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) & 31u);
  return k;
}
bool cardKbPresent() { return s_present; }
