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
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);

private:
  lgfx::Bus_SPI _bus;
  lgfx::Panel_ILI9488 _panel;
  lgfx::Light_PWM _light;
  lgfx::Touch_GT911 _touch;
  lgfx::LGFX_Device _lcd;
  bool _isOn = false;
  uint16_t _color = 0xffff;
};
