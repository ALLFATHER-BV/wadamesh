// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the SenseCAP Indicator TCA9535 driver (IndicatorExpander.h): the
// shadow registers, the write order, the rollback on a failed write and the
// radio-strap vote.
//
//   c++ -std=c++17 -Wall -Wextra -I src -I variants/sensecap_indicator test/test_indicator_expander.cpp -o /tmp/ind_exp && /tmp/ind_exp

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "IndicatorExpander.h"

using namespace indicator;

// Eight registers of a TCA9535 and a log of every write that reached it.
struct FakeBus : ExpanderBus {
  struct Write { uint8_t reg; uint8_t bytes[2]; size_t n; };

  uint8_t regs[8] = {};
  bool reads[8] = {};
  Write log[32] = {};
  size_t writes = 0;
  int fail_write_at = 0;   // 0: never fail; N: the Nth write from now fails

  bool read(uint8_t reg, uint8_t* data, size_t n) override {
    for (size_t i = 0; i < n; ++i) {
      data[i] = regs[reg + i];
      reads[reg + i] = true;
    }
    return true;
  }

  bool write(uint8_t reg, const uint8_t* data, size_t n) override {
    if (fail_write_at > 0 && --fail_write_at == 0) return false;
    Write& w = log[writes++];
    w.reg = reg;
    w.n = n;
    for (size_t i = 0; i < n; ++i) { w.bytes[i] = data[i]; regs[reg + i] = data[i]; }
    return true;
  }
};

static void seedChip(FakeBus& bus, uint8_t out0, uint8_t out1, uint8_t cfg0, uint8_t cfg1) {
  bus.regs[0x02] = out0; bus.regs[0x03] = out1;
  bus.regs[0x06] = cfg0; bus.regs[0x07] = cfg1;
}

static void seeds_shadow_from_chip_not_defaults() {
  FakeBus bus;
  seedChip(bus, 0x5A, 0x0F, 0x4C, 0xFF);
  Tca9535 chip(bus);
  assert(chip.begin());
  assert(chip.output(0) == 0x5A);
  assert(chip.output(1) == 0x0F);
  assert(chip.config(0) == 0x4C);
  assert(chip.config(1) == 0xFF);
  assert(bus.reads[0x00]);   // /INT released
  assert(bus.writes == 0);
}

static void latch_before_direction() {
  FakeBus bus;
  seedChip(bus, 0x00, 0x00, 0xFF, 0xFF);
  Tca9535 chip(bus);
  assert(chip.begin());
  assert(chip.setOutput(pins::kLcdCs, true));
  assert(bus.writes == 2);
  assert(bus.log[0].reg == 0x02);
  assert(bus.log[1].reg == 0x06);
  assert(chip.output(0) & 0x10);
  assert(!(chip.config(0) & 0x10));
}

static void output_pin_writes_level_only() {
  FakeBus bus;
  seedChip(bus, 0x00, 0x00, 0xFF, 0xFF);
  Tca9535 chip(bus);
  assert(chip.begin());
  assert(chip.setOutput(pins::kLoraNss, true));
  assert(bus.writes == 2);
  // Already an output: the level changes, the config register is left alone.
  assert(chip.setOutput(pins::kLoraNss, false));
  assert(chip.setOutput(pins::kLoraNss, true));
  assert(bus.writes == 4);
  assert(bus.log[2].reg == 0x02 && bus.log[3].reg == 0x02);
  assert(bus.regs[0x02] & 0x01);
  assert(!(bus.regs[0x06] & 0x01));
}

static void port1_untouched_except_written_pin() {
  FakeBus bus;
  seedChip(bus, 0x00, 0xA4, 0xFF, 0xF7);
  Tca9535 chip(bus);
  assert(chip.begin());
  assert(chip.setOutput(pins::kRp2040Reset, true));
  assert(chip.output(1) == 0xA5);
  assert(chip.config(1) == 0xF6);
  assert(bus.regs[0x03] == 0xA5);
  assert(bus.regs[0x07] == 0xF6);
  assert((bus.regs[0x03] & 0xFE) == (0xA4 & 0xFE));
  assert((bus.regs[0x07] & 0xFE) == (0xF7 & 0xFE));
}

static void failed_write_rolls_back_shadow() {
  FakeBus bus;
  seedChip(bus, 0x01, 0x00, 0xFF, 0xFF);
  Tca9535 chip(bus);
  assert(chip.begin());
  const uint8_t out0 = chip.output(0), out1 = chip.output(1);
  const uint8_t cfg0 = chip.config(0), cfg1 = chip.config(1);

  bus.fail_write_at = 1;
  assert(!chip.setOutput(pins::kLoraNss, false));
  assert(chip.output(0) == out0 && chip.output(1) == out1);
  assert(chip.config(0) == cfg0 && chip.config(1) == cfg1);

  // The next write starts from the restored shadow, not from the half-applied one.
  assert(chip.setOutput(pins::kLcdCs, true));
  assert(bus.writes == 2);
  assert(bus.log[0].reg == 0x02 && bus.log[0].bytes[0] == (out0 | 0x10));
  assert(bus.log[1].reg == 0x06 && bus.log[1].bytes[0] == (cfg0 & (uint8_t)~0x10));
}

