// SPDX-License-Identifier: GPL-3.0-or-later
// T-LoRa Pager simulator: the Pager's keyboard and rotary encoder on the host keyboard,
// and the few board functions the UI calls by name.
//
//   Up / Down   turn the knob (one detent each)
//   Right       click the knob (short press)
//   Left / Esc  hold the knob ~1.2 s: Back (as on the device; Backspace still deletes in a field)
//   Enter       the keyboard's Enter      Backspace  Backspace
//   letters     type, as on the Pager's QWERTY (M / C / H / A / S jump tabs)
#include <Arduino.h>
#include <SPI.h>
#include <time.h>
#include "SimPagerBoard.h"
#include "helpers/input/PagerKeyboard.h"
#include "helpers/input/PagerEncoder.h"

namespace {
struct SimSoftClock : mesh::RTCClock {   // what the Pager's PCF85063A falls back to
  uint32_t getCurrentTime() override { return (uint32_t)time(nullptr); }
  void setCurrentTime(uint32_t) override {}
} s_soft_clock;
}
HardwareRtcClock hw_rtc(s_soft_clock);   // never begun: reads "no chip" in About
SPIClass* tloraPagerSharedSPI() { return nullptr; }
void tloraPagerGpsPowerDown() {}

int cardKbReadKey();   // web/platform/WebKeyboard.cpp: the host keyboard's queue

namespace {
int s_ring[64];
uint8_t s_head = 0, s_tail = 0;
int s_enc_delta = 0;
int s_click_frames = 0;   // > 0: the knob reads as pressed for this many more polls
uint32_t s_hold_until = 0; // the knob reads as pressed until then (the Back hold)
uint32_t s_letter_until = 0; // the last letter's key reads as held until then (holdkey)

void drainHost() {
  for (int k; (k = cardKbReadKey()) != 0;) {
    if (k == 0xB5) { s_enc_delta--; continue; }          // Up: turn back
    if (k == 0xB6) { s_enc_delta++; continue; }          // Down: turn forward
    if (k == 0xB7) { s_click_frames = 3; continue; }     // Right: click
    if (k == 0xB4 || k == 0x1B) { s_hold_until = millis() + 1200; continue; }   // Left / Esc: hold = Back
    const uint8_t next = (uint8_t)((s_head + 1) & 63u);
    if (next != s_tail) { s_ring[s_head] = k; s_head = next; }
  }
}
}  // namespace

void pagerKeyboardBegin() {}
void pagerKeyboardPoll() { drainHost(); }
int pagerKeyboardReadKey() {
  drainHost();
  if (s_tail == s_head) return 0;
  const int k = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) & 63u);
  return k;
}
void pagerKeyboardSetBacklight(uint8_t) {}
static uint32_t s_shift_until = 0, s_fn_until = 0;   // "holdkey shift MS" / "holdkey fn MS"
bool pagerKeyboardAltHeld() { return s_fn_until && (int32_t)(millis() - s_fn_until) < 0; }
void pagerKeyboardMarkAltUsed() {}
void pagerKeyboardDiscardAlt() {}
bool pagerKeyboardBackspaceHeld() { return false; }
static uint32_t s_space_until = 0;   // "holdkey space MS": the bar held, nothing typed
bool pagerKeyboardSpaceHeld() { return s_space_until && (int32_t)(millis() - s_space_until) < 0; }
bool pagerKeyboardLetterHeld() { return s_letter_until && (int32_t)(millis() - s_letter_until) < 0; }
bool pagerKeyboardShiftHeld() { return s_shift_until && (int32_t)(millis() - s_shift_until) < 0; }
void simPagerHoldKey(uint8_t c, uint32_t ms) {   // script: type c and hold its key for ms
  if (c == ' ') { s_space_until = millis() + ms; return; }
  if (c == 0x01) { s_shift_until = millis() + ms; return; }
  if (c == 0x02) { s_fn_until = millis() + ms; return; }
  const uint8_t next = (uint8_t)((s_head + 1) & 63u);
  if (next != s_tail) { s_ring[s_head] = c; s_head = next; }
  s_letter_until = millis() + ms;
}
bool pagerKeyboardConsumeAltShiftChord() { return false; }
void pagerKeyboardToggleCaps() {}
bool pagerKeyboardConsumeAltBackspaceChord() { return false; }
bool pagerKeyboardConsumeAltEnterChord() { return false; }
bool pagerKeyboardConsumeAltBChord() { return false; }
bool pagerKeyboardConsumeBothShiftChord() { return false; }

void pagerEncoderBegin() {}
int pagerEncoderReadDelta() {
  drainHost();
  const int d = s_enc_delta;
  s_enc_delta = 0;
  return d;
}
bool pagerEncoderClickHeld() {
  drainHost();
  if (s_hold_until && (int32_t)(millis() - s_hold_until) < 0) return true;
  s_hold_until = 0;
  if (s_click_frames > 0) { --s_click_frames; return true; }
  return false;
}

// The SD-migration hooks main.cpp provides on the device: no card in the simulator.
bool g_sd_migration_blocked = false;
bool g_full_data_on_sd = false;
bool g_contacts_on_sd = false;
bool meshcomodMigrateSpiffsToSd(bool) { return false; }
bool meshcomodPrepareSdMigration() { return false; }
bool meshcomodSdProfileMatchesInternal() { return false; }
bool meshcomodArmSdMigLatch() { return false; }
void meshcomodClearSdMigLatch() {}
