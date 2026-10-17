// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <helpers/ui/DisplayDriver.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

// ST7701S with the Indicator's scan direction: LovyanGFX's stock ST7701 list,
// then a second list that flips both axes.
class IndicatorPanel : public lgfx::Panel_ST7701 {
protected:
  const uint8_t* getInitCommands(uint8_t listno) const override;
};

// 480x480 ST7701S RGB panel. The 3-wire SPI that configures it shares GPIO41/48
// with the radio; its CS and RESET sit on the TCA9535.
class IndicatorDisplay : public DisplayDriver {
public:
  IndicatorDisplay();
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
  IndicatorPanel _panel;
  lgfx::Bus_RGB _bus;
  lgfx::Light_PWM _light;
  lgfx::LGFX_Device _lcd;
  bool _panelOk = false;   // begin() brought the panel up; until then the backlight stays off
  bool _isOn = false;
  uint16_t _color = 0xFFFF;
};
