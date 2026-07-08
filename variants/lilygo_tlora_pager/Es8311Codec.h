// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal ES8311 codec driver (I2C register control only -- I2S data is
// configured/driven separately via the ESP32's own I2S peripheral, same as
// this repo's existing T-Deck sound code). Output (DAC/speaker) path only:
// the pager wires the codec to a one-way Class-D amp (NS4150B), so the
// ADC/mic side of the chip is left in its power-down reset default.
//
// Register sequence and clock-divider coefficients are derived from
// Espressif's public esp_codec_dev ES8311 driver (Apache-2.0) -- see
// NOTICE. This is original code, not a port of that driver's generic
// multi-codec plugin architecture: it's trimmed to exactly what this board
// needs -- DAC-only, ESP32-S3 as I2S master with the codec as slave, and an
// external MCLK fixed at 256x the sample rate (the caller's I2S driver must
// be configured that way before calling start()).
#pragma once

#include <Wire.h>
#include <stdint.h>

class Es8311Codec {
public:
  // Probe the codec at `addr` on `wire` and push its startup register
  // sequence (clocks left in a powered-down state -- call start() before
  // playback). Returns false if the I2C probe or any register write fails.
  bool begin(TwoWire& wire, uint8_t addr = 0x18);

  // Configure the I2S format/sample-rate dividers and power up the DAC
  // path. `sampleRate` must be one of the rates in the codec's supported
  // set (matches wavParse()'s accepted WAV sample rates, plus the fixed
  // tone-generator rate); returns false and changes nothing on an
  // unsupported rate. Call once per playback session, after the ESP32's
  // own I2S driver is already clocking (the codec's PLL won't lock until
  // MCLK is toggling).
  bool start(uint32_t sampleRate);

  // Power down the DAC path. Call after playback finishes, before the
  // amp's AMP_EN is dropped.
  void suspend();

  void setMute(bool mute);

  // Map the existing 0-100 UI volume pref onto the codec's digital-volume
  // register range. The bottom of that range is inaudible on this amp, so
  // nonzero settings are floored into an audible band -- same idea as
  // Tanmatsu's applyVolume() (UITask.cpp), different codec/registers.
  void setVolumePercent(uint8_t pct);

private:
  bool writeReg(uint8_t reg, uint8_t val);
  bool readReg(uint8_t reg, uint8_t* val);

  TwoWire* wire_   = nullptr;
  uint8_t  addr_   = 0x18;
  bool     opened_ = false;
};
