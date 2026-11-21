// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// LilyGo T-Watch S3 — LovyanGFX-based ST7789 display driver (240x240 square).
//
// The T-Watch S3 routes its ST7789 panel on SPI3_HOST (SCK=18/MOSI=13), a
// different bus from the SX1262 radio (FSPI SCK=3/MOSI=1). LovyanGFX owns
// SPI3 here (bus_shared=false); the radio owns the Arduino default FSPI.

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
  void startFrame(ColorVal bkg = UIColor::window_bkg) override;
  void setTextSize(int sz) override;
  void setColor(ColorVal c) override;
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
