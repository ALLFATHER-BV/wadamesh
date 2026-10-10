// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <helpers/ui/DisplayDriver.h>

// 480x480 ST7701S RGB panel.
class IndicatorDisplay : public DisplayDriver {
public:
  IndicatorDisplay() : DisplayDriver(480, 480) {}
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

  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* px);
  void setDisplayRotation(uint8_t rotation);
  void setBrightness(uint8_t brightness);

private:
  bool _isOn = false;
};
