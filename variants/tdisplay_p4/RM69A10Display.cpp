// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_TDISPLAY_P4)
#include "RM69A10Display.h"
#include <Arduino.h>
#include "esp_heap_caps.h"
#include "esp_cache.h"   // esp_cache_msync — flush the memset framebuffer out to the DSI DMA
#include "esp_ldo_regulator.h"
#include "Xl9535.h"                 // SCREEN_RST lives on the expander (reset_gpio_num = -1)
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"       // esp_lcd_panel_io_tx_param (runtime brightness 0x51)
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_rm69a10.h"
#if defined(TDP4_POKE_TRACE)
#include "soc/mipi_dsi_host_struct.h"   // MIPI_DSI_HOST — raw host error status (poke-trace poller)
#include "freertos/task.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// The DPI panel copies each draw_bitmap band into the internal frame buffer ASYNCHRONOUSLY
// (DMA2D) and fires on_color_trans_done when the copy is complete. writePixelsRGB565 reuses a
// single upscale scratch buffer (and LVGL reuses its draw buffer) on the very next flush, so
// without waiting for that copy the next band overwrites the source mid-transfer — corrupting
// the frame buffer as horizontal black/garbage bands, worst during transitions (many rapid
// flushes). This binary semaphore makes each flush synchronous: draw_bitmap → wait for done.
static SemaphoreHandle_t s_flush_sem = nullptr;
#if defined(TDP4_POKE_TRACE)
// #167 hunt, round 3: the esp_lcd driver watches ONLY the bridge-FIFO underrun (proven silent
// during visible flashes). The DSI HOST has its own never-read error latches: int_st0 = the
// panel acking packets WITH ERRORS, int_st1 = ECC/CRC/timeout/payload-write errors INCLUDING
// dpi_buff_pld_under (the host payload buffer underflowing -- a different FIFO than the bridge).
// Both are read-clear, so a 25 ms poll gives per-event capture. If these latch when the screen
// flashes, we have a hardware flash-detector and can bisect subsystems without eyes on the panel.
static void dsiErrPollTask(void*) {
  for (;;) {
    const uint32_t st0 = MIPI_DSI_HOST.int_st0.val;   // RC
    const uint32_t st1 = MIPI_DSI_HOST.int_st1.val;   // RC
    if (st0 || st1) printf("[DSIERR] %lu st0=%08X st1=%08X\n", (unsigned long)millis(), (unsigned)st0, (unsigned)st1);
    vTaskDelay(pdMS_TO_TICKS(25));
  }
}
#endif
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
// Lane bit rate. LilyGo/Meck ship 1000 Mbps (confirmed against both sources 2026-07-31), and the
// panel needs only ~550 Mbps of it at DPI 60 MHz / RGB565 / 2 lanes -- the rest is eye margin.
// #167 made that margin matter: SD-card write bursts load the P4's on-chip LDO4 and disturb the
// sibling LDO3 that powers the DSI PHY, and at a 1 ns unit interval that wobble was enough to
// glitch the link -- the panel loses a frame to a whole-screen blue flash (proven by elimination:
// no flash with the card out or unwritten; framebuffer/underrun/host-error/DCS all measured clean;
// identical timings+init to the flash-free Meck build). A 750 Mbps derate was tried against #167 and changed nothing
// (the disturbance reaches the panel around the link, not through it), so this stays at the
// vendor value. Overridable per build: -DRM_BITRATE_MBPS=<n>.
#ifndef RM_BITRATE_MBPS
#define RM_BITRATE_MBPS 1000
#endif
#define RM_BITRATE  RM_BITRATE_MBPS
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

  // Blank the framebuffer BEFORE switching the panel on. It is freshly allocated PSRAM, so it holds
  // garbage; turning the panel on first meant the DSI streamed that garbage to the screen until the
  // clear below finished -- the colour flash testers see before the boot splash (#167). Write the
  // buffer directly (memset + a cache flush to make it visible to the DSI's DMA) instead of pushing
  // strips through draw_bitmap: it is one pass with the panel still dark, and it avoids a burst of
  // 39 back-to-back DMA transfers into a buffer the panel is scanning out.
  void* fb = nullptr;
  if (esp_lcd_dpi_panel_get_frame_buffer(_panel, 1, &fb) == ESP_OK && fb) {
    const size_t fb_bytes = (size_t)RM_W * RM_H * 2;
    memset(fb, 0, fb_bytes);
    esp_cache_msync(fb, fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  }
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

  #if defined(TDP4_POKE_TRACE)
  xTaskCreatePinnedToCore(dsiErrPollTask, "dsierr", 3072, nullptr, 3, nullptr, 1);
#endif
  Serial.printf("[RM69A10] up %dx%d\n", RM_W, RM_H);
  return true;
}

