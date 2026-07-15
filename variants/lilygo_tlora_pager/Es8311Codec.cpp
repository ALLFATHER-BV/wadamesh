// SPDX-License-Identifier: GPL-3.0-or-later
#include "Es8311Codec.h"

namespace {
// ES8311 register addresses (datasheet names).
constexpr uint8_t REG_RESET  = 0x00;
constexpr uint8_t REG_CLK01  = 0x01;
constexpr uint8_t REG_CLK02  = 0x02;
constexpr uint8_t REG_CLK03  = 0x03;
constexpr uint8_t REG_CLK04  = 0x04;
constexpr uint8_t REG_CLK05  = 0x05;
constexpr uint8_t REG_CLK06  = 0x06;
constexpr uint8_t REG_CLK07  = 0x07;
constexpr uint8_t REG_CLK08  = 0x08;
constexpr uint8_t REG_SDPIN  = 0x09;   // DAC serial port format/width
constexpr uint8_t REG_SDPOUT = 0x0A;   // ADC serial port -- mirrors SDPIN's iface bits on this chip
constexpr uint8_t REG_SYS0B  = 0x0B;
constexpr uint8_t REG_SYS0C  = 0x0C;
constexpr uint8_t REG_SYS0D  = 0x0D;
constexpr uint8_t REG_SYS0E  = 0x0E;
constexpr uint8_t REG_SYS10  = 0x10;
constexpr uint8_t REG_SYS11  = 0x11;
constexpr uint8_t REG_SYS12  = 0x12;
constexpr uint8_t REG_SYS13  = 0x13;
constexpr uint8_t REG_SYS14  = 0x14;
constexpr uint8_t REG_ADC15  = 0x15;
constexpr uint8_t REG_ADC16  = 0x16;
constexpr uint8_t REG_ADC17  = 0x17;
constexpr uint8_t REG_ADC1B  = 0x1B;
constexpr uint8_t REG_ADC1C  = 0x1C;
constexpr uint8_t REG_DAC31  = 0x31;   // mute
constexpr uint8_t REG_DAC32  = 0x32;   // digital volume, 0x00=-95.5dB .. 0xFF=+32dB
constexpr uint8_t REG_DAC37  = 0x37;
constexpr uint8_t REG_GPIO44 = 0x44;
constexpr uint8_t REG_GP45   = 0x45;

// Clock-divider coefficients for MCLK = 256 x sample rate (the ratio the
// caller's I2S driver is expected to configure), covering the sample rates
// wavParse() accepts plus the tone generator's fixed 16 kHz. This is the
// subset of Espressif's public coeff_div table (esp_codec_dev's es8311.c,
// see NOTICE) at the 256x ratio: at that ratio every divider field is
// constant across rates -- pre_div=pre_multi=adc_div=dac_div=1, fs_mode=0
// ("single speed"), lrck=0x00FF, bclk_div=4, adc_osr=0x10 -- except dac_osr,
// which steps from 0x20 to 0x10 at the 16 kHz/22.05 kHz speed-mode boundary.
struct RateCoeff { uint32_t rate; uint8_t dac_osr; };
constexpr RateCoeff kRateCoeffs[] = {
  {8000,  0x20}, {11025, 0x20}, {12000, 0x20}, {16000, 0x20},
  {22050, 0x10}, {24000, 0x10}, {32000, 0x10}, {44100, 0x10}, {48000, 0x10},
};

const RateCoeff* findRateCoeff(uint32_t rate) {
  for (const auto& c : kRateCoeffs) if (c.rate == rate) return &c;
  return nullptr;
}
}  // namespace

bool Es8311Codec::writeReg(uint8_t reg, uint8_t val) {
  wire_->beginTransmission(addr_);
  wire_->write(reg);
  wire_->write(val);
  return wire_->endTransmission() == 0;
}

bool Es8311Codec::readReg(uint8_t reg, uint8_t* val) {
  wire_->beginTransmission(addr_);
  wire_->write(reg);
  if (wire_->endTransmission(false) != 0) return false;
  if (wire_->requestFrom((int)addr_, 1) != 1) return false;
  *val = (uint8_t)wire_->read();
  return true;
}

