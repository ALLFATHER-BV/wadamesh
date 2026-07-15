// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// T-Display P4 speaker path: ES8311 codec (I²C_2, 0x18) + NS4150B class-D amp (always
// enabled in hardware — LilyGo routes no control pin) fed by I2S MCLK=13 BCLK=12 WS=9
// DOUT=10. Fixed 16 kHz / 16-bit stereo — plenty for notification chimes, tiny buffers.
// All calls are safe from any task; init is lazy on first use (~ms, I²C + I2S bring-up).
#include <stdint.h>

// Bring the codec + I2S up (idempotent). Returns false if the codec never answered.
bool p4AudioReady();

// Blocking sine tone: `amplitude` is the 16-bit peak (0..~30000; the UI uses pct*130).
// No-op (fast) when amplitude <= 0 or the codec is absent.
void p4AudioTone(int freq_hz, int duration_ms, int amplitude);
