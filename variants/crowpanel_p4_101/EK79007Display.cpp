// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_CROWPANEL_P4_101)
#include "EK79007Display.h"
#include <Arduino.h>
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ek79007.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Panel geometry + DSI timing: Elecrow's validated BSP (idf-code Lesson07 bsp_illuminate.c, identical
// on V1.0, V1.1 and V1.2) and the V1.2 hardware driver guide.
#define CP_W            1024
#define CP_H            600
#define CP_LANES        2
#define CP_LANE_MBPS    900
#define CP_DPI_MHZ      51
#define CP_HBP          160
#define CP_HPW          70
#define CP_HFP          160
#define CP_VBP          23
#define CP_VPW          10
#define CP_VFP          12
// J21 LCD_RESET is wired to GPIO41 (schematic, all revisions, pulled up through R69), but Elecrow's
// working BSP passes reset_gpio_num = -1 and relies on the power-on reset. Kept the same here; build
// with -DCP_LCD_RESET_GPIO=41 to let the driver pulse it if the panel stays dark after a restart.
#ifndef CP_LCD_RESET_GPIO
#define CP_LCD_RESET_GPIO -1
#endif

// Backlight: MT9201 EN on GPIO31, LEDC 30 kHz / 11-bit like Elecrow (PLL-divided clock; the 40 MHz
// crystal cannot reach 30 kHz at 11 bits). A timer/channel pair the Arduino ledc layer does not hand
// out first, so the two cannot collide.
#define CP_BL_GPIO      31
#define CP_BL_HZ        30000
#define CP_BL_TIMER     LEDC_TIMER_3
#define CP_BL_CHANNEL   LEDC_CHANNEL_7

static SemaphoreHandle_t s_flush_sem = nullptr;
static bool s_bl_ok = false;

// The DPI panel copies each draw_bitmap into its PSRAM framebuffer asynchronously (DMA2D). LVGL
// reuses its band on the next flush, so every flush waits for this (see RM69A10Display.cpp).
static bool IRAM_ATTR ek79007TransDone(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
  BaseType_t hpw = pdFALSE;
  if (s_flush_sem) xSemaphoreGiveFromISR(s_flush_sem, &hpw);
  return hpw == pdTRUE;
}

static void backlightInit() {
  ledc_timer_config_t t = {};
  t.speed_mode = LEDC_LOW_SPEED_MODE;
  t.duty_resolution = LEDC_TIMER_11_BIT;
  t.timer_num = CP_BL_TIMER;
  t.freq_hz = CP_BL_HZ;
  t.clk_cfg = LEDC_USE_PLL_DIV_CLK;
  if (ledc_timer_config(&t) != ESP_OK) { Serial.println("[EK79007] backlight timer fail"); return; }
  ledc_channel_config_t c = {};
  c.gpio_num = CP_BL_GPIO;
  c.speed_mode = LEDC_LOW_SPEED_MODE;
  c.channel = CP_BL_CHANNEL;
  c.intr_type = LEDC_INTR_DISABLE;
  c.timer_sel = CP_BL_TIMER;
  c.duty = 0;
  c.hpoint = 0;
  s_bl_ok = (ledc_channel_config(&c) == ESP_OK);
  if (!s_bl_ok) Serial.println("[EK79007] backlight channel fail");
}

static void backlightDuty(uint32_t duty) {
  if (!s_bl_ok) return;
  if (duty > 2047) duty = 2047;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, CP_BL_CHANNEL, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, CP_BL_CHANNEL);
}

