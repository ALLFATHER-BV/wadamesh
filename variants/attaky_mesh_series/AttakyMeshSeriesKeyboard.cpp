// SPDX-License-Identifier: GPL-3.0-or-later
#include "AttakyMeshSeriesKeyboard.h"

#if defined(HAS_ATTAKY_MESH_KEYBOARD) && defined(ESP32)

#include <Arduino.h>
#include <Wire.h>
#include "AttakySharedI2C.h"

static const uint8_t AW_REG_IN_P0   = 0x00;
static const uint8_t AW_REG_OUT_P1  = 0x03;
static const uint8_t AW_REG_CFG_P0  = 0x04;
static const uint8_t AW_REG_CFG_P1  = 0x05;
static const uint8_t AW_REG_OUT_P0  = 0x02;
static const uint8_t AW_REG_MODE_P0 = 0x12;
static const uint8_t AW_REG_MODE_P1 = 0x13;
static const uint8_t AW_REG_SWRST   = 0x7F;

static const uint8_t KB_ADDR[2] = { 0x5A, 0x5B };

static const uint8_t P1_IDLE = 0xE0;
static const uint8_t ROW_DRIVE[5] = { 0xFE, 0xFD, 0xFB, 0xF7, 0xEF };

#define K_NOP  0x00
#define K_SFT  0x01
#define K_BSP  0x08
#define K_TAB  0x09
#define K_ENT  0x0D
#define K_SPC  0x20

static const uint8_t KEYMAP_L[5][5] = {
  { '1', '2', '3',  '4',  '5' },
  { 'q', 'w', 'e',  'r',  't' },
  { 'a', 's', 'd',  'f',  'g' },
  { K_SFT, 'z', 'x', 'c', 'v' },
  { K_NOP, K_TAB, K_NOP, ',', K_SPC },
};
static const uint8_t KEYMAP_R[5][5] = {
  { '6', '7', '8', '9', '0' },
  { 'y', 'u', 'i', 'o', 'p' },
  { 'h', 'j', 'k', 'l', K_BSP },
  { 'b', 'n', 'm', K_NOP, K_ENT },
  { K_NOP, '.', K_NOP, '#', K_NOP },
};
static const uint8_t SHIFT_HALF = 0, SHIFT_ROW = 3, SHIFT_COL = 0;

static bool     s_inited      = false;
static bool     s_present[2]  = { false, false };
static uint8_t  s_prev[2][5]  = { { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
                                  { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF } };

static volatile uint8_t s_ring[16];
static volatile uint8_t s_head = 0, s_tail = 0;

static void ringPush(uint8_t b) {
  uint8_t nh = (uint8_t)((s_head + 1) & 15);
  if (nh != s_tail) { s_ring[s_head] = b; s_head = nh; }
}
static void ringClear() { s_head = s_tail = 0; }

static bool awWrite(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static uint8_t awRead(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom((int)addr, 1) != 1) return 0xFF;
  return (uint8_t)Wire.read();
}

static bool initHalf(uint8_t addr) {
  bool ok = true;
  ok &= awWrite(addr, AW_REG_SWRST,   0x00);
  delay(2);
  ok &= awWrite(addr, AW_REG_MODE_P0, 0xFF);
  ok &= awWrite(addr, AW_REG_MODE_P1, 0xFF);
  ok &= awWrite(addr, AW_REG_CFG_P0,  0xFF);
  ok &= awWrite(addr, AW_REG_CFG_P1,  0x00);
  ok &= awWrite(addr, AW_REG_OUT_P0,  0x00);
  ok &= awWrite(addr, AW_REG_OUT_P1,  P1_IDLE);
  return ok;
}

static void ensureInit() {
  if (s_inited) return;
  s_inited = true;
  for (int h = 0; h < 2; h++) {
    if (!attakyI2cLock(30)) continue;
    s_present[h] = initHalf(KB_ADDR[h]);
    attakyI2cUnlock();
  }
}

static void scanHalf(uint8_t addr, uint8_t cols[5]) {
  for (uint8_t row = 0; row < 5; row++) {
    awWrite(addr, AW_REG_OUT_P1, (uint8_t)(ROW_DRIVE[row] | 0xE0));
    delayMicroseconds(50);
    cols[row] = (uint8_t)(awRead(addr, AW_REG_IN_P0) & 0x1F);
  }
  awWrite(addr, AW_REG_OUT_P1, P1_IDLE);
}

void attakyKeyboardPoll(bool active) {
  ensureInit();

  if (!active) {
    for (int h = 0; h < 2; h++)
      for (int r = 0; r < 5; r++) s_prev[h][r] = 0xFF;
    ringClear();
    return;
  }

  uint8_t cur[2][5] = { { 0x1F,0x1F,0x1F,0x1F,0x1F }, { 0x1F,0x1F,0x1F,0x1F,0x1F } };
  for (int h = 0; h < 2; h++) {
    if (!s_present[h]) continue;
    if (!attakyI2cLock(20)) continue;
    scanHalf(KB_ADDR[h], cur[h]);
    attakyI2cUnlock();
  }

  const bool shift_down =
      s_present[SHIFT_HALF] && !(cur[SHIFT_HALF][SHIFT_ROW] & (1u << SHIFT_COL));

  for (int h = 0; h < 2; h++) {
    if (!s_present[h]) continue;
    for (int row = 0; row < 5; row++) {
      uint8_t now_bits  = cur[h][row];
      uint8_t prev_bits = s_prev[h][row];
      for (int col = 0; col < 5; col++) {
        const uint8_t mask = (uint8_t)(1u << col);
        const bool down_now  = !(now_bits  & mask);
        const bool down_prev = !(prev_bits & mask);
        if (down_now && !down_prev) {
          uint8_t k = (h == 0) ? KEYMAP_L[row][col] : KEYMAP_R[row][col];
          if (k == K_NOP || k == K_SFT) continue;
          if (shift_down && k >= 'a' && k <= 'z') k = (uint8_t)(k - 0x20);
          ringPush(k);
        }
      }
      s_prev[h][row] = now_bits;
    }
  }
}

int attakyKeyboardReadKey() {
  if (s_tail == s_head) return 0;
  uint8_t b = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) & 15);
  return b;
}

bool attakyKeyboardPresent() { return s_present[0] || s_present[1]; }

#endif
