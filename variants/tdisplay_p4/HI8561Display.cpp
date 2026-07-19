// SPDX-License-Identifier: GPL-3.0-or-later
//
// HI8561 TFT-LCD DisplayDriver for the LilyGo T-Display P4 (LCD SKU). Mirrors RM69A10Display.cpp
// almost line-for-line — the ESP32-P4 MIPI-DSI bring-up is identical; only the vendor panel driver,
// the 540x1168 resolution and the DPI timing differ. The vendor panel driver lives in the vendored
// esp_lcd_hi8561.{h,cpp} (Espressif's Apache-2.0 esp_lcd_hi8561, from Waveshare-ESP32-components,
// unmodified except the .c->.cpp rename for our variants/ glob). The 540x1168 @ 48 MHz DPI timing is
// the vendor's HI8561_540_1168_PANEL_60HZ_DPI_CONFIG macro (hand-inlined here so num_fbs / pixel
// format stay explicit and this file diffs cleanly against RM69A10Display.cpp). The DSI-PHY LDO is
// kept at the tested-on-the-board 1.83 V (a deprecated LilyGo P4 bin notes "changed MIPI voltage
// domain to 1.8V" — same rail for both panels), NOT the 2.5 V a generic HI8561 eval board uses.
#if defined(HAS_TDISPLAY_P4)
#include "HI8561Display.h"
#include <Arduino.h>
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "Xl9535.h"                 // SCREEN_RST lives on the expander (reset_gpio_num = -1)
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"       // esp_lcd_panel_io_tx_param (runtime brightness 0x51)
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_hi8561.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// The DPI panel copies each draw_bitmap band into the internal frame buffer ASYNCHRONOUSLY
// (DMA2D) and fires on_color_trans_done when the copy is complete. writePixelsRGB565 reuses a
// single upscale scratch buffer (and LVGL reuses its draw buffer) on the very next flush, so
// without waiting for that copy the next band overwrites the source mid-transfer — corrupting
// the frame buffer as horizontal black/garbage bands. This binary semaphore makes each flush
// synchronous: draw_bitmap -> wait for done. (Same contract as RM69A10Display.)
static SemaphoreHandle_t s_flush_sem = nullptr;
static bool IRAM_ATTR hi8561TransDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
  BaseType_t hpw = pdFALSE;
  if (s_flush_sem) xSemaphoreGiveFromISR(s_flush_sem, &hpw);
  return hpw == pdTRUE;
}

// HI8561 TFT-LCD, 540x1168 portrait. DPI timing = vendor HI8561_540_1168_PANEL_60HZ_DPI_CONFIG.
#define HI_W        540
#define HI_H        1168
// UI scale: the panel is high-DPI, so LVGL renders at 1/HI_UI_SCALE and writePixelsRGB565
// nearest-neighbour-upscales each flush to fill the native panel — whole UI HI_UI_SCALE x bigger,
// uniformly. UITask's HAS_TDP4_LCD LVGL res + the HI8561-touch scale must match (HI_W/HI_UI_SCALE).
#ifndef HI_UI_SCALE
#define HI_UI_SCALE 2
#endif
#define HI_DPI_MHZ  48              // vendor HI8561_540_1168 DPI clock
#define HI_LANES    2
#define HI_BITRATE  1000            // Mbps per lane (vendor HI8561_PANEL_BUS_DSI_2CH_CONFIG)
#define HI_HSYNC    20              // hsync_pulse_width
#define HI_HBP      40              // hsync_back_porch
#define HI_HFP      20              // hsync_front_porch
#define HI_VSYNC    2               // vsync_pulse_width
#define HI_VBP      12              // vsync_back_porch
#define HI_VFP      200             // vsync_front_porch
// ESP32-P4 MIPI-DSI PHY internal LDO: channel 3 @ 1.83V — the tested LilyGo T-Display P4 value
// (same rail the RM69A10 SKU uses; the board's MIPI voltage domain is 1.8V, per LilyGo).
#define DSI_LDO_CHAN     3
#define DSI_LDO_MV       1830

