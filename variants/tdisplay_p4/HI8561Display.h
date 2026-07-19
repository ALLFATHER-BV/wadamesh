// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// HI8561 TFT-LCD (MIPI-DSI, 540x1168 portrait) — wadamesh DisplayDriver for the LilyGo T-Display P4
// *LCD* SKU. The T-Display P4 ships in two panel variants (LilyGo's own firmware branches
// [hi8561] vs [rm69a10]): the AMOLED SKU uses the RM69A10 (see RM69A10Display), the TFT-LCD SKU
// uses the HI8561 TDDI (integrated display+touch driver). This class is the LCD-SKU display; it is
// a near line-for-line mirror of RM69A10Display — the P4 DSI bring-up is identical (internal LDO ->
// DSI bus -> DBI IO -> DPI panel -> vendor init -> reset/init/on) — only the vendor panel driver
// (esp_lcd_new_panel_hi8561, in the vendored Apache-2.0 esp_lcd_hi8561.*), the resolution
// (540x1168), and the DPI timing (48 MHz, from the vendor's HI8561_540_1168 macro) differ. The
// panel RESET line is on the XL9535 (released in RM/HI begin), so the vendor reset_gpio_num = -1.
// Backlight is HI8561-internal (no GPIO): brightness is DCS 0x51, like the RM69A10.
// Selected at build time by HAS_TDP4_LCD (DISPLAY_CLASS=HI8561Display) — see tdisplay_p4/main/CMakeLists.txt.
#include <stdint.h>
#include <helpers/ui/DisplayDriver.h>
#include "esp_lcd_types.h"

class HI8561Display : public DisplayDriver {
  bool _on = true;
  esp_lcd_panel_handle_t _panel = nullptr;
  esp_lcd_panel_io_handle_t _dbi_io = nullptr;   // DCS command channel (runtime brightness 0x51)
public:
  HI8561Display() : DisplayDriver(540, 1168) {}

  bool begin();   // full DSI bring-up; returns false on failure (logs the stage)

  // LVGL flush hot path — exclusive end coords (see RM69A10Display note).
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);
  void setDisplayRotation(int rot) { (void)rot; }   // TODO(device): panel rotation / SW-rotate
  void startFrame() {}
  void endFrame()   {}

  // --- DisplayDriver contract (minimal; LVGL does the real drawing) ---
  bool isOn() override { return _on; }
  void turnOn() override;
  void turnOff() override;
  void setBrightness(uint8_t b);   // HI8561 cmd 0x51 (integrated backlight)
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
