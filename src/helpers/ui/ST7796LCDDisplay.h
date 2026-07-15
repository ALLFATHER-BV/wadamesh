// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// T-LoRa Pager 480x222 (landscape) / 222x480 (native) ST7796 panel, driven
// via TFT_eSPI — the first real TFT_eSPI consumer in this codebase (the
// Heltec V4 env's TFT_eSPI lib_dep/-D flags are vestigial: nothing in the
// repo actually #includes TFT_eSPI.h; both Heltec V4 and T-Deck's
// ST7789LCDDisplay are built on Adafruit_GFX/Adafruit_ST7789 instead). Only
// the DisplayDriver-satisfying shape is mirrored from ST7789LCDDisplay, not
// any TFT_eSPI API calls — see the .cpp for the exact TFT_eSPI equivalents.
//
// Backlight is the AW9364 stepped pulse-dimmer IC (16 discrete steps), not a
// PWM-capable pin (TFT_BL=-1) — wrapped directly in here rather than as a
// separate Aw9364Backlight.{h,cpp}, since UITask.cpp has no display-class
// brightness hook to mirror today (brightness there is raw LEDC PWM on
// PIN_TFT_LEDA_CTL, which the AW9364 can't use). setBrightness()/
// getBrightness() here are pct-based (0-100) so a later UITask.cpp wiring
// pass can call display.setBrightness(pct) directly, matching the existing
// pct-based Settings convention.
#if defined(TLORA_PAGER) && defined(ESP32)

#include <helpers/ui/DisplayDriver.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <AW9364LedDriver.hpp>

class ST7796LCDDisplay : public DisplayDriver {
  TFT_eSPI display;
  AW9364LedDriver _backlight;
  bool _isOn;
  uint16_t _color;
  uint8_t  _brightness_pct;   // exact requested pct, cached for getBrightness() (the 0-100 -> 0-16 -> 0-100
                              // round trip through the AW9364's discrete steps is lossy)

public:
  // Native panel is 222x480 portrait; landscape (480x222) comes from MADCTL
  // rotation, same approach as the other boards. These are placeholders --
  // begin()/setDisplayRotation() overwrite via setLogicalSize() using
  // TFT_eSPI's own (rotation-aware) width()/height().
  static const int LOGICAL_WIDTH = 222;
  static const int LOGICAL_HEIGHT = 480;

  ST7796LCDDisplay() : DisplayDriver(LOGICAL_WIDTH, LOGICAL_HEIGHT), _isOn(false), _color(0xFFFF), _brightness_pct(100) { }

  bool begin();

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

  // Extra methods beyond DisplayDriver, called directly on the concrete type
  // (same pattern as ST7789LCDDisplay's writePixelsRGB565/setDisplayRotation).
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);
  void setDisplayRotation(uint8_t r);

  // AW9364 brightness hook, pct in/pct out by design (see file header).
  void setBrightness(uint8_t pct);
  uint8_t getBrightness() const { return _brightness_pct; }
};

#endif
