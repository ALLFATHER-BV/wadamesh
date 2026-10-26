// SPDX-License-Identifier: GPL-3.0-only
#if defined(HAS_TDISPLAY_P4)
// AW86224 register sequence adapted from LilyGo's cpp_bus_driver Aw862xx driver (GPL-3.0,
// github.com/Llgok/cpp_bus_driver, src/chip/i2c/aw862xx.cpp) -- trimmed to RAM-mode playback
// of one waveform and re-based onto Arduino Wire1 like P4Audio.cpp. Register names follow
// that driver (and the AW86224 datasheet map).
#include "P4Haptic.h"
#include "P4HapticWaveform.h"
#include "P4I2c1.h"
#include <Arduino.h>
#include <Wire.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// lilygo_device_driver t_display_p4/v1/config.h: AW86224 on IIC_2 (pins 20/21, in P4I2c1.cpp).
#define P4H_ADDR       0x58

#define AW_ID          0x00   // chip id (read) / soft reset (write 0xAA)
#define AW_PLAYCFG2    0x07   // RAM/RTP playback gain
#define AW_PLAYCFG3    0x08   // [1:0] play mode (0 = RAM), [2] auto brake
#define AW_PLAYCFG4    0x09   // [0] go, [1] stop
#define AW_WAVCFG1     0x0A   // sequencer slot 1: waveform number
#define AW_WAVCFG2     0x0B   // sequencer slot 2 (0 = end of sequence)
#define AW_WAVCFG9     0x12   // [7:4] slot 1 loop count (15 = forever)
#define AW_RTPCFG1     0x2D   // [3:0] RAM base address high
#define AW_RTPCFG2     0x2E   // RAM base address low
#define AW_RTPCFG3     0x2F   // FIFO almost-empty/full thresholds (3 bytes)
#define AW_GLBRD5      0x3F   // [3:0] global state (0 = standby)
#define AW_RAMADDRH    0x40   // SRAM pointer (2 bytes)
#define AW_RAMDATA     0x42   // SRAM data port
#define AW_SYSCTRL1    0x43   // [3] RAM init (SRAM access clock)
#define AW_SYSCTRL2    0x44   // [1:0] sample rate (2 = 12 kHz), [6] force standby
#define AW_SYSCTRL7    0x49   // [6] playing-changed gain bypass

#define P4H_BASE       0x0800 // from the waveform header (first wave 0x0811 - 4 waves*4 - 1)
#define P4H_WAVE       1      // ~18 ms buzz, looped for the requested duration
#define P4H_CHUNK      32     // per-transaction SRAM bytes (well under Wire's 128 B buffer)

// ---- minimal AW86224 register layer --------------------------------------------------------
static bool awWrite(uint8_t reg, uint8_t val) {
  Wire1.beginTransmission(P4H_ADDR);
  Wire1.write(reg);
  Wire1.write(val);
  return Wire1.endTransmission() == 0;
}
static bool awWriteN(uint8_t reg, const uint8_t* data, size_t len) {
  Wire1.beginTransmission(P4H_ADDR);
  Wire1.write(reg);
  Wire1.write(data, len);
  return Wire1.endTransmission() == 0;
}
static bool awReadN(uint8_t reg, uint8_t* data, size_t len) {
  Wire1.beginTransmission(P4H_ADDR);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((uint8_t)P4H_ADDR, (uint8_t)len) != len) return false;
  for (size_t i = 0; i < len; i++) data[i] = Wire1.read();
  return true;
}
static bool awRead(uint8_t reg, uint8_t* val) { return awReadN(reg, val, 1); }
// Read-modify-write: clear `mask`, set `bits`.
static bool awUpdate(uint8_t reg, uint8_t mask, uint8_t bits) {
  uint8_t v;
  return awRead(reg, &v) && awWrite(reg, (uint8_t)((v & ~mask) | bits));
}
static bool awSetRamAddr(uint16_t addr) {
  const uint8_t a[2] = {(uint8_t)(addr >> 8), (uint8_t)(addr & 0xFF)};
  return awWriteN(AW_RAMADDRH, a, sizeof(a));
}

// Stop flag + forced standby until the state machine reports standby (<= ~100 ms).
static bool awStop() {
  if (!awUpdate(AW_PLAYCFG4, 0x02, 0x02)) return false;
  if (!awUpdate(AW_SYSCTRL2, 0x40, 0x40)) return false;
  bool idle = false;
  for (int i = 0; i < 100 && !idle; i++) {
    uint8_t st;
    idle = awRead(AW_GLBRD5, &st) && (st & 0x0F) == 0;
    if (!idle) vTaskDelay(pdMS_TO_TICKS(1));
  }
  return awUpdate(AW_SYSCTRL2, 0x40, 0x00) && idle;
}

