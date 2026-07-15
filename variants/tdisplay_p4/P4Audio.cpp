// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_TDISPLAY_P4)
// ES8311 register code adapted from Espressif's esp-bsp components/es8311 (Apache-2.0,
// SPDX-FileCopyrightText: 2015-2026 Espressif Systems) — trimmed to the playback path this
// board needs and re-based onto Arduino Wire1, because the original talks the LEGACY
// driver/i2c API which aborts at boot when linked next to arduino-esp32 3.x's new-driver
// Wire. Clocking is fixed at MCLK = 256×fs from the MCLK pin, so one static divider row
// ({4096000, 16000}: pre_div 1, mult 1x, adc/dac_div 1, bclk_div 4, osr 0x10) covers us.
#include "P4Audio.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// t_display_p4_config.h (LilyGo): ES8311 on IIC_2, I2S pins below.
#define P4A_I2C_SDA   20
#define P4A_I2C_SCL   21
#define P4A_ADDR      0x18
#define P4A_MCLK      13
#define P4A_BCLK      12
#define P4A_WS        9
#define P4A_DOUT      10

#define P4A_RATE      16000
#define P4A_MCLK_HZ   (P4A_RATE * 256)

// ---- minimal ES8311 register layer (names == hex addresses in the datasheet map) ----
static bool es8311Write(uint8_t reg, uint8_t val) {
  Wire1.beginTransmission(P4A_ADDR);
  Wire1.write(reg);
  Wire1.write(val);
  return Wire1.endTransmission() == 0;
}
static bool es8311Read(uint8_t reg, uint8_t* val) {
  Wire1.beginTransmission(P4A_ADDR);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((uint8_t)P4A_ADDR, (uint8_t)1) != 1) return false;
  *val = Wire1.read();
  return true;
}

// es8311_init() for clk{mclk_from_mclk_pin, 4.096 MHz, 16 kHz} + 16-bit I2S slave + the
// "NOT default" analog power-ups, then DAC volume + unmute. Register values follow the
// esp-bsp driver verbatim.
static bool es8311Init() {
  uint8_t v;
  if (!es8311Write(0x00, 0x1F)) return false;      // reset
  vTaskDelay(pdMS_TO_TICKS(20));
  es8311Write(0x00, 0x00);
  es8311Write(0x00, 0x80);                          // power-on, CSM on

  es8311Write(0x01, 0x3F);                          // all clocks on, MCLK from MCLK pin
  if (es8311Read(0x06, &v)) es8311Write(0x06, v & ~0x20);   // SCLK not inverted

  // divider row {mclk=4096000, rate=16000}: pre_div=1 mult=1x adc/dac_div=1 fs_mode=0
  // lrck 0x00/0xFF bclk_div=4 osr 0x10/0x10  (es8311_sample_frequency_config)
  if (es8311Read(0x02, &v)) es8311Write(0x02, (uint8_t)((v & 0x07) | ((1 - 1) << 5) | (0 << 3)));
  es8311Write(0x03, (uint8_t)((0 << 6) | 0x10));    // fs_mode | adc_osr
  es8311Write(0x04, 0x10);                          // dac_osr
  es8311Write(0x05, (uint8_t)(((1 - 1) << 4) | (1 - 1)));   // adc_div | dac_div
  if (es8311Read(0x06, &v)) es8311Write(0x06, (uint8_t)((v & 0xE0) | (4 - 1)));   // bclk_div
  if (es8311Read(0x07, &v)) es8311Write(0x07, (uint8_t)((v & 0xC0) | 0x00));      // lrck_h
  es8311Write(0x08, 0xFF);                          // lrck_l

  if (es8311Read(0x00, &v)) es8311Write(0x00, v & 0xBF);    // slave serial port
  es8311Write(0x09, (uint8_t)(3 << 2));             // SDP-in 16-bit
  es8311Write(0x0A, (uint8_t)(3 << 2));             // SDP-out 16-bit

  es8311Write(0x0D, 0x01);                          // power up analog circuitry
  es8311Write(0x0E, 0x02);                          // enable analog PGA / ADC modulator
  es8311Write(0x12, 0x00);                          // power up DAC
  es8311Write(0x13, 0x10);                          // enable output to HP drive
  es8311Write(0x1C, 0x6A);                          // ADC EQ bypass, cancel DC offset
  es8311Write(0x37, 0x08);                          // bypass DAC equalizer

  es8311Write(0x32, (uint8_t)(85 * 256 / 100 - 1)); // DAC volume ~85% (loudness = amplitude)
  if (es8311Read(0x31, &v)) es8311Write(0x31, v & ~0x60);   // unmute
  return true;
}

