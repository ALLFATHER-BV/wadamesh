// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_TDISPLAY_P4)
#include "RM69A10Display.h"
#include <Arduino.h>
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "Xl9535.h"                 // SCREEN_RST lives on the expander (reset_gpio_num = -1)
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"       // esp_lcd_panel_io_tx_param (runtime brightness 0x51)
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_rm69a10.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// The DPI panel copies each draw_bitmap band into the internal frame buffer ASYNCHRONOUSLY
// (DMA2D) and fires on_color_trans_done when the copy is complete. writePixelsRGB565 reuses a
// single upscale scratch buffer (and LVGL reuses its draw buffer) on the very next flush, so
// without waiting for that copy the next band overwrites the source mid-transfer — corrupting
// the frame buffer as horizontal black/garbage bands, worst during transitions (many rapid
// flushes). This binary semaphore makes each flush synchronous: draw_bitmap → wait for done.
static SemaphoreHandle_t s_flush_sem = nullptr;
static bool IRAM_ATTR rm69a10TransDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
  BaseType_t hpw = pdFALSE;
  if (s_flush_sem) xSemaphoreGiveFromISR(s_flush_sem, &hpw);
  return hpw == pdTRUE;
}

// RM69A10 DSI timing — LilyGo rm69a10_driver.h (values confirmed via t_display_p4_config.h).
#define RM_W        568
#define RM_H        1232
// UI scale: the 568x1232 AMOLED is very high-DPI, so LVGL renders at 1/RM_UI_SCALE and
// writePixelsRGB565 nearest-neighbour-upscales each flush to fill the native panel — the whole UI is
// RM_UI_SCALE x bigger, uniformly, with no per-element tuning. UITask's HAS_TDISPLAY_P4 LVGL res +
// the GT9895 touch scale must match (568/RM_UI_SCALE).
#ifndef RM_UI_SCALE
#define RM_UI_SCALE 2
#endif
#define RM_DPI_MHZ  60
#define RM_LANES    2
#define RM_BITRATE  1000            // Mbps per lane — TODO(device): confirm vs LilyGo RM69A10 define
#define RM_HSYNC    50
#define RM_HBP      150
#define RM_HFP      50
#define RM_VSYNC    40
#define RM_VBP      120
#define RM_VFP      80
// ESP32-P4 MIPI-DSI PHY internal LDO: channel 3 @ 1.83V. This is the tested LilyGo/Meck-P4 value —
// the "standard" 2500 mV left the RM69A10 dark (the panel's DSI rail wants 1.83 V here).
#define DSI_LDO_CHAN     3
#define DSI_LDO_MV       1830

static esp_ldo_channel_handle_t s_ldo = nullptr;

