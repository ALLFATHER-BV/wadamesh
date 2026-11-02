// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// T-LoRa Pager simulator: the legacy ESP-IDF I2S driver API, silent. The Pager's
// notification tones and WAV playback compile and run against it; nothing plays.
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef int i2s_port_t;
#define I2S_NUM_0 0
#define I2S_NUM_1 1
typedef int i2s_mode_t;
#define I2S_MODE_MASTER 1
#define I2S_MODE_TX     4
typedef int i2s_bits_per_sample_t;
#define I2S_BITS_PER_SAMPLE_16BIT 16
typedef int i2s_channel_fmt_t;
#define I2S_CHANNEL_FMT_ONLY_LEFT  4
#define I2S_CHANNEL_FMT_RIGHT_LEFT 0
typedef int i2s_comm_format_t;
#define I2S_COMM_FORMAT_STAND_I2S 1
#define I2S_PIN_NO_CHANGE (-1)

typedef struct {
  i2s_mode_t mode; uint32_t sample_rate; i2s_bits_per_sample_t bits_per_sample;
  i2s_channel_fmt_t channel_format; i2s_comm_format_t communication_format;
  int intr_alloc_flags; int dma_buf_count; int dma_buf_len; bool use_apll;
  bool tx_desc_auto_clear; int fixed_mclk; int mclk_multiple; int bits_per_chan;
} i2s_config_t;
typedef struct { int mck_io_num, bck_io_num, ws_io_num, data_out_num, data_in_num; } i2s_pin_config_t;

static inline esp_err_t i2s_driver_install(i2s_port_t, const i2s_config_t*, int, void*) { return ESP_OK; }
static inline esp_err_t i2s_driver_uninstall(i2s_port_t) { return ESP_OK; }
static inline esp_err_t i2s_set_pin(i2s_port_t, const i2s_pin_config_t*) { return ESP_OK; }
static inline esp_err_t i2s_zero_dma_buffer(i2s_port_t) { return ESP_OK; }
static inline esp_err_t i2s_set_sample_rates(i2s_port_t, uint32_t) { return ESP_OK; }
static inline esp_err_t i2s_write(i2s_port_t, const void*, size_t n, size_t* written, uint32_t) {
  if (written) *written = n; return ESP_OK;
}
