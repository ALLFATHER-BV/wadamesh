// SPDX-License-Identifier: GPL-3.0-or-later
#if defined(HAS_WIO_TRACKER_L2)
#include "WioTrackerL2Audio.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <math.h>

#include "../lilygo_tlora_pager/Es8311Codec.h"   // shared with the pager and the T-Deck Max
#include "WioTrackerL2Io.h"

// Seeed's pin map (Meshtastic variant.h: DAC_I2S_MCLK/BCK/WS/DOUT); platformio.ini wins.
#ifndef PIN_I2S_MCLK
  #define PIN_I2S_MCLK 10
#endif
#ifndef PIN_I2S_BCK
  #define PIN_I2S_BCK 11
#endif
#ifndef PIN_I2S_WS
  #define PIN_I2S_WS 12
#endif
#ifndef PIN_I2S_DOUT
  #define PIN_I2S_DOUT 16
#endif

namespace {

constexpr i2s_port_t kPort = I2S_NUM_0;
constexpr int kRate = 16000;            // in Es8311Codec's rate table; MCLK = 256 x fs
constexpr uint8_t kCodecAddr = 0x18;
constexpr uint32_t kAmpSettleMs = 250;  // Seeed's AUDIO_AMP_SETTLE_MS for this amp
constexpr uint8_t kMaxNotes = 4;

enum class State : uint8_t { Idle, Playing, Done };

Es8311Codec s_codec;
volatile State s_state = State::Idle;
bool s_i2s_installed = false;
uint32_t s_amp_on_ms = 0;
uint16_t s_freq[kMaxNotes];
uint16_t s_dur[kMaxNotes];
uint8_t s_notes = 0;

// Installed per chime and freed after, like the T-Deck and Max paths: holding the
// driver resident would keep its DMA buffers in scarce internal RAM for good.
bool i2sInstall() {
  // i2s_driver_install aborts the firmware on NO_MEM, so skip the chime instead.
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 16 * 1024) {
    Serial.println("[wio-l2] audio: no internal heap");
    return false;
  }
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = kRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;   // mono tone buffer
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;                     // underrun sends silence
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;         // the codec's dividers assume 256 x fs
  esp_err_t e = i2s_driver_install(kPort, &cfg, 0, nullptr);
  if (e != ESP_OK) {
    Serial.printf("[wio-l2] audio: i2s_driver_install failed %d\n", (int)e);
    return false;
  }
  i2s_pin_config_t pins = {};
  pins.mck_io_num = PIN_I2S_MCLK;
  pins.bck_io_num = PIN_I2S_BCK;
  pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = PIN_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  e = i2s_set_pin(kPort, &pins);
  if (e != ESP_OK) {
    Serial.printf("[wio-l2] audio: i2s_set_pin failed %d\n", (int)e);
    i2s_driver_uninstall(kPort);
    return false;
  }
  s_i2s_installed = true;
  return true;
}

void teardown() {
  s_codec.setMute(true);
  s_codec.suspend();
  if (s_i2s_installed) {
    i2s_zero_dma_buffer(kPort);
    i2s_driver_uninstall(kPort);
    s_i2s_installed = false;
  }
  (void)WioTrackerL2Io::setAudioPaPower(false);
}

// 16-bit sine with a short fade in and out so it doesn't click. Loudness is the
// codec's volume register, not sample scaling.
void playTone(int freq, int ms) {
  const int total = (kRate * ms) / 1000;
  const int fade = total / 8 > 0 ? total / 8 : 1;
  const float step = 2.0f * (float)M_PI * (float)freq / (float)kRate;
  int16_t buf[128];
  float phase = 0.0f;
  int done = 0;
  while (done < total) {
    int n = 0;
    for (; n < 128 && done < total; ++n, ++done) {
      float amp = 22000.0f;
      if (done < fade) amp *= (float)done / fade;
      else if (done > total - fade) amp *= (float)(total - done) / fade;
      buf[n] = (int16_t)(sinf(phase) * amp);
      phase += step;
      if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
    }
    size_t written = 0;
    i2s_write(kPort, buf, (size_t)n * sizeof(int16_t), &written, pdMS_TO_TICKS(200));
  }
}

// I2S only: no I2C from here (see the header).
void chimeTask(void*) {
  const uint32_t since = millis() - s_amp_on_ms;
  if (since < kAmpSettleMs) vTaskDelay(pdMS_TO_TICKS(kAmpSettleMs - since));
  for (uint8_t i = 0; i < s_notes; ++i) playTone(s_freq[i], s_dur[i]);
  i2s_zero_dma_buffer(kPort);
  s_state = State::Done;
  vTaskDelete(nullptr);
}

}  // namespace

bool wioL2AudioPlay(const uint16_t* freqs, const uint16_t* durationsMs, uint8_t count, uint8_t volPct) {
  if (s_state != State::Idle || !freqs || !durationsMs || count == 0 || volPct == 0) return false;
  if (count > kMaxNotes) count = kMaxNotes;
  for (uint8_t i = 0; i < count; ++i) { s_freq[i] = freqs[i]; s_dur[i] = durationsMs[i]; }
  s_notes = count;

  // Amp first: if the expander isn't answering there's nothing to drive.
  if (!WioTrackerL2Io::setAudioPaPower(true)) return false;
  s_amp_on_ms = millis();
  // The codec's PLL locks to MCLK, so I2S has to be clocking before start(). begin()
  // runs every time rather than once: if the codec's supply follows PA_PWR_EN, its
  // registers are gone after each chime.
  if (!i2sInstall()) { teardown(); return false; }
  if (!s_codec.begin(Wire, kCodecAddr)) {
    Serial.println("[wio-l2] audio: ES8311 not answering at 0x18");
    teardown();
    return false;
  }
  if (!s_codec.start((uint32_t)kRate)) {
    Serial.println("[wio-l2] audio: ES8311 start failed");
    teardown();
    return false;
  }
  s_codec.setVolumePercent(volPct);
  s_codec.setMute(false);

  s_state = State::Playing;
  if (xTaskCreate(chimeTask, "l2chime", 4096, nullptr, 3, nullptr) != pdPASS) {
    s_state = State::Idle;
    teardown();
    return false;
  }
  return true;
}

void wioL2AudioService() {
  if (s_state != State::Done) return;
  teardown();
  s_state = State::Idle;
}

bool wioL2AudioBusy() { return s_state != State::Idle; }

#endif  // HAS_WIO_TRACKER_L2