bool RM69A10Display::begin() {
  // 1. Power the DSI PHY via the P4 internal LDO (BEFORE creating the DSI bus).
  esp_ldo_channel_config_t ldo_cfg = { .chan_id = DSI_LDO_CHAN, .voltage_mv = DSI_LDO_MV };
  if (esp_ldo_acquire_channel(&ldo_cfg, &s_ldo) != ESP_OK) {
    Serial.println("[RM69A10] LDO acquire fail"); return false;
  }
  delay(100);

  // 1b. Panel hardware reset via the XL9535 (reset_gpio_num = -1 so esp_lcd won't do it). The
  // tested Meck-P4 order is HIGH -> LOW -> HIGH with 200 ms settling, done AFTER the LDO and BEFORE
  // the DSI bus. (An earlier attempt reset after the bus / with short delays kept the panel dark.)
  xl9535.write(Xl9535::IO_SCREEN_RST, true);  delay(200);
  xl9535.write(Xl9535::IO_SCREEN_RST, false); delay(200);
  xl9535.write(Xl9535::IO_SCREEN_RST, true);  delay(200);

  // 2. DSI bus (initialises the DSI PHY).
  esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
  esp_lcd_dsi_bus_config_t bus_cfg = {
    .bus_id = 0, .num_data_lanes = RM_LANES,
    .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT, .lane_bit_rate_mbps = RM_BITRATE,
  };
  if (esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus) != ESP_OK) { Serial.println("[RM69A10] dsi_bus fail"); return false; }

  // 3. DBI IO (LCD command channel).
  esp_lcd_panel_io_handle_t dbi_io = nullptr;
  esp_lcd_dbi_io_config_t dbi_cfg = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
  if (esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io) != ESP_OK) { Serial.println("[RM69A10] dbi_io fail"); return false; }

  // 4. DPI (video) config.
  esp_lcd_dpi_panel_config_t dpi_cfg = {
    .virtual_channel = 0,
    .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
    .dpi_clock_freq_mhz = RM_DPI_MHZ,
    .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
    .num_fbs = 1,
    .video_timing = {
      .h_size = RM_W, .v_size = RM_H,
      .hsync_pulse_width = RM_HSYNC, .hsync_back_porch = RM_HBP, .hsync_front_porch = RM_HFP,
      .vsync_pulse_width = RM_VSYNC, .vsync_back_porch = RM_VBP, .vsync_front_porch = RM_VFP,
    },
    .flags = { .use_dma2d = true },
  };

  // 5. RM69A10 vendor panel (reset via the XL9535, done in powerOnSequence -> reset_gpio_num=-1).
  rm69a10_vendor_config_t vendor = {
    .init_cmds = nullptr, .init_cmds_size = 0,   // use the driver's vendor_specific_init_default
    .mipi_config = { .dsi_bus = dsi_bus, .dpi_config = &dpi_cfg, .lane_num = RM_LANES },
  };
  esp_lcd_panel_dev_config_t dev = {
    .reset_gpio_num = -1,
    .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 16,
    .vendor_config = &vendor,
  };
  if (esp_lcd_new_panel_rm69a10(dbi_io, &dev, &_panel) != ESP_OK) { Serial.println("[RM69A10] new_panel fail"); return false; }

  esp_lcd_panel_reset(_panel);
  if (esp_lcd_panel_init(_panel) != ESP_OK) { Serial.println("[RM69A10] panel_init fail"); return false; }
  esp_lcd_panel_disp_on_off(_panel, true);

  // Brightness: the init array ships 0x51=0 (placeholder), and RM69A10 brightness is driven at
  // runtime — Meck-P4 ramps set_rm69a10_brightness() after display-on. Keep the DBI IO handle so
  // setBrightness() can drive DCS 0x51 live (CC slider); start at max so the AMOLED actually emits.
  _dbi_io = dbi_io;
  setBrightness(255);
  _on = true;

  // Signal each draw_bitmap's frame-buffer copy completion so writePixelsRGB565 can wait for it
  // (single scratch/draw buffer reuse — see the note at s_flush_sem).
  s_flush_sem = xSemaphoreCreateBinary();
  esp_lcd_dpi_panel_event_callbacks_t cbs = {};
  cbs.on_color_trans_done = rm69a10TransDone;
  esp_lcd_dpi_panel_register_event_callbacks(_panel, &cbs, nullptr);

  // Clear the panel framebuffer to black once, so nothing stale shows before LVGL's first flush.
  {
    const int SH = 32;
    uint16_t* strip = (uint16_t*)heap_caps_calloc((size_t)RM_W * SH, 2, MALLOC_CAP_SPIRAM);
    if (strip) {
      for (int y0 = 0; y0 < RM_H; y0 += SH) {
        int hh = (y0 + SH <= RM_H) ? SH : (RM_H - y0);
        esp_lcd_panel_draw_bitmap(_panel, 0, y0, RM_W, y0 + hh, strip);
        if (s_flush_sem) xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(100));   // let the copy finish before reusing/freeing strip
      }
      free(strip);
    }
  }

  Serial.printf("[RM69A10] up %dx%d\n", RM_W, RM_H);
  return true;
}

void RM69A10Display::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_panel || !pixels || w <= 0 || h <= 0) return;
#if RM_UI_SCALE > 1
  // Nearest-neighbour upscale the (half-res) LVGL band to the native panel: each source pixel becomes
  // an RM_UI_SCALE x RM_UI_SCALE block. One expanded band -> one draw_bitmap (exclusive end coords).
  const int S = RM_UI_SCALE, dw = w * S, dh = h * S;
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

void RM69A10Display::setBrightness(uint8_t b) {
  if (!_dbi_io) return;
  esp_lcd_panel_io_tx_param(_dbi_io, 0x51, &b, 1);   // DCS SET_DISPLAY_BRIGHTNESS
}

void RM69A10Display::turnOn()  { if (_panel) esp_lcd_panel_disp_on_off(_panel, true);  _on = true;  }
void RM69A10Display::turnOff() { if (_panel) esp_lcd_panel_disp_on_off(_panel, false); _on = false; }

#endif  // HAS_TDISPLAY_P4