static i2s_chan_handle_t  s_tx  = nullptr;
static SemaphoreHandle_t  s_mtx = nullptr;   // one player at a time
static bool s_ready = false, s_failed = false;

bool p4AudioReady() {
  if (s_ready)  return true;
  if (s_failed) return false;
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_ready || s_failed) { xSemaphoreGive(s_mtx); return s_ready; }

  Wire1.begin(P4A_I2C_SDA, P4A_I2C_SCL, 400000);
  Wire1.beginTransmission(P4A_ADDR);
  bool present = (Wire1.endTransmission() == 0);
  if (!present || !es8311Init()) {
    printf("[P4AUDIO] ES8311 %s\n", present ? "init failed" : "not found @0x18");
    s_failed = true;
    xSemaphoreGive(s_mtx);
    return false;
  }

  i2s_chan_config_t ch = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  ch.auto_clear = true;                       // underrun sends silence, not a looping last buffer
  if (i2s_new_channel(&ch, &s_tx, nullptr) != ESP_OK) {
    s_failed = true;
    xSemaphoreGive(s_mtx);
    return false;
  }
  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(P4A_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = (gpio_num_t)P4A_MCLK,
      .bclk = (gpio_num_t)P4A_BCLK,
      .ws   = (gpio_num_t)P4A_WS,
      .dout = (gpio_num_t)P4A_DOUT,
      .din  = GPIO_NUM_NC,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;   // codec dividers assume 256×fs
  if (i2s_channel_init_std_mode(s_tx, &std_cfg) != ESP_OK ||
      i2s_channel_enable(s_tx) != ESP_OK) {
    printf("[P4AUDIO] I2S init failed\n");
    s_failed = true;
    xSemaphoreGive(s_mtx);
    return false;
  }
  printf("[P4AUDIO] ES8311 up (16 kHz, MCLK 256x)\n");
  s_ready = true;
  xSemaphoreGive(s_mtx);
  return true;
}

void p4AudioTone(int freq_hz, int duration_ms, int amplitude) {
  if (amplitude <= 0 || freq_hz <= 0 || duration_ms <= 0) return;
  if (!p4AudioReady()) return;
  if (amplitude > 30000) amplitude = 30000;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  const int total = P4A_RATE * duration_ms / 1000;
  const int fade  = P4A_RATE * 4 / 1000;             // 4 ms in/out fade kills the click
  static int16_t buf[256 * 2];                       // stereo frames, chunked
  int done = 0;
  float phase = 0.0f;
  const float step = 2.0f * (float)M_PI * (float)freq_hz / (float)P4A_RATE;
  while (done < total) {
    int n = 0;
    for (; n < 256 && done < total; ++n, ++done) {
      float env = 1.0f;
      if (done < fade)              env = (float)done / fade;
      else if (done > total - fade) env = (float)(total - done) / fade;
      int16_t s = (int16_t)((float)amplitude * env * sinf(phase));
      phase += step;
      if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
      buf[2 * n] = buf[2 * n + 1] = s;
    }
    size_t wr = 0;
    i2s_channel_write(s_tx, buf, (size_t)n * 2 * sizeof(int16_t), &wr, pdMS_TO_TICKS(300));
  }
  xSemaphoreGive(s_mtx);
}

#endif  // HAS_TDISPLAY_P4
