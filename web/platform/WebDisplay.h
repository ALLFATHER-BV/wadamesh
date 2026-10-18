#pragma once
// The web board's panel: an RGB565 framebuffer the LVGL flush writes into
// (display.writePixelsRGB565 in UITask's lvglFlushImpl) and the platform layer
// presents (SDL texture now, a <canvas> under Emscripten). Text drawing is the
// DisplayDriver contract for non-LVGL screens; the touch UI never uses it here.
#include <helpers/ui/DisplayDriver.h>
#include <stdint.h>
#include <stddef.h>

#ifndef WADA_WEB_W
#define WADA_WEB_W 800
#endif
#ifndef WADA_WEB_H
#define WADA_WEB_H 480
#endif

class WebDisplay : public DisplayDriver {
public:
  WebDisplay();
  bool begin() { return true; }
  bool isOn() override { return _on; }
  void turnOn() override { _on = true; }
  void turnOff() override { _on = false; }
  void clear() override;
  void startFrame(ColorVal bkg = UIColor::window_bkg) override { (void)bkg; }
  void setTextSize(int sz) override { (void)sz; }
  void setColor(ColorVal c) override { _color = c; }
  void setCursor(int x, int y) override { (void)x; (void)y; }
  void print(const char* str) override { (void)str; }
  void fillRect(int x, int y, int w, int h) override;
  void drawRect(int x, int y, int w, int h) override;
  void drawXbm(int x, int y, const uint8_t* bits, int w, int h) override { (void)x; (void)y; (void)bits; (void)w; (void)h; }
  uint16_t getTextWidth(const char* str) override;
  void endFrame() override {}

  // the LVGL flush target
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* px);
  void setBrightness(uint8_t level) { _brightness = level; }
  uint8_t brightness() const { return _brightness; }
  void setDisplayRotation(uint8_t r) { (void)r; }   // fixed landscape canvas

  // platform side
  const uint16_t* framebuffer() const { return _fb; }
  bool takeDirty() { bool d = _dirty; _dirty = false; return d; }
private:
  uint16_t* _fb;
  ColorVal _color = 0xFFFF;
  uint8_t _brightness = 255;
  bool _on = true;
  volatile bool _dirty = true;
};