static esp_ldo_channel_handle_t s_ldo = nullptr;

bool HI8561Display::begin() {
  // 1. Power the DSI PHY via the P4 internal LDO (BEFORE creating the DSI bus).
  esp_ldo_channel_config_t ldo_cfg = { .chan_id = DSI_LDO_CHAN, .voltage_mv = DSI_LDO_MV };
  if (esp_ldo_acquire_channel(&ldo_cfg, &s_ldo) != ESP_OK) {
    Serial.println("[HI8561] LDO acquire fail"); return false;
  }
  delay(100);

  // 1b. Panel hardware reset via the XL9535 (reset_gpio_num = -1 so esp_lcd won't do it). Same
  // HIGH -> LOW -> HIGH / 200 ms order as the RM69A10 SKU, done AFTER the LDO and BEFORE the DSI bus.
  xl9535.write(Xl9535::IO_SCREEN_RST, true);  delay(200);
  xl9535.write(Xl9535::IO_SCREEN_RST, false); delay(200);
  xl9535.write(Xl9535::IO_SCREEN_RST, true);  delay(200);

  // 2. DSI bus (initialises the DSI PHY).
  esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
  esp_lcd_dsi_bus_config_t bus_cfg = {
    .bus_id = 0, .num_data_lanes = HI_LANES,
    .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT, .lane_bit_rate_mbps = HI_BITRATE,
  };
  if (esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus) != ESP_OK) { Serial.println("[HI8561] dsi_bus fail"); return false; }

  // 3. DBI IO (LCD command channel).
  esp_lcd_panel_io_handle_t dbi_io = nullptr;
  esp_lcd_dbi_io_config_t dbi_cfg = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
  if (esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io) != ESP_OK) { Serial.println("[HI8561] dbi_io fail"); return false; }

  // 4. DPI (video) config — vendor HI8561_540_1168_PANEL_60HZ_DPI_CONFIG values, inlined.
  esp_lcd_dpi_panel_config_t dpi_cfg = {
    .virtual_channel = 0,
    .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
    .dpi_clock_freq_mhz = HI_DPI_MHZ,
    .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
    .num_fbs = 1,
    .video_timing = {
      .h_size = HI_W, .v_size = HI_H,
      .hsync_pulse_width = HI_HSYNC, .hsync_back_porch = HI_HBP, .hsync_front_porch = HI_HFP,
      .vsync_pulse_width = HI_VSYNC, .vsync_back_porch = HI_VBP, .vsync_front_porch = HI_VFP,
    },
    .flags = { .use_dma2d = true },
  };

  // 5. HI8561 vendor panel (reset via the XL9535 -> reset_gpio_num=-1; use the driver's default init).
  hi8561_vendor_config_t vendor = {
    .init_cmds = nullptr, .init_cmds_size = 0,
    .mipi_config = { .dsi_bus = dsi_bus, .dpi_config = &dpi_cfg },
  };
  esp_lcd_panel_dev_config_t dev = {
    .reset_gpio_num = -1,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 16,
    .vendor_config = &vendor,
  };
  if (esp_lcd_new_panel_hi8561(dbi_io, &dev, &_panel) != ESP_OK) { Serial.println("[HI8561] new_panel fail"); return false; }

  esp_lcd_panel_reset(_panel);
  if (esp_lcd_panel_init(_panel) != ESP_OK) { Serial.println("[HI8561] panel_init fail"); return false; }
  esp_lcd_panel_disp_on_off(_panel, true);

  // Brightness: the HI8561's default init does SLPOUT/DISPON but leaves brightness unset; drive it at
  // runtime via DCS 0x51 (the TDDI's integrated backlight — no GPIO). Keep the DBI IO handle so
  // setBrightness() can drive it live (CC slider); start at max so the LCD actually lights.
  _dbi_io = dbi_io;
  setBrightness(255);
  _on = true;

  // Signal each draw_bitmap's frame-buffer copy completion so writePixelsRGB565 can wait for it.
  s_flush_sem = xSemaphoreCreateBinary();
  esp_lcd_dpi_panel_event_callbacks_t cbs = {};
  cbs.on_color_trans_done = hi8561TransDone;
  esp_lcd_dpi_panel_register_event_callbacks(_panel, &cbs, nullptr);

  // Clear the panel framebuffer to black once, so nothing stale shows before LVGL's first flush.
  {
    const int SH = 32;
    uint16_t* strip = (uint16_t*)heap_caps_calloc((size_t)HI_W * SH, 2, MALLOC_CAP_SPIRAM);
    if (strip) {
      for (int y0 = 0; y0 < HI_H; y0 += SH) {
        int hh = (y0 + SH <= HI_H) ? SH : (HI_H - y0);
        esp_lcd_panel_draw_bitmap(_panel, 0, y0, HI_W, y0 + hh, strip);
        if (s_flush_sem) xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(100));   // let the copy finish before reusing/freeing strip
      }
      free(strip);
    }
  }

  Serial.printf("[HI8561] up %dx%d\n", HI_W, HI_H);
  return true;
}

