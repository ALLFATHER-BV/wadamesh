// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// T-Display P4 vibration motor: an LRA driven by an AW86224 haptic driver (I²C_2 @0x58, the
// same 20/21 pair as the ES8311 codec -- P4I2c1.h). The chip plays waveforms out of its own
// SRAM, which is loaded once on first use. All calls are safe from any task; init is lazy.
#include <stdint.h>

// Probe the chip and load its waveform RAM (idempotent). Returns false if it never answered.
bool p4HapticReady();

// Blocking: vibrate for ~duration_ms at `gain` (0..255 drive strength), then brake to a stop.
// Run it off the UI/mesh loop -- the I²C bus is only held for the start/stop writes, not while
// the motor runs. No-op (fast) when duration_ms <= 0 or the chip is absent.
#define P4_HAPTIC_GAIN_DEFAULT 0x80   // LilyGo's default (127) feels right on this motor
void p4HapticBuzz(int duration_ms, uint8_t gain = P4_HAPTIC_GAIN_DEFAULT);
