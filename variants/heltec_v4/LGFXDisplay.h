// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Heltec WiFi LoRa 32 V4-R8 (+ Expansion Kit V2) — LovyanGFX ST7789 driver.
//
// The V4-R8's 8 MB OCTAL PSRAM claims GPIO33-37, so the Expansion Kit V2 moved
// the TFT off the R2's software-SPI-on-GPIO33 path onto a real hardware SPI bus
// (SPI2_HOST/FSPI: SCK=16, MOSI=15, MISO=45) that it SHARES with the micro-SD
// slot (CS=3). Hardware SPI is therefore mandatory here — hence LovyanGFX with
// bus_shared=true, exactly like the RAK Tap V2 variant. All pins come from the
// PIN_TFT_* build flags. Compiled only for the R8 (guarded in the .cpp).

#include <helpers/ui/DisplayDriver.h>
#include <helpers/RefCountedDigitalPin.h>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// Display SPI write clock. 80 MHz (perf pass 2026-08-20) = parity with the plain V4's
// TFT_eSPI driver; the S3's rungs are 80 / 40 / 26.7 / 20 MHz. Override with
// -D LGFX_SPI_WRITE_HZ=40000000 if a unit shows tearing / garbled bands. Exposed here
// (not just in the .cpp) so Settings -> About can report it.
#ifndef LGFX_SPI_WRITE_HZ
  #define LGFX_SPI_WRITE_HZ 80000000
#endif

class LGFXDisplay : public DisplayDriver {
private:
  lgfx::Panel_ST7789  _panel;
  lgfx::Bus_SPI       _bus;
  lgfx::Light_PWM     _light;
  lgfx::LGFX_Device   _lcd;

  bool _isOn;
  uint16_t _color;
  RefCountedDigitalPin* _periph_power;

  // Async LVGL flush state (see flushBandRGB565): one internal DMA-capable band buffer
  // holding the byte-swapped copy of the LVGL band currently on the wire, and whether a
  // frame-spanning startWrite() transaction is open on the (micro-SD-shared) bus.
  uint16_t* _swap_buf;
  size_t    _swap_px;
  bool      _swap_alloc_failed;
  bool      _frame_open;

public:
  LGFXDisplay(RefCountedDigitalPin* peripher_power = nullptr);
  bool begin();

  // ---- DisplayDriver overrides ----
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

  // ---- LVGL flush entry points ----
  // Synchronous: byte-swap + write, returns once the band is on the wire (LovyanGFX's
  // convert path). Kept for non-LVGL callers and as the fallback of the async path.
  void writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels);
  // Asynchronous (lvglFlush on the R8): swaps the band into an internal DMA buffer, kicks a
  // single DMA and returns immediately so LVGL renders band N+1 while band N drains. `last`
  // = lv_disp_flush_is_last(): waits for the DMA and closes the frame transaction so the
  // shared micro-SD gets the bus back between frames. Falls back to the sync path if the
  // DMA buffer could not be allocated.
  void flushBandRGB565(int x, int y, int w, int h, const uint16_t* pixels, bool last);
  // Wait for any in-flight band DMA and end the frame transaction (no-op if none is open).
  // Every non-LVGL bus user below (sleep, rotation, clear) calls this first.
  void finishFrame();
  // True once the async path has its internal DMA buffer (i.e. LVGL flushes are async);
  // false before the first flush or if internal DRAM was exhausted (sync fallback).
  bool asyncFlushActive() const { return _swap_buf != nullptr; }

  // ---- Hardware panel rotation ----
  void setDisplayRotation(uint8_t r);

  // ---- Anti-burn-in panel sleep (SLPIN/SLPOUT) ----
  // Sends the ST7789 sleep commands over LovyanGFX's OWN SPI2 bus. The shared
  // touchPanelSleep() path in UITask must call this on the R8 instead of its
  // HSPI s_cmd_spi shim — that shim re-routes GPIO16/15 to HSPI and steals the
  // FSPI display pins from LGFX, wedging the bus on wake (frozen last frame).
  void panelSleep(bool sleep);
};
