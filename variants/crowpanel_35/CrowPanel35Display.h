// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <helpers/ui/DisplayDriver.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class CrowPanel35Display : public DisplayDriver {
public:
  CrowPanel35Display();
  bool begin();
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

  void setDisplayRotation(uint8_t rotation);
  void setBrightness(uint8_t value);
  bool getTouchPoint(uint16_t& x, uint16_t& y);
  uint8_t touchAddr();   // 0x5D / 0x14 the GT911 answers at, 0 while silent
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);

private:
  // Touch_GT911 keeps its init result to itself; this only reads it back.
  struct Gt911 : lgfx::Touch_GT911 { bool ready() const { return _inited; } };
  bool recoverTouch();

  lgfx::Bus_SPI _bus;
  lgfx::Panel_ILI9488 _panel;
  lgfx::Light_PWM _light;
  Gt911 _touch;
  lgfx::LGFX_Device _lcd;
  bool _isOn = false;
  uint32_t _touch_retry_ms = 0;
  uint8_t _touch_retries = 0;
  uint16_t _color = 0xffff;
};
