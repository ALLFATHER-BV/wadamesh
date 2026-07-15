// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_TDISPLAY_P4)
#include "Xl9535.h"
#include <Arduino.h>
#include <Wire.h>

// 9535 register addresses
static constexpr uint8_t REG_OUT0 = 0x02;   // out1 = 0x03
static constexpr uint8_t REG_CFG0 = 0x06;   // cfg1 = 0x07 (bit: 1=input, 0=output)

void Xl9535::writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(_addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

uint8_t Xl9535::readReg(uint8_t reg) {
  Wire.beginTransmission(_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  Wire.requestFrom((int)_addr, 1);
  return Wire.available() ? Wire.read() : 0xFF;
}

bool Xl9535::begin(uint8_t sda, uint8_t scl, uint8_t addr) {
  _addr = addr;
  Wire.begin(sda, scl);
  Wire.setClock(400000);
  // probe
  Wire.beginTransmission(_addr);
  _ok = (Wire.endTransmission() == 0);
  if (!_ok) { Serial.printf("[XL9535] not found at 0x%02X (SDA=%d SCL=%d)\n", _addr, sda, scl); return false; }
  // start with outputs high, everything as output (we'll flip the few inputs in powerOnSequence)
  _out[0] = _out[1] = 0xFF;
  _cfg[0] = _cfg[1] = 0x00;   // all outputs by default
  writeReg(REG_OUT0, _out[0]); writeReg(REG_OUT0 + 1, _out[1]);
  writeReg(REG_CFG0, _cfg[0]); writeReg(REG_CFG0 + 1, _cfg[1]);
  Serial.println("[XL9535] up");
  return true;
}

void Xl9535::setDir(uint8_t io, bool input) {
  uint8_t port, bit; split(io, port, bit);
  if (input) _cfg[port] |=  (1 << bit);
  else       _cfg[port] &= ~(1 << bit);
  writeReg(REG_CFG0 + port, _cfg[port]);
}

void Xl9535::write(uint8_t io, bool high) {
  uint8_t port, bit; split(io, port, bit);
  if (high) _out[port] |=  (1 << bit);
  else      _out[port] &= ~(1 << bit);
  writeReg(REG_OUT0 + port, _out[port]);
}

bool Xl9535::read(uint8_t io) {
  uint8_t port, bit; split(io, port, bit);
  return (readReg(port /*in0=0x00,in1=0x01*/) >> bit) & 1;
}

void Xl9535::sx1262Reset() {
  write(IO_SX1262_RST, false); delay(2);
  write(IO_SX1262_RST, true);  delay(5);
}

void Xl9535::powerOnSequence() {
  if (!_ok) return;
  // inputs: touch INT, ext-sensor INT, RTC INT, SX1262 DIO1
  setDir(IO_TOUCH_INT, true);
  setDir(IO_EXT_SENSOR_INT, true);
  setDir(IO_RTC_INT, true);
  setDir(IO_SX1262_DIO1, true);

  // Power-rail bring-up — replicated EXACTLY from the tested-working LilyGo/Meck-P4 screen_lvgl
  // sequence. The rails are power-CYCLED with 200 ms settling and end in specific states (VCCA LOW,
  // 5V HIGH, 3V3 LOW); "just set them all high" leaves the AMOLED dark. Do not reorder / drop delays.
  setDir(IO_VCCA_EN, false); setDir(IO_5V_EN, false); setDir(IO_3V3_EN, false);
  setDir(IO_C6_EN, false);             // C6 enable is an output — reset-pulsed at the end of this sequence
  // GPS wake: HIGH = L76K awake (LilyGo's l76k example writes it HIGH, commented "turn off sleep").
  // We used to park it LOW = module in sleep -> UART silent -> "no GPS detected" (SD_EN's twin).
  write(IO_GPS_WAKE, true);

  write(IO_VCCA_EN, false);            // VCCA -> LOW (final)
  write(IO_5V_EN, true);  delay(200);  // 5V power-cycle: HIGH -> LOW -> HIGH
  write(IO_5V_EN, false); delay(200);
  write(IO_5V_EN, true);
  write(IO_3V3_EN, false); delay(200); // 3V3 power-cycle: LOW -> HIGH -> LOW
  write(IO_3V3_EN, true);  delay(200);
  write(IO_3V3_EN, false);
  delay(200);

  // Board peripherals we still need (Meck's display-only example omits these): the SX1262.
  // SD_EN (IO15) is an ACTIVE-LOW load-switch enable for the SD slot's VDD (pad-sweep proven
  // 2026-07-15: SD_EN high -> all six SD pads clamp LOW through the unpowered card's ESD diodes,
  // even against the P4's internal pull-ups; SD_EN low or Hi-Z -> all pads high, card powered).
  // Meck never touches it because the XL9535 powers up all-inputs (Hi-Z) and the board strap
  // defaults the switch ON — but OUR begin() parks every pin output-HIGH, which turned the slot
  // OFF and made every mount fail (0x107 OCR timeout / 0x109 CRC). Drive it LOW, always.
  write(IO_SD_EN, false);              // SD slot power ON (active-low)
  write(IO_SX1262_RST, true);          // park high; radio_init pulses it
  write(IO_RF_SWITCH, false);          // RX path
  write(IO_TOUCH_RST, true);           // release touch (HI8561 driver probes it)
  // NB: SCREEN_RST is intentionally NOT touched here — RM69A10Display::begin() sequences it with
  // the DSI-PHY LDO (HIGH->LOW->HIGH, 200 ms each), exactly as the working Meck-P4 app_main does.

  // ESP32-C6 reset + enable: pulse C6_EN (IO14) HIGH->LOW->HIGH (100 ms each) + ~1 s so the C6 boots
  // its ESP-AT firmware, which the c6_at component then drives over SDIO (AT-based Wi-Fi/BLE). C6_EN IS
  // the reset (no separate line). Doing it here means the C6 is booting while the rest of the app inits.
#ifdef TDP4_C6_FLASH_HELPER
  // C6-FLASH HELPER: do NOT reset-pulse the C6. C6_EN stays HIGH (powered) from begin()'s default, so
  // the C6 has power but is never yanked out of a user-held ROM download mode by a P4 (re)boot.
  Serial.println("[XL9535] power-on done — C6 reset SKIPPED (flash-helper build; C6 left free)");
#else
  write(IO_C6_EN, true);  delay(100);
  write(IO_C6_EN, false); delay(100);
  write(IO_C6_EN, true);  delay(100);
  Serial.println("[XL9535] power-on sequence done (C6 enabled for ESP-AT)");
#endif
}

#endif  // HAS_TDISPLAY_P4
