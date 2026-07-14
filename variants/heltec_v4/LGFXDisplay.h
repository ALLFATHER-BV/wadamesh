// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Heltec WiFi LoRa 32 V4-R8 (+ Expansion Kit V2) — LovyanGFX ST7789 driver.
//
// The V4-R8's 8 MB OCTAL PSRAM claims GPIO33-37, so the Expansion Kit V2 moved
// the TFT off the R2's software-SPI-on-GPIO33 path onto a real hardware SPI bus
// (SPI2_HOST/FSPI: SCK=16, MOSI=15, MISO=45) that it SHARES with the micro-SD
// slot (CS=3). Hardware SPI is therefore mandatory here — hence LovyanGFX with
// bus_shared=true, exactly like the RAK Tap V2 variant. All pins come from the
// PIN_TFT_* build flags. Compiled only for the R8 (guarded in the .cpp).

#include <helpers/ui/DisplayDriver.h>
#include <helpers/RefCountedDigitalPin.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFXDisplay : public DisplayDriver {
private:
  lgfx::Panel_ST7789  _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::LGFX_Device   _lcd;

  bool _isOn;
  uint16_t _color;
  RefCountedDigitalPin* _periph_power;

public:
  LGFXDisplay(RefCountedDigitalPin* peripher_power = nullptr);
  bool begin();

  // ---- DisplayDriver overrides ----
  bool isOn() override { return _isOn; }
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(Color bkg = DARK) override;
  void setTextSize(int sz) override;
  void setColor(Color c) override;
  void setCursor(int x, int y) override;
  void print(const char* str) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override;
  uint16_t getTextWidth(const char* str) override;
  void endFrame() override;

  // ---- LVGL flush entry point ----
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);

  // ---- Hardware panel rotation ----
  void setDisplayRotation(uint8_t r);
};
