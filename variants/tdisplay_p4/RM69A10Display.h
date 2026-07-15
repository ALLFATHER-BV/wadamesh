// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// RM69A10 AMOLED (MIPI-DSI, 568x1232 portrait) — wadamesh DisplayDriver for the LilyGo T-Display P4.
// begin() runs the full P4 DSI bring-up: internal LDO (DSI PHY power) -> DSI bus -> DBI IO -> DPI
// panel -> RM69A10 vendor init (esp_lcd_rm69a10.*, ported from LilyGo) -> reset/init/on. LVGL's
// lvglFlush() -> writePixelsRGB565() blits the dirty area via esp_lcd_panel_draw_bitmap. Mirrors
// TanmatsuDisplay's contract. The panel RESET line is on the XL9535 (released in powerOnSequence),
// so the vendor driver's reset_gpio_num = -1. Timing from LilyGo rm69a10_driver.h (TDISPLAY_P4_PORT.md).
#include <stdint.h>
#include <helpers/ui/DisplayDriver.h>
#include "esp_lcd_types.h"

class RM69A10Display : public DisplayDriver {
  bool _on = true;
  esp_lcd_panel_handle_t _panel = nullptr;
  esp_lcd_panel_io_handle_t _dbi_io = nullptr;   // DCS command channel (runtime brightness 0x51)
public:
  RM69A10Display() : DisplayDriver(568, 1232) {}

  bool begin();   // full DSI bring-up; returns false on failure (logs the stage)

  // LVGL flush hot path — exclusive end coords (see TanmatsuDisplay note).
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);
  void setDisplayRotation(int rot) { (void)rot; }   // TODO(device): panel rotation / SW-rotate
  void startFrame() {}
  void endFrame()   {}

  // --- DisplayDriver contract (minimal; LVGL does the real drawing) ---
  bool isOn() override { return _on; }
  void turnOn() override;
  void turnOff() override;
  void setBrightness(uint8_t b);   // RM69A10 cmd 0x51
  void clear() override {}
  void startFrame(Color) override {}
  void setTextSize(int) override {}
  void setColor(Color) override {}
  void setCursor(int, int) override {}
  void print(const char*) override {}
  void fillRect(int, int, int, int) override {}
  void drawRect(int, int, int, int) override {}
  void drawXbm(int, int, const uint8_t*, int, int) override {}
  uint16_t getTextWidth(const char*) override { return 0; }
};
