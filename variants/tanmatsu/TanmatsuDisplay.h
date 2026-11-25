#pragma once
#include <stdint.h>
#include <helpers/ui/DisplayDriver.h>
// badge-bsp 2.x headers carry their own extern "C" guards; the wrapper stays for older copies.
extern "C" {
#include "bsp/display.h"
#include "bsp/input.h"
}

// DisplayDriver backed by the Tanmatsu's badge-bsp MIPI panel (800x480 RGB565).
//
// The touch UI renders with LVGL; UITask's lvglFlush() calls writePixelsRGB565(), which
// blits the dirty area straight to the bsp panel — that's the hot path and the only part
// that must be correct for the UI to show. The classic DisplayDriver text/shape methods are
// used only for the pre-LVGL boot screen + the "Shutting down…" screen; they're minimal
// stubs for now (TODO(device): a tiny font renderer if we want those splashes back).
class TanmatsuDisplay : public DisplayDriver {
  bool _on = true;
public:
  TanmatsuDisplay() : DisplayDriver(800, 480) {}

  // Match the ST7789LCDDisplay surface UITask/main.cpp call on the global `display`:
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
    // bsp_display_blit takes (x_start, y_start, x_end, y_end), end exclusive (badge-bsp 2.x
    // names them that way; 0.9.x already behaved so under width/height names). Passing w/h
    // fails with "start position must be smaller than end position" and shows garbage.
    // From badge-bsp 2.x the blit only STARTS the transfer and the buffer must stay valid
    // until bsp_display_wait(): LVGL reuses this buffer for the next band as soon as the
    // flush returns, so wait here. (On IDF 5.5 the copy is done by the CPU inside the blit
    // and the wait returns at once; with DMA2D it is a real wait.)
    if (bsp_display_blit((size_t)x, (size_t)y, (size_t)(x + w), (size_t)(y + h), pixels) == ESP_OK)
      bsp_display_wait(1000);
  }
  void setDisplayRotation(int rot) { (void)rot; /* TODO(device): bsp panel rotation */ }
  void startFrame() { /* boot-screen no-op; LVGL owns the frame */ }
  void endFrame()   {}

  // --- DisplayDriver contract (minimal; LVGL does the real drawing) ---
  bool isOn() override { return _on; }
  void turnOn() override  { _on = true;  bsp_input_set_backlight_brightness(100); }
  void turnOff() override { _on = false; bsp_input_set_backlight_brightness(0); }
  void clear() override {}
  void startFrame(ColorVal) override {}
  void setTextSize(int) override {}
  void setColor(ColorVal) override {}
  void setCursor(int, int) override {}
  void print(const char*) override {}
  void fillRect(int, int, int, int) override {}
  void drawRect(int, int, int, int) override {}
  void drawXbm(int, int, const uint8_t*, int, int) override {}
  uint16_t getTextWidth(const char*) override { return 0; }
};