bool EK79007Display::begin() {
  // The LDO rails (LDO3 DSI PHY 2.5 V, LDO4 pad domain 3.3 V) are already up: crowpanelP4PowerOn()
  // acquires them before anything else runs (target.cpp).
  backlightInit();   // dark until the first frame is in the framebuffer

  esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
  esp_lcd_dsi_bus_config_t bus_cfg = {};
  bus_cfg.bus_id = 0;
  bus_cfg.num_data_lanes = CP_LANES;
  bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
  bus_cfg.lane_bit_rate_mbps = CP_LANE_MBPS;
  if (esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus) != ESP_OK) { Serial.println("[EK79007] dsi_bus fail"); return false; }

  esp_lcd_panel_io_handle_t dbi_io = nullptr;
  esp_lcd_dbi_io_config_t dbi_cfg = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
  if (esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &dbi_io) != ESP_OK) { Serial.println("[EK79007] dbi_io fail"); return false; }

  esp_lcd_dpi_panel_config_t dpi_cfg = {};
  dpi_cfg.virtual_channel = 0;
  dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi_cfg.dpi_clock_freq_mhz = CP_DPI_MHZ;
  dpi_cfg.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
  dpi_cfg.num_fbs = 1;
  dpi_cfg.video_timing.h_size = CP_W;
  dpi_cfg.video_timing.v_size = CP_H;
  dpi_cfg.video_timing.hsync_back_porch = CP_HBP;
  dpi_cfg.video_timing.hsync_pulse_width = CP_HPW;
  dpi_cfg.video_timing.hsync_front_porch = CP_HFP;
  dpi_cfg.video_timing.vsync_back_porch = CP_VBP;
  dpi_cfg.video_timing.vsync_pulse_width = CP_VPW;
  dpi_cfg.video_timing.vsync_front_porch = CP_VFP;
  dpi_cfg.flags.use_dma2d = true;

  ek79007_vendor_config_t vendor = {};
  vendor.init_cmds = nullptr;          // the component's vendor_specific_init_default (as Elecrow uses)
  vendor.init_cmds_size = 0;
  vendor.mipi_config.dsi_bus = dsi_bus;
  vendor.mipi_config.dpi_config = &dpi_cfg;
  vendor.mipi_config.lane_num = CP_LANES;
  esp_lcd_panel_dev_config_t dev = {};
  dev.reset_gpio_num = CP_LCD_RESET_GPIO;
  dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  dev.bits_per_pixel = 16;
  dev.vendor_config = &vendor;
  if (esp_lcd_new_panel_ek79007(dbi_io, &dev, &_panel) != ESP_OK) { Serial.println("[EK79007] new_panel fail"); return false; }

  esp_lcd_panel_reset(_panel);
  if (esp_lcd_panel_init(_panel) != ESP_OK) { Serial.println("[EK79007] panel_init fail"); return false; }

  // Blank the freshly allocated (garbage) PSRAM framebuffer before the backlight comes on.
  void* fb = nullptr;
  if (esp_lcd_dpi_panel_get_frame_buffer(_panel, 1, &fb) == ESP_OK && fb) {
    const size_t fb_bytes = (size_t)CP_W * CP_H * 2;
    memset(fb, 0, fb_bytes);
    esp_cache_msync(fb, fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  }
  esp_lcd_panel_disp_on_off(_panel, true);   // EK79007 may report "not supported"; harmless

  s_flush_sem = xSemaphoreCreateBinary();
  esp_lcd_dpi_panel_event_callbacks_t cbs = {};
  cbs.on_color_trans_done = ek79007TransDone;
  esp_lcd_dpi_panel_register_event_callbacks(_panel, &cbs, nullptr);

  _on = true;
  setBrightness(_bright);   // light it now so a dark panel reads as a DSI problem, not a backlight one
  Serial.printf("[EK79007] up %dx%d, %d lanes @ %d Mbps, DPI %d MHz\n", CP_W, CP_H, CP_LANES, CP_LANE_MBPS, CP_DPI_MHZ);
  return true;
}

void EK79007Display::writeNativeRGB565(int x, int y, int w, int h, const uint16_t* px) {
  writePixelsRGB565(x, y, w, h, px);
}

void EK79007Display::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_panel || !pixels || w <= 0 || h <= 0) return;
  esp_lcd_panel_draw_bitmap(_panel, x, y, x + w, y + h, pixels);   // exclusive end coords
  if (s_flush_sem) xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(100));
}

void EK79007Display::setBrightness(uint8_t b) {
  _bright = b ? b : _bright;
  if (!_on && b) return;   // remembered; applied by turnOn()
  // Elecrow: duty = pct * 18 + 200 for pct 1..100, i.e. 218..2000 of 2047. Same band on 1..255.
  backlightDuty(b ? 200u + (uint32_t)b * 1800u / 255u : 0u);
}

void EK79007Display::turnOn() {
  _on = true;
  setBrightness(_bright);
}

void EK79007Display::turnOff() {
  _on = false;
  backlightDuty(0);
}

#endif  // HAS_CROWPANEL_P4_101
