// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <helpers/ui/DisplayDriver.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/panel/Panel_NV3031B.hpp>

class WioTrackerL2Light : public lgfx::v1::ILight {
public:
  bool init(uint8_t brightness) override;
  void setBrightness(uint8_t brightness) override;
  uint8_t getBrightness() const { return _brightness; }

private:
  bool writeRegister(uint8_t reg, uint8_t value);
  uint8_t _brightness = 160;
};

class WioTrackerL2Display : public DisplayDriver {
public:
  WioTrackerL2Display();
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

  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);
  void setDisplayRotation(uint8_t rotation);
  void setBrightness(uint8_t brightness);
  bool getTouchPoint(uint16_t& x, uint16_t& y);

private:
  lgfx::Panel_NV3031B _panel;
  lgfx::Bus_SPI _bus;
  WioTrackerL2Light _light;
  lgfx::Touch_GT911 _touch;
  lgfx::LGFX_Device _lcd;
  bool _isOn = false;
  uint16_t _color = 0xFFFF;
};