#pragma once
// The web board's panel: an RGB565 framebuffer the LVGL flush writes into
// (display.writePixelsRGB565 in UITask's lvglFlushImpl) and the platform layer
// presents (SDL texture now, a <canvas> under Emscripten). Text drawing is the
// DisplayDriver contract for non-LVGL screens; the touch UI never uses it here.
#include <helpers/ui/DisplayDriver.h>
#include <stdint.h>
#include <stddef.h>

// The canvas size is the board's resolution, picked by the front end at boot from the
// window (web_main.cpp / host_main.cpp --size): 800x480 landscape, or a portrait
// canvas on a phone (WEB_BUILD.md "Phones"). It never changes while the UI runs: an
// orientation flip restarts the UI in the other layout, as a rotatable board does.
extern int g_wada_web_w, g_wada_web_h;
#define WADA_WEB_W g_wada_web_w
#define WADA_WEB_H g_wada_web_h
// Before the board boots (the display is a global, built before main()).
void wadaWebSetSize(int w, int h);

class WebDisplay : public DisplayDriver {
public:
  WebDisplay();
  bool begin() { return true; }
  bool isOn() override { return _on; }
  void turnOn() override { _on = true; }
  void turnOff() override { _on = false; }
  void panelSleep(bool) {}   // the Pager's ST7796 sleep-in (T-LoRa Pager simulator)
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
  void setDisplayRotation(uint8_t r) { (void)r; }   // the canvas is already the right way up
  void resize(int w, int h);

  // platform side
  const uint16_t* framebuffer() const { return _fb; }
  bool takeDirty() { bool d = _dirty; _dirty = false; resetBox(); return d; }
  // The changed area since the last take (x0,y0 inclusive, x1,y1 exclusive): the
  // canvas front end uploads only that rectangle.
  bool takeDirtyRect(int& x0, int& y0, int& x1, int& y1) {
    if (!_dirty) return false;
    x0 = _bx0; y0 = _by0; x1 = _bx1; y1 = _by1;
    _dirty = false; resetBox();
    return x1 > x0 && y1 > y0;
  }
private:
  void resetBox() { _bx0 = WADA_WEB_W; _by0 = WADA_WEB_H; _bx1 = 0; _by1 = 0; }
  void addBox(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > WADA_WEB_W) x1 = WADA_WEB_W;
    if (y1 > WADA_WEB_H) y1 = WADA_WEB_H;
    if (x0 < _bx0) _bx0 = x0;
    if (y0 < _by0) _by0 = y0;
    if (x1 > _bx1) _bx1 = x1;
    if (y1 > _by1) _by1 = y1;
    _dirty = true;
  }
  int _bx0 = 0, _by0 = 0, _bx1 = WADA_WEB_W, _by1 = WADA_WEB_H;
  uint16_t* _fb;
  ColorVal _color = 0xFFFF;
  uint8_t _brightness = 255;
  bool _on = true;
  volatile bool _dirty = true;
};