bool Es8311Codec::begin(TwoWire& wire, uint8_t addr) {
  wire_   = &wire;
  addr_   = addr;
  opened_ = false;

  wire_->beginTransmission(addr_);
  if (wire_->endTransmission() != 0) return false;   // no ack at this address

  bool ok = true;
  // I2C noise immunity -- written twice; the first write right after the
  // amp/codec rail powers up occasionally doesn't take.
  ok &= writeReg(REG_GPIO44, 0x08);
  ok &= writeReg(REG_GPIO44, 0x08);

  ok &= writeReg(REG_CLK01, 0x30);
  ok &= writeReg(REG_CLK02, 0x00);
  ok &= writeReg(REG_CLK03, 0x10);
  ok &= writeReg(REG_ADC16, 0x24);   // mic-gain default; ADC/mic path otherwise unused on this board
  ok &= writeReg(REG_CLK04, 0x10);
  ok &= writeReg(REG_CLK05, 0x00);
  ok &= writeReg(REG_SYS0B, 0x00);
  ok &= writeReg(REG_SYS0C, 0x00);
  ok &= writeReg(REG_SYS10, 0x1F);
  ok &= writeReg(REG_SYS11, 0x7F);

  // Bring the chip out of reset in slave mode (bit6 clear) -- the ESP32-S3
  // drives BCLK/WS/MCLK as I2S master, the codec follows.
  ok &= writeReg(REG_RESET, 0x80);

  // Clock source = external MCLK pin, not inverted.
  ok &= writeReg(REG_CLK01, 0x3F);

  ok &= writeReg(REG_SYS13, 0x10);
  ok &= writeReg(REG_ADC1B, 0x0A);
  ok &= writeReg(REG_ADC1C, 0x6A);
  ok &= writeReg(REG_GPIO44, 0x58);   // internal reference signal (ADCL+DACR)

  opened_ = ok;
  return ok;
}

bool Es8311Codec::start(uint32_t sampleRate) {
  if (!opened_) return false;
  const RateCoeff* c = findRateCoeff(sampleRate);
  if (!c) return false;

  bool ok = true;
  ok &= writeReg(REG_RESET, 0x80);   // slave mode, chip active (see begin())
  ok &= writeReg(REG_CLK01, 0x3F);   // external MCLK, not inverted (see begin())

  // Standard I2S, 16-bit -- fixed for every caller of this driver (the
  // notification-sound path never varies bit depth), so this is a direct
  // write rather than the read-modify-write the reference driver uses to
  // support runtime format/width changes we don't need.
  ok &= writeReg(REG_SDPIN,  0x0C);
  ok &= writeReg(REG_SDPOUT, 0x0C);

  ok &= writeReg(REG_CLK02, 0x00);          // pre_div=1, pre_multi=1
  ok &= writeReg(REG_CLK05, 0x00);          // adc_div=1, dac_div=1
  ok &= writeReg(REG_CLK03, 0x10);          // fs_mode=0 (single speed) | adc_osr=0x10
  ok &= writeReg(REG_CLK04, c->dac_osr);
  ok &= writeReg(REG_CLK07, 0x00);          // lrck_h
  ok &= writeReg(REG_CLK08, 0xFF);          // lrck_l
  ok &= writeReg(REG_CLK06, 0x03);          // bclk_div=4 -> (4-1)

  // Power up the DAC path only -- ADC/mic stays powered down.
  ok &= writeReg(REG_ADC17, 0xBF);
  ok &= writeReg(REG_SYS0E, 0x02);
  ok &= writeReg(REG_SYS12, 0x00);
  ok &= writeReg(REG_SYS14, 0x1A);
  ok &= writeReg(REG_SYS0D, 0x01);
  ok &= writeReg(REG_ADC15, 0x40);
  ok &= writeReg(REG_DAC37, 0x08);
  ok &= writeReg(REG_GP45,  0x00);

  return ok;
}

void Es8311Codec::suspend() {
  if (!opened_) return;
  writeReg(REG_DAC32, 0x00);
  writeReg(REG_ADC17, 0x00);
  writeReg(REG_SYS0E, 0xFF);
  writeReg(REG_SYS12, 0x02);
  writeReg(REG_SYS14, 0x00);
  writeReg(REG_SYS0D, 0xFA);
  writeReg(REG_ADC15, 0x00);
  writeReg(REG_CLK02, 0x10);
  writeReg(REG_RESET, 0x00);
  writeReg(REG_RESET, 0x1F);
  writeReg(REG_CLK01, 0x30);
  writeReg(REG_CLK01, 0x00);
  writeReg(REG_GP45,  0x00);
  writeReg(REG_SYS0D, 0xFC);
  writeReg(REG_CLK02, 0x00);
}

void Es8311Codec::setMute(bool mute) {
  if (!opened_) return;
  uint8_t regv = 0;
  readReg(REG_DAC31, &regv);
  regv &= 0x9F;
  writeReg(REG_DAC31, mute ? (regv | 0x60) : regv);
}

void Es8311Codec::setVolumePercent(uint8_t pct) {
  if (!opened_) return;
  if (pct > 100) pct = 100;
  if (pct == 0) { writeReg(REG_DAC32, 0x00); return; }
  constexpr uint8_t kFloorReg = 0x60;   // quietest setting that's still clearly audible
  uint8_t reg = (uint8_t)(kFloorReg + ((uint32_t)(0xFF - kFloorReg) * pct) / 100);
  writeReg(REG_DAC32, reg);
}