void HI8561Display::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_panel || !pixels || w <= 0 || h <= 0) return;
#if HI_UI_SCALE > 1
  // Nearest-neighbour upscale the (half-res) LVGL band to the native panel: each source pixel becomes
  // an HI_UI_SCALE x HI_UI_SCALE block. One expanded band -> one draw_bitmap (exclusive end coords).
  const int S = HI_UI_SCALE, dw = w * S, dh = h * S;
  static uint16_t* s_up = nullptr; static size_t s_up_px = 0;
  size_t need = (size_t)dw * dh;
  if (need > s_up_px) {
    if (s_up) heap_caps_free(s_up);
    s_up = (uint16_t*)heap_caps_malloc(need * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_up_px = s_up ? need : 0;
  }
  if (s_up) {
    for (int row = 0; row < h; row++) {
      const uint16_t* src = pixels + (size_t)row * w;
      uint16_t* d0 = s_up + (size_t)(row * S) * dw;                 // first of S output rows
      for (int i = 0; i < w; i++) { uint16_t c = src[i]; uint16_t* p = d0 + i * S; for (int sx = 0; sx < S; sx++) p[sx] = c; }
      for (int sy = 1; sy < S; sy++) memcpy(d0 + (size_t)sy * dw, d0, (size_t)dw * sizeof(uint16_t));
    }
    esp_lcd_panel_draw_bitmap(_panel, x * S, y * S, x * S + dw, y * S + dh, s_up);
    if (s_flush_sem) xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(100));   // wait for the FB copy before the next flush reuses s_up
    return;
  }
  // fall through to an unscaled draw if the scratch alloc failed (tiny UI, but visible)
#endif
  // exclusive end coords (esp_lcd_panel_draw_bitmap contract).
  esp_lcd_panel_draw_bitmap(_panel, x, y, x + w, y + h, pixels);
  if (s_flush_sem) xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(100));   // wait for the FB copy before LVGL reuses this buffer
}

void HI8561Display::setBrightness(uint8_t b) {
  if (!_dbi_io) return;
  esp_lcd_panel_io_tx_param(_dbi_io, 0x51, &b, 1);   // DCS SET_DISPLAY_BRIGHTNESS
}

void HI8561Display::turnOn()  { if (_panel) esp_lcd_panel_disp_on_off(_panel, true);  _on = true;  }
void HI8561Display::turnOff() { if (_panel) esp_lcd_panel_disp_on_off(_panel, false); _on = false; }

#endif  // HAS_TDISPLAY_P4
