// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// RAK WisMesh Tap V2 — LovyanGFX-based ST7789 display driver.
//
// Replaces the USE_PIN_TFT (bit-bang software SPI) path with LovyanGFX
// hardware SPI on SPI2_HOST (FSPI). The Arduino framework's default SPI
// already initialised FSPI on the TFT pins (13/11/10 from pins_arduino.h);
// LovyanGFX with bus_shared=true reuses that existing bus.
//
// This is architecturally identical to what Meshtastic's rak_wismesh_tap_v2
// variant does — same SPI host, same pins, same library.

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
  // Mirrors ST7789LCDDisplay::writePixelsRGB565.
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);

  // ---- Hardware panel rotation ----
  void setDisplayRotation(uint8_t r);
};
