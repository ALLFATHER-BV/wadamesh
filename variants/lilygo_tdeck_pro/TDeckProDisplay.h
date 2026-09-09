// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <helpers/ui/DisplayDriver.h>

#define ENABLE_GxEPD2_GFX 0
#include <GxEPD2_BW.h>
#include <CSE_CST328.h>
#include <LovyanGFX.hpp>
#include <SPI.h>
#include <Wire.h>

class TDeckProDisplay : public DisplayDriver {
public:
  using BusyHook = void (*)();

  TDeckProDisplay();
  bool begin();

  bool isOn() override { return _is_on; }
  bool isEink() override { return true; }
  void turnOn() override;
  void turnOff() override;
  void clear() override;
  void startFrame(ColorVal background = UIColor::window_bkg) override;
  void setTextSize(int size) override;
  void setColor(ColorVal color) override;
  void setCursor(int x, int y) override;
  void print(const char* text) override;
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override;
  uint16_t getTextWidth(const char* text) override;
  void endFrame() override;

  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);
  static void prepareMapTileRGB565(uint16_t* pixels, int width, int height);
  void setDisplayRotation(uint8_t rotation);
  void setBrightness(uint8_t brightness);
  bool getTouchPoint(uint16_t& x, uint16_t& y);
  void serviceRefresh(bool force = false);
  void setBusyHook(BusyHook hook) { _busy_hook = hook; }

private:
  using Panel = GxEPD2_310_GDEQ031T10;
  using Eink = GxEPD2_BW<Panel, 40>;

  static void busyCallback(const void*);
  static bool isDark(uint16_t color);
  void resetTouch();
  bool probeCst3530();
  bool writeCst3530Command(uint32_t command);
  bool initCst3530();
  bool readCst3530(int16_t& x, int16_t& y);
  void canvasToMono();
  void requestRefresh(bool full = false);
  void writeBrightness(uint8_t brightness);

  static BusyHook _busy_hook;
  static constexpr int WIDTH = 240;
  static constexpr int HEIGHT = 320;
  static constexpr size_t MONO_BYTES = (size_t)WIDTH * HEIGHT / 8;

  lgfx::LGFX_Sprite _canvas;
  Eink _epd;
  CSE_CST328 _cst328;
  uint8_t* _mono = nullptr;
  uint8_t* _sent = nullptr;
  uint16_t _color = 0x0000;
  uint32_t _last_refresh_ms = 0;
  uint8_t _partial_refreshes = 0;
  uint8_t _brightness = 255;
  bool _refresh_pending = false;
  bool _full_refresh_pending = true;
  bool _sleeping = false;
  bool _touch_ready = false;
  bool _touch_is_cst3530 = false;
  bool _sent_valid = false;
  bool _is_on = false;
};