// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Wio Tracker L2 speaker path: ES8311 codec (0x18, on Wire with the expander) feeding a
// class-D amp whose enable is expander line 12 (PA_PWR_EN). I2S MCLK/BCLK/WS/DOUT come
// from the env's PIN_I2S_* flags. Pin map per Seeed's Meshtastic variant
// (variants/esp32s3/seeed_wio_tracker_L2/variant.h).
//
// Threading: the touch controller (LovyanGFX's own I2C driver) and Wire share I2C port 0,
// and today only the UI/loop task drives that bus. To keep it that way, every codec and
// expander write happens on the caller's task -- wioL2AudioPlay() brings the path up,
// wioL2AudioService() takes it down -- and only the blocking I2S writes run on a short
// worker task. Both functions must be called from the UI task.
#include <stdint.h>

// Start a chime of up to four notes (freq Hz / duration ms pairs) at `volPct` (0..100,
// the codec's hardware volume). Returns false, and leaves the amp off, if a chime is
// already playing, internal RAM is too tight for the I2S driver, or the codec/expander
// did not answer.
bool wioL2AudioPlay(const uint16_t* freqs, const uint16_t* durationsMs, uint8_t count, uint8_t volPct);

// Call every UI loop: once the worker has finished, mutes the codec, powers its DAC
// down, frees the I2S driver and drops the amp enable.
void wioL2AudioService();

bool wioL2AudioBusy();