// The chip keeps its SRAM across a P4 reset while it stays powered, so compare first and only
// write what differs -- the same verify-then-reload LilyGo does. Pointer set per chunk, since
// it is not guaranteed to survive between transactions.
static bool awSramMatches() {
  uint8_t buf[P4H_CHUNK];
  for (size_t off = 0; off < sizeof(kP4HapticWaveform); off += P4H_CHUNK) {
    const size_t n = min((size_t)P4H_CHUNK, sizeof(kP4HapticWaveform) - off);
    if (!awSetRamAddr(P4H_BASE + off) || !awReadN(AW_RAMDATA, buf, n)) return false;
    if (memcmp(buf, kP4HapticWaveform + off, n) != 0) return false;
  }
  return true;
}
static bool awSramLoad() {
  for (size_t off = 0; off < sizeof(kP4HapticWaveform); off += P4H_CHUNK) {
    const size_t n = min((size_t)P4H_CHUNK, sizeof(kP4HapticWaveform) - off);
    if (!awSetRamAddr(P4H_BASE + off) || !awWriteN(AW_RAMDATA, kP4HapticWaveform + off, n)) return false;
  }
  return true;
}

// Sample rate, then (with SRAM access on) base address + FIFO thresholds + waveform data.
static bool awInit(bool* reloaded) {
  *reloaded = false;
  if (!awUpdate(AW_SYSCTRL2, 0x03, 0x02)) return false;          // 12 kHz waveform data
  if (!awStop()) return false;
  if (!awUpdate(AW_SYSCTRL1, 0x08, 0x08)) return false;          // RAM init on
  delayMicroseconds(500);
  const uint16_t ae = P4H_BASE >> 1, af = P4H_BASE - (P4H_BASE >> 2);   // vendor formula
  const uint8_t fifo[3] = {(uint8_t)(((ae >> 8) << 4) | ((af >> 8) & 0x0F)),
                           (uint8_t)(ae & 0xFF), (uint8_t)(af & 0xFF)};
  bool ok = awUpdate(AW_RTPCFG1, 0x0F, (uint8_t)((P4H_BASE >> 8) & 0x0F)) &&
            awWrite(AW_RTPCFG2, (uint8_t)(P4H_BASE & 0xFF)) &&
            awWriteN(AW_RTPCFG3, fifo, sizeof(fifo));
  if (ok && !awSramMatches()) {
    *reloaded = true;
    ok = awSramLoad() && awSramMatches();
  }
  const bool ram_off = awUpdate(AW_SYSCTRL1, 0x08, 0x00);        // RAM init off
  return ok && ram_off;
}

static SemaphoreHandle_t s_mtx = nullptr;   // one buzz at a time
static portMUX_TYPE      s_create_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready = false, s_failed = false;

bool p4HapticReady() {
  if (s_ready)  return true;
  if (s_failed) return false;
  if (!s_mtx) {
    SemaphoreHandle_t m = xSemaphoreCreateMutex();
    portENTER_CRITICAL(&s_create_mux);
    if (!s_mtx) { s_mtx = m; m = nullptr; }
    portEXIT_CRITICAL(&s_create_mux);
    if (m) vSemaphoreDelete(m);
    if (!s_mtx) return false;
  }
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_ready || s_failed) { xSemaphoreGive(s_mtx); return s_ready; }

  bool present = false, inited = false, reloaded = false;
  uint8_t id = 0xFF;
  if (p4I2c1Acquire(P4_I2C1_AUDIO, UINT32_MAX)) {
    present = awRead(AW_ID, &id);
    inited = present && awInit(&reloaded);
    p4I2c1Release();
  }
  if (!inited) {
    printf("[P4HAPTIC] AW86224 %s\n", present ? "init failed" : "not found @0x58");
    s_failed = true;
  } else {
    printf("[P4HAPTIC] AW86224 up (id 0x%02X, waveform RAM %s)\n", id, reloaded ? "loaded" : "retained");
    s_ready = true;
  }
  xSemaphoreGive(s_mtx);
  return s_ready;
}

void p4HapticBuzz(int duration_ms, uint8_t gain) {
  if (duration_ms <= 0) return;
  if (!p4HapticReady()) return;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  // Slot 1 = the buzz waveform looped forever, slot 2 = end; RAM mode, auto brake, given gain.
  bool started = false;
  if (p4I2c1Acquire(P4_I2C1_AUDIO, 200)) {
    started = awWrite(AW_WAVCFG1, P4H_WAVE) && awWrite(AW_WAVCFG2, 0) &&
              awUpdate(AW_WAVCFG9, 0xF0, 0xF0) &&
              awUpdate(AW_PLAYCFG3, 0x07, 0x04) &&
              awUpdate(AW_SYSCTRL7, 0x40, 0x40) &&
              awWrite(AW_PLAYCFG2, gain) &&
              awUpdate(AW_PLAYCFG4, 0x01, 0x01);
    p4I2c1Release();
  }
  if (started) {
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    // Must not be skipped -- the loop count is "forever". Wait as long as it takes for the bus.
    if (p4I2c1Acquire(P4_I2C1_AUDIO, UINT32_MAX)) {
      bool stopped = false;
      for (int i = 0; i < 3 && !stopped; i++) stopped = awStop();
      if (!stopped) printf("[P4HAPTIC] stop failed\n");
      p4I2c1Release();
    }
  }
  xSemaphoreGive(s_mtx);
}

#endif
