// SPDX-License-Identifier: GPL-3.0-or-later
// The host keyboard as the UI's external keyboard. The web board declares HAS_CARDKB
// (an ASCII keyboard that can come and go), so the touch UI's own external-keyboard
// path does the work: typing into the bound field, Enter to send, Esc = Back, arrows
// drive the focus group (CAP_KEYPAD_NAV). It counts as "connected" from the first
// physical key press, so a touch-only visitor keeps the on-screen keyboard.
// Text in any script (and emoji) goes through as WADA_KEY_TEXT | code point; on a phone
// the page forwards its own keyboard's text the same way (app.js "phone keyboard").
#include "helpers/input/CardKbKeyboard.h"
#include "WebInput.h"
#include <Arduino.h>

namespace {
int s_ring[256];   // a phone keyboard's voice typing or a paste arrives in bursts
uint16_t s_head = 0, s_tail = 0;
bool s_present = false;
bool s_native = false;
}
void webKeyboardPresent(bool on) { s_present = on; s_native = on; }
bool boardNativeKeyboard() { return s_native; }
void webKeyPush(int code) {
  s_present = true;
  const uint16_t next = (uint16_t)((s_head + 1) & 255u);
  if (next != s_tail) { s_ring[s_head] = code; s_head = next; }
}
void webTextPush(const char* p) {
  const uint8_t* u = (const uint8_t*)p;
  while (u && *u) {
    uint32_t cp; int n;
    if (*u < 0x80) { cp = *u; n = 1; }
    else if ((*u & 0xE0) == 0xC0) { cp = *u & 0x1F; n = 2; }
    else if ((*u & 0xF0) == 0xE0) { cp = *u & 0x0F; n = 3; }
    else if ((*u & 0xF8) == 0xF0) { cp = *u & 0x07; n = 4; }
    else { ++u; continue; }
    int i = 1;
    for (; i < n && (u[i] & 0xC0) == 0x80; ++i) cp = (cp << 6) | (u[i] & 0x3F);
    u += i;
    if (i != n || cp < 0x20 || cp == 0x7F) continue;
    webKeyPush(cp < 0x7F ? (int)cp : (WADA_KEY_TEXT | (int)cp));   // ASCII stays a CardKB code (hotkeys)
  }
}
void cardKbPoll() {}
int cardKbReadKey() {
  if (s_tail == s_head) return 0;
  const int k = s_ring[s_tail];
  s_tail = (uint16_t)((s_tail + 1) & 255u);
  return k;
}
bool cardKbPresent() { return s_present; }
