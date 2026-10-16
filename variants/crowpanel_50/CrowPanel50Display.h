// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <helpers/ui/DisplayDriver.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>

// Whatever owns the backlight and the GT911 reset on this board. V1.0 has a
// PCA9557 expander at 0x18; from V1.1 a small board MCU at 0x30 takes both over
// (and the buzzer/speaker). begin() tells them apart by which address answers,
// so one build drives either. Ported from Camillia-MT's crowpanel_50_display.h.
class CrowPanel50BoardCtl {
public:
  enum Kind : uint8_t { NONE, PCA9557, MCU };

  Kind begin();
  uint8_t resetTouch();            // 0x5D / 0x14 the GT911 answered at, 0 if neither
  void setBacklight(uint8_t level);
  Kind kind() const { return _kind; }

private:
  static bool probe(uint8_t addr);
  void pcaSet(uint8_t pin, bool high);
  void pcaWrite(uint8_t reg, uint8_t value);
  void mcuSend(uint8_t cmd);
  bool mcuSendAlways(uint8_t cmd);

  Kind _kind = NONE;
  uint8_t _out = 0;
  int16_t _lastMcu = -1;
};

class CrowPanel50Light : public lgfx::v1::ILight {
public:
  void setController(CrowPanel50BoardCtl* ctl) { _ctl = ctl; }
  bool init(uint8_t brightness) override { setBrightness(brightness); return true; }
  void setBrightness(uint8_t brightness) override { if (_ctl) _ctl->setBacklight(brightness); }

private:
  CrowPanel50BoardCtl* _ctl = nullptr;
};

class CrowPanel50Display : public DisplayDriver {
public:
  CrowPanel50Display();
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
  uint8_t touchAddr() const { return _touchAddr; }

private:
  lgfx::Bus_RGB _bus;
  lgfx::Panel_RGB _panel;
  CrowPanel50Light _light;
  lgfx::Touch_GT911 _touch;
  lgfx::LGFX_Device _lcd;
  CrowPanel50BoardCtl _ctl;
  uint8_t _touchAddr = 0;
  bool _touchDown = false;
  uint16_t _touchX = 0, _touchY = 0;
  uint8_t _brightness = 255;
  bool _isOn = false;
  uint16_t _color = 0xffff;
};
