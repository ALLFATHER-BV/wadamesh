// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the T-Display P4 vibration motor driver (#730,
// variants/tdisplay_p4/P4Haptic.cpp): the production file against a register-level model
// of the AW86224 on a fake Wire1 (test/p4_haptic_fakes/Wire.h), with virtual time. Each
// scenario runs in a child process, as the driver's state is file-static and lazy.
//
//   c++ -std=c++17 -Wall -Wextra -I test/p4_haptic_fakes test/test_p4_haptic.cpp \
//       -o /tmp/test_p4_haptic && /tmp/test_p4_haptic
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Arduino.h"
#include "Wire.h"

uint32_t g_ms = 1000;
bool g_bus_held = false;
bool g_bus_busy = false;   // another user holds I2C_2 (the codec) past the wait
int g_acquires = 0;
Aw86224 g_aw;
TwoWire Wire1;

#define HAS_TDISPLAY_P4 1
#include "../variants/tdisplay_p4/P4I2c1.h"
TwoWire* p4I2c1Acquire(P4I2c1Route route, uint32_t) {
  assert(route == P4_I2C1_AUDIO);   // the motor shares the codec's pins
  assert(!g_bus_held);
  ++g_acquires;
  if (g_bus_busy) return nullptr;
  g_bus_held = true;
  return &Wire1;
}
void p4I2c1Release() { assert(g_bus_held); g_bus_held = false; }

#include "../variants/tdisplay_p4/P4Haptic.cpp"

static const uint16_t BASE = 0x0800;

static bool sramHoldsTable() {
  return memcmp(g_aw.sram + BASE, kP4HapticWaveform, sizeof kP4HapticWaveform) == 0;
}
static int lastWrite(uint8_t reg) {
  for (size_t i = g_aw.log.size(); i-- > 0;) if (g_aw.log[i].first == reg) return g_aw.log[i].second;
  return -1;
}

static void scenarioFreshChip() {
  // First use on a chip with an empty SRAM: probe, configure, load the waveform, verify.
  assert(p4HapticReady());
  assert(sramHoldsTable());
  assert(g_aw.sram_writes == (int)sizeof kP4HapticWaveform);
  assert((g_aw.reg[0x44] & 0x03) == 0x02);           // 12 kHz
  assert((g_aw.reg[0x2D] & 0x0F) == 0x08 && g_aw.reg[0x2E] == 0x00);   // RAM base 0x0800
  const uint16_t ae = BASE >> 1, af = BASE - (BASE >> 2);
  assert(g_aw.reg[0x2F] == (uint8_t)(((ae >> 8) << 4) | ((af >> 8) & 0x0F)));
  assert(g_aw.reg[0x30] == (uint8_t)(ae & 0xFF) && g_aw.reg[0x31] == (uint8_t)(af & 0xFF));
  assert((g_aw.reg[0x43] & 0x08) == 0);              // RAM init (SRAM access) left off
  assert(!g_bus_held);
  // Idempotent: a second call touches nothing.
  const int t = g_aw.transactions;
  assert(p4HapticReady() && g_aw.transactions == t);

  // One buzz: 120 ms at the default gain, slot 1 = waveform 1 looped, auto brake, then stopped.
  g_aw.log.clear();
  const uint32_t t0 = g_ms;
  p4HapticBuzz(120);
  assert(g_aw.played_ms.size() == 1 && g_aw.played_ms[0] == 120);
  assert(g_aw.played_gain[0] == P4_HAPTIC_GAIN_DEFAULT);
  assert(lastWrite(0x0A) == 1 && lastWrite(0x0B) == 0);          // WAVCFG1 = wave 1, WAVCFG2 = end
  assert((lastWrite(0x12) & 0xF0) == 0xF0);                       // loop forever
  assert((lastWrite(0x08) & 0x07) == 0x04);                       // RAM mode + auto brake
  assert((lastWrite(0x49) & 0x40) == 0x40);
  assert(!g_aw.playing && (g_aw.reg[0x44] & 0x40) == 0);          // stopped, standby force released
  assert(g_ms - t0 >= 120 && g_ms - t0 < 120 + 20);
  assert(!g_bus_held);

  // Gain passes through; a second buzz is a second play.
  p4HapticBuzz(60, 0x50);
  assert(g_aw.played_ms.size() == 2 && g_aw.played_ms[1] == 60 && g_aw.played_gain[1] == 0x50);
  // Nothing to do for 0 or negative durations: no bus traffic at all.
  const int tr = g_aw.transactions;
  p4HapticBuzz(0); p4HapticBuzz(-5);
  assert(g_aw.transactions == tr && g_aw.played_ms.size() == 2);
}

