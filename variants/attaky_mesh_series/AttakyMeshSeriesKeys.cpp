// SPDX-License-Identifier: GPL-3.0-or-later
#include "AttakyMeshSeriesKeys.h"

#if defined(ATTAKY_MESH_SERIES) && defined(ESP32)

#include <Arduino.h>
#include <Wire.h>
#include "AttakySharedI2C.h"

static const uint8_t KEYS_ADDR      = 0x59;

static const uint8_t AW_REG_IN_P0   = 0x00;
static const uint8_t AW_REG_CFG_P0  = 0x04;
static const uint8_t AW_REG_MODE_P0 = 0x12;   // 0x12 = P0 LED-mode select (0x13 is P1)

static const uint8_t POWER_BIT      = (1u << 7);   // P07 = POWER_BTN

// Debounce: the buttons sit behind a shared-bus I2C expander, so require a
// stable read across several polls before a press counts.
static const uint8_t  DEBOUNCE_POLLS = 2;
static const uint32_t POLL_INTERVAL_MS = 30;

// Auto-repeat for the four directions (never SELECT — one press must stay one
// activation). Without it, walking a long contact list means one press per row.
static const uint32_t REPEAT_DELAY_MS = 450;
static const uint32_t REPEAT_RATE_MS  = 130;

// P0 bit -> nav event. 0 = not a nav button (L1/R2 are read but unbound, POWER
// has its own edge flag below).
static const uint8_t BIT_NAV[8] = {
  ATTAKY_NAV_DOWN,     // P00
  ATTAKY_NAV_LEFT,     // P01
  ATTAKY_NAV_SELECT,   // P02
  ATTAKY_NAV_RIGHT,    // P03
  ATTAKY_NAV_UP,       // P04
  ATTAKY_NAV_NONE,     // P05 BTN_L1 — unbound
  ATTAKY_NAV_NONE,     // P06 BTN_R2 — unbound
  ATTAKY_NAV_NONE,     // P07 POWER_BTN — attakyPowerKeyPressed()
};

static bool     s_inited   = false;
static bool     s_present  = false;
static uint32_t s_next_ms  = 0;

// Debounced whole-port state, active-low (a 0 bit = that button is down).
static uint8_t s_stable      = 0xFF;
static uint8_t s_pending     = 0xFF;   // candidate awaiting DEBOUNCE_POLLS agreement
static uint8_t s_pending_n   = 0;
static volatile bool s_power_event = false;

// Nav press queue. Small ring: the poll runs at ~30 ms and the UI drains every
// tick, so it only has to absorb a burst, never buffer.
static const uint8_t NAV_Q_LEN = 8;
static uint8_t s_navq[NAV_Q_LEN];
static uint8_t s_navq_head = 0, s_navq_tail = 0;

static int8_t   s_repeat_bit = -1;   // direction currently held, or -1
static uint32_t s_repeat_ms  = 0;    // when it next repeats

static void navPush(uint8_t ev) {
  const uint8_t next = (uint8_t)((s_navq_head + 1) % NAV_Q_LEN);
  if (next == s_navq_tail) return;   // full — drop the newest rather than stall
  s_navq[s_navq_head] = ev;
  s_navq_head = next;
}

static bool awWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(KEYS_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

// Reads IN_P0. Returns false on I2C failure without touching *val, so a failed
// read is never mistaken for 0xFF ("nothing pressed").
static bool awReadInP0(uint8_t* val) {
  Wire.beginTransmission(KEYS_ADDR);
  Wire.write(AW_REG_IN_P0);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)KEYS_ADDR, 1) != 1) return false;
  *val = (uint8_t)Wire.read();
  return true;
}

static void ensureInit() {
  if (s_inited) return;
  s_inited = true;
  if (!attakyI2cLock(30)) { s_inited = false; return; }   // retry on the next poll
  // P0 as GPIO inputs (POR defaults, but the touch driver shares this expander,
  // so set them explicitly rather than depend on init order).
  bool ok = awWrite(AW_REG_MODE_P0, 0xFF);   // GPIO mode, not LED mode
  ok &= awWrite(AW_REG_CFG_P0,  0xFF);       // all inputs
  s_present = ok;
  attakyI2cUnlock();
}

void attakyKeysPoll() {
  ensureInit();
  if (!s_present) return;

  const uint32_t now = millis();
  if ((int32_t)(now - s_next_ms) < 0) return;
  s_next_ms = now + POLL_INTERVAL_MS;

  uint8_t p0 = 0xFF;
  bool ok = false;
  if (attakyI2cLock(10)) {
    ok = awReadInP0(&p0);
    attakyI2cUnlock();
  }
  if (!ok) return;   // bus busy or read failed — hold the previous state

  // Every button reading pressed at once is a bad read, not a hand: acting on
  // it would inject a burst of nav keys AND a panel toggle from one glitch.
  // Resync the debouncer instead of treating the recovery as presses.
  if (p0 == 0x00) { s_pending = s_stable; s_pending_n = 0; return; }

  if (p0 != s_pending) { s_pending = p0; s_pending_n = 1; return; }
  if (s_pending_n < DEBOUNCE_POLLS) { s_pending_n++; return; }

  const uint8_t changed = (uint8_t)(s_stable ^ p0);
  if (changed) {
    const uint8_t went_down = (uint8_t)(changed & ~p0);   // active low: 1 -> 0
    s_stable = p0;
    if (went_down & POWER_BIT) s_power_event = true;      // fire on the press edge
    for (uint8_t b = 0; b < 8; b++) {
      if (!(went_down & (1u << b)) || BIT_NAV[b] == ATTAKY_NAV_NONE) continue;
      navPush(BIT_NAV[b]);
      // Only the directions repeat, and only the most recent one: rolling a
      // thumb across the pad should follow the finger, not fire two axes.
      if (BIT_NAV[b] != ATTAKY_NAV_SELECT) {
        s_repeat_bit = (int8_t)b;
        s_repeat_ms  = now + REPEAT_DELAY_MS;
      }
    }
    // The repeating direction was released (or another button's edge arrived
    // while it was already up) — stop repeating.
    if (s_repeat_bit >= 0 && (s_stable & (1u << (uint8_t)s_repeat_bit))) s_repeat_bit = -1;
  }

  if (s_repeat_bit >= 0 && (int32_t)(now - s_repeat_ms) >= 0) {
    navPush(BIT_NAV[(uint8_t)s_repeat_bit]);
    s_repeat_ms = now + REPEAT_RATE_MS;
  }
}

bool attakyPowerKeyPressed() {
  if (!s_power_event) return false;
  s_power_event = false;
  return true;
}

int attakyNavKeyRead() {
  if (s_navq_tail == s_navq_head) return ATTAKY_NAV_NONE;
  const uint8_t ev = s_navq[s_navq_tail];
  s_navq_tail = (uint8_t)((s_navq_tail + 1) % NAV_Q_LEN);
  return (int)ev;
}

void attakyNavKeysDiscard() {
  s_navq_tail = s_navq_head;
  s_repeat_bit = -1;
}

bool attakyKeysPresent() { return s_present; }

#else

void attakyKeysPoll() {}
bool attakyPowerKeyPressed() { return false; }
int  attakyNavKeyRead() { return ATTAKY_NAV_NONE; }
void attakyNavKeysDiscard() {}
bool attakyKeysPresent() { return false; }

#endif
