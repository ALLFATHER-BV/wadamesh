// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// EK79007 IPS panel (MIPI-DSI 2-lane, DPI RGB565, 1024x600 landscape) on the Elecrow CrowPanel
// Advanced 10.1" ESP32-P4. begin() runs the DSI bring-up with Elecrow's validated parameters
// (900 Mbps lanes, 51 MHz DPI, porches 160/70/160 + 23/10/12) through Espressif's esp_lcd_ek79007
// component. LVGL renders at the NATIVE 1024x600 (no upscale: at ~118 dpi the panel is low density,
// so a 2x upscale would leave a 512x300 canvas, smaller than the CrowPanel 5.0's). Backlight =
// MT9201 boost driver, EN/PWM on GPIO31 (LEDC); GPIO29 is the optional backlight power switch.
#include <stdint.h>
#include "../tdisplay_p4/P4PanelPainter.h"   // native-resolution fills/text for non-LVGL screens
#include "esp_lcd_types.h"

class EK79007Display : public P4PanelPainter {
  bool _on = true;
  uint8_t _bright = 255;
  esp_lcd_panel_handle_t _panel = nullptr;
public:
  EK79007Display() : P4PanelPainter(1024, 600) {}

  bool begin();   // full DSI bring-up; returns false on failure (logs the stage)

  // LVGL flush hot path. Exclusive end coords, synchronous (waits for the DMA2D copy).
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);
  void setDisplayRotation(int rot) { (void)rot; }   // fixed landscape panel (CAP_ROTATABLE 0)
  void startFrame() {}
  void startFrame(ColorVal bkg) override { P4PanelPainter::startFrame(bkg); }
  void endFrame()   {}

  bool isOn() override { return _on; }
  void turnOn() override;
  void turnOff() override;
  // 0 = backlight off, 1..255 = PWM duty band (Elecrow's 0..100 -> 200..2000 of 2047 curve).
  void setBrightness(uint8_t b);
protected:
  void writeNativeRGB565(int x, int y, int w, int h, const uint16_t* px) override;
};
