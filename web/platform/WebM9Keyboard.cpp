// SPDX-License-Identifier: GPL-3.0-or-later
// The ThinkNode M9 keyboard controller for the M9 flavour of the host build
// (cmake -DWADA_HOST_BOARD=m9). The controller hands the UI one byte per key: printable
// ASCII, or an M9_KEY_* sentinel for the d-pad and the function keys. The host keys
// arrive through the same ring the web board's CardKB path reads (WebKeyboard.cpp):
// Esc stands in for the BACK key, the arrows and Enter are already the M9's own bytes,
// and a script sends any other sentinel with `key 0x82` (HOME), `key 0xA3` (held OK), ...
#if defined(HAS_M9_KEYBOARD)
#include <M9Keyboard.h>
int cardKbReadKey();   // WebKeyboard.cpp: the host key ring

void m9KeyboardBegin() {}
void m9KeyboardPoll() {}
int m9KeyboardReadKey() {
  const int k = cardKbReadKey();
  if (k == 0x1B) return M9_KEY_HW_BACK;
  return k & 0xFF;   // the controller only ever sends one byte
}
bool m9KeyboardSetBacklight(uint8_t) { return true; }
uint8_t m9KeyboardAddr() { return 0x6C; }
void m9KeyboardSetFoundCallback(void (*cb)(uint8_t)) { if (cb) cb(0x6C); }
#endif