void RM69A10Display::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* pixels) {
  if (!_panel || !pixels || w <= 0 || h <= 0) return;
#if defined(TDP4_FLUSH_TRACE)
  // Opt-in flush tracer for whole-screen colour-flash reports (#167). OFF unless the build adds
  // -DTDP4_FLUSH_TRACE, so it costs nothing normally. Logs any large, mostly-single-colour band
  // with its RGB565 value: if the flash is LVGL painting something you see a burst carrying that
  // colour (brand teal 0x15B6A6 -> 0x15B4); if the screen visibly flashes and nothing logs, the
  // cause is below LVGL (panel/DSI) and hunting in the UI is wasted effort. Note when using it
  // that the per-band uniformity scan slows the flush path, which can mask a timing-sensitive
  // bug — always confirm a fix on a build WITHOUT this enabled.
  {
    const long px = (long)w * h;
    if (px > 1500) {                       // one full LVGL band is 284*24 = 6816 px
      const uint16_t c0 = pixels[0];
      long same = 0;
      for (long i = 0; i < px; i += 37) { if (pixels[i] == c0) ++same; }   // sparse uniformity probe
      const long probes = (px + 36) / 37;
      // Log the colour whenever a large band is mostly ONE colour. A whole-screen flash shows
      // up as a burst of these in the same millisecond range, all carrying the flash colour
      // (brand teal 0x15B6A6 -> rgb565 0x15B4). Rate-limited so a normal repaint storm can't
      // flood the console and perturb what we are measuring.
      static uint32_t s_last_log = 0; static int s_burst = 0;
      const uint32_t now_ms = (uint32_t)millis();
      if (now_ms - s_last_log > 500) { s_burst = 0; s_last_log = now_ms; }
      if (same * 10 >= probes * 9 && s_burst < 12) {
        ++s_burst;
        Serial.printf("[FLUSHTRACE] %lu x=%d y=%d w=%d h=%d rgb565=0x%04X\n",
                      (unsigned long)now_ms, x, y, w, h, (unsigned)c0);
      }
    }
  }
#endif
#if RM_UI_SCALE > 1
  // Nearest-neighbour upscale the (half-res) LVGL band to the native panel: each source pixel becomes
  // an RM_UI_SCALE x RM_UI_SCALE block. One expanded band -> one draw_bitmap (exclusive end coords).
  const int S = RM_UI_SCALE, dw = w * S, dh = h * S;
  // The scratch band lives in INTERNAL DMA RAM, not PSRAM. This panel is a DPI/DSI one with a
  // single ~1.4 MB framebuffer that can only live in PSRAM, and the DSI DMA streams that
  // framebuffer out to the glass CONTINUOUSLY. Every draw_bitmap below is a copy INTO that
  // framebuffer, so with the scratch also in PSRAM a flush put three streams on the same bus at
  // once: the scratch read, the framebuffer write, and the DSI's own read. Anything else that
  // wants PSRAM at that moment — notably the priority-10 "lora_rx" drain task, which wakes on
  // every received packet — can then starve the DSI read, and a DPI underrun shows up as a
  // whole-screen colour flash for one frame. That is the P4 "flicker on every RX" report
  // (issue #167): it appears on any screen, because it is a display-bus problem and nothing to
  // do with what is being drawn. Internal RAM removes the scratch read from the PSRAM bus.
  // Cost is bounded and small: one LVGL band is 284x24 (LV_DRAW_BUF_LINES), so upscaled 2x this
  // is 568*48*2 = ~53 KB out of the P4's 768 KB SRAM. PSRAM stays as the fallback so a
  // fragmented heap degrades to the old behaviour instead of dropping to the unscaled path.
  static uint16_t* s_up = nullptr; static size_t s_up_px = 0;
  size_t need = (size_t)dw * dh;
  if (need > s_up_px) {
    if (s_up) heap_caps_free(s_up);
    s_up = (uint16_t*)heap_caps_malloc(need * sizeof(uint16_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_up) s_up = (uint16_t*)heap_caps_malloc(need * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
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
#if defined(TDP4_POKE_TRACE)
  // #167 hunt: DCS 0x51 is a low-power command injected into the LIVE video stream -- if the
  // panel glitches a frame per command, these lines will pace the visible flashes exactly.
  printf("[BRI] %lu val=%u\n", (unsigned long)millis(), b);
#endif
  esp_lcd_panel_io_tx_param(_dbi_io, 0x51, &b, 1);   // DCS SET_DISPLAY_BRIGHTNESS
}

#if defined(TDP4_POKE_TRACE)
void RM69A10Display::turnOn()  { printf("[DSP] %lu on\n",  (unsigned long)millis()); if (_panel) esp_lcd_panel_disp_on_off(_panel, true);  _on = true;  }
void RM69A10Display::turnOff() { printf("[DSP] %lu off\n", (unsigned long)millis()); if (_panel) esp_lcd_panel_disp_on_off(_panel, false); _on = false; }
#else
void RM69A10Display::turnOn()  { if (_panel) esp_lcd_panel_disp_on_off(_panel, true);  _on = true;  }
void RM69A10Display::turnOff() { if (_panel) esp_lcd_panel_disp_on_off(_panel, false); _on = false; }
#endif

#endif  // HAS_TDISPLAY_P4