static void scenarioRetainedSram() {
  // The chip kept its SRAM across a P4 reset: verified, not rewritten.
  memcpy(g_aw.sram + BASE, kP4HapticWaveform, sizeof kP4HapticWaveform);
  assert(p4HapticReady());
  assert(g_aw.sram_writes == 0);
}

static void scenarioOneByteDiffers() {
  memcpy(g_aw.sram + BASE, kP4HapticWaveform, sizeof kP4HapticWaveform);
  g_aw.sram[BASE + 300] ^= 0xFF;
  assert(p4HapticReady());
  assert(sramHoldsTable() && g_aw.sram_writes == (int)sizeof kP4HapticWaveform);
}

static void scenarioAbsent() {
  // No chip at 0x58: not ready, and after the first try the driver stays off the bus.
  g_aw.present = false;
  assert(!p4HapticReady());
  const int a = g_acquires;
  p4HapticBuzz(120);
  assert(!p4HapticReady());
  assert(g_acquires == a && g_aw.played_ms.empty());
}

static void scenarioBusBusy() {
  // The codec holds the bus past the 200 ms wait: no start, so nothing to stop.
  assert(p4HapticReady());
  g_bus_busy = true;
  const uint32_t t0 = g_ms;
  p4HapticBuzz(120);
  assert(g_aw.played_ms.empty() && !g_aw.playing);
  assert(g_ms == t0);   // it did not sit out the duration either
}

static void scenarioStopNeverIdles() {
  // A chip that never reports standby: three stop attempts, the bus released, no hang.
  assert(p4HapticReady());
  g_aw.stop_ignored = true;
  p4HapticBuzz(50);
  assert(!g_bus_held);
}

static int run(const char* name, void (*fn)()) {
  fflush(stdout);
  const pid_t pid = fork();
  if (pid == 0) { fn(); _exit(0); }
  int st = 0;
  waitpid(pid, &st, 0);
  const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  printf("  %-24s %s\n", name, ok ? "ok" : "FAILED");
  return ok ? 0 : 1;
}

int main() {
  // The waveform table's own header: version, then 4 {start, end} pairs from 0x0811,
  // ending at the table's last byte.
  assert(kP4HapticWaveform[0] == 0x55);
  assert(((kP4HapticWaveform[1] << 8) | kP4HapticWaveform[2]) == BASE + 1 + 4 * 4);
  assert(((kP4HapticWaveform[15] << 8) | kP4HapticWaveform[16]) == BASE + sizeof kP4HapticWaveform - 1);

  int fails = 0;
  fails += run("fresh chip + buzz", scenarioFreshChip);
  fails += run("retained SRAM", scenarioRetainedSram);
  fails += run("one SRAM byte differs", scenarioOneByteDiffers);
  fails += run("chip absent", scenarioAbsent);
  fails += run("bus busy", scenarioBusBusy);
  fails += run("stop never idles", scenarioStopNeverIdles);
  if (fails) { printf("test_p4_haptic: %d FAILED\n", fails); return 1; }
  printf("test_p4_haptic: ok\n");
  return 0;
}