static void failed_config_write_rolls_back_both_shadows() {
  FakeBus bus;
  seedChip(bus, 0x01, 0x00, 0xFF, 0xFF);
  Tca9535 chip(bus);
  assert(chip.begin());
  const uint8_t out0 = chip.output(0), out1 = chip.output(1);
  const uint8_t cfg0 = chip.config(0), cfg1 = chip.config(1);

  // The output write (0x02) lands, the config write (0x06) fails.
  bus.fail_write_at = 2;
  assert(!chip.setOutput(pins::kLcdReset, true));
  assert(bus.writes == 1 && bus.log[0].reg == 0x02);
  assert(chip.output(0) == out0 && chip.output(1) == out1);
  assert(chip.config(0) == cfg0 && chip.config(1) == cfg1);

  // The next write starts from the restored shadow: LCD RESET is not carried
  // over from the half-applied call, and the chip's output latch is rewritten.
  assert(chip.setOutput(pins::kLcdCs, true));
  assert(bus.writes == 3);
  assert(bus.log[1].reg == 0x02 && bus.log[1].bytes[0] == (out0 | 0x10));
  assert(bus.log[1].bytes[1] == out1);
  assert(bus.log[2].reg == 0x06 && bus.log[2].bytes[0] == (cfg0 & (uint8_t)~0x10));
  assert(bus.log[2].bytes[1] == cfg1);
  assert(bus.regs[0x02] == (out0 | 0x10));
}

static uint8_t s_seen = 0;
static int s_calls = 0;
static void* s_ctx = nullptr;

static void observer(uint8_t value, void* ctx) {
  s_seen = value;
  s_ctx = ctx;
  ++s_calls;
}

static void port0_observer_sees_every_read() {
  FakeBus bus;
  seedChip(bus, 0x00, 0x00, 0xFF, 0xFF);
  Tca9535 chip(bus);
  assert(chip.begin());
  int token = 0;
  chip.setPort0Observer(observer, &token);

  bus.regs[0x00] = 0x0C;   // BUSY and DIO1 high
  bool high = false;
  assert(chip.readPin(pins::kLoraBusy, high));
  assert(high);
  assert(s_calls == 1 && s_seen == 0x0C && s_ctx == &token);

  bus.regs[0x00] = 0x08;
  uint8_t value = 0;
  assert(chip.readPort0(value));
  assert(value == 0x08);
  assert(s_calls == 2 && s_seen == 0x08);

  // A port-1 pin read covers both input bytes, so port 0 is reported too.
  bus.regs[0x00] = 0x48;
  bus.regs[0x01] = 0x08;
  assert(chip.readPin(pins::kRadioStrap, high));
  assert(high);
  assert(s_calls == 3 && s_seen == 0x48);
}

static void strap_majority_vote() {
  const bool a[5] = {true, true, false, false, true};
  const bool b[5] = {true, false, false, false, true};
  const bool c[5] = {true, true, true, false, false};
  assert(strapMajority(a));
  assert(!strapMajority(b));
  assert(strapMajority(c));
}

static void clears_polarity_inversion_only_when_set() {
  FakeBus clean;
  seedChip(clean, 0xFF, 0xFF, 0x4C, 0xFE);
  Tca9535 a(clean);
  assert(a.begin());
  assert(clean.writes == 0);   // already 0: nothing written

  FakeBus inverted;
  seedChip(inverted, 0xFF, 0xFF, 0x4C, 0xFE);
  inverted.regs[0x04] = 0x0C; inverted.regs[0x05] = 0x08;
  Tca9535 b(inverted);
  assert(b.begin());
  assert(inverted.writes == 1);
  assert(inverted.log[0].reg == 0x04 && inverted.log[0].n == 2);
  assert(inverted.regs[0x04] == 0x00 && inverted.regs[0x05] == 0x00);
  assert(b.output(0) == 0xFF && b.config(0) == 0x4C);   // shadows still from the chip
}

int main() {
  seeds_shadow_from_chip_not_defaults();
  clears_polarity_inversion_only_when_set();
  latch_before_direction();
  output_pin_writes_level_only();
  port1_untouched_except_written_pin();
  failed_write_rolls_back_shadow();
  failed_config_write_rolls_back_both_shadows();
  port0_observer_sees_every_read();
  strap_majority_vote();
  printf("all indicator expander tests passed\n");
  return 0;
}
