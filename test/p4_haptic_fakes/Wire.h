// SPDX-License-Identifier: GPL-3.0-or-later
// An AW86224 on I2C 0x58, modelled at the register level for the host test: register
// auto-increment, the SRAM pointer and data port, play (go / stop / forced standby) and
// the global state the driver polls. Every transaction must happen while the test's
// p4I2c1Acquire() holds the bus.
#pragma once
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <vector>

extern uint32_t g_ms;
extern bool g_bus_held;

struct Aw86224 {
  bool present = true;
  uint8_t reg[256] = {};
  uint8_t sram[4096] = {};
  uint16_t ptr = 0;
  bool playing = false;
  bool stop_ignored = false;          // a chip that never reaches standby (stop retries)
  uint32_t play_start = 0;
  std::vector<uint32_t> played_ms;    // how long each play ran
  std::vector<uint8_t> played_gain;
  int sram_writes = 0, transactions = 0;
  std::vector<std::pair<uint8_t, uint8_t>> log;   // register writes in order
  void writeReg(uint8_t r, uint8_t v) {
    log.push_back({r, v});
    if (r == 0x42) { sram[ptr++ & 0x0FFF] = v; ++sram_writes; return; }   // RAMDATA
    reg[r] = v;
    if (r == 0x40 || r == 0x41) ptr = (uint16_t)((reg[0x40] << 8) | reg[0x41]);
    if (r == 0x09) {                                   // PLAYCFG4
      if ((v & 0x01) && !playing) { playing = true; play_start = g_ms; played_gain.push_back(reg[0x07]); }
      reg[0x09] &= ~0x01;                              // go self-clears
    }
    if (r == 0x44 && (v & 0x40) && (reg[0x09] & 0x02) && playing && !stop_ignored) {   // forced standby after stop
      playing = false; played_ms.push_back(g_ms - play_start); reg[0x09] &= ~0x02;
    }
  }
  uint8_t readReg(uint8_t r) {
    if (r == 0x42) return sram[ptr++ & 0x0FFF];
    if (r == 0x3F) return playing ? 0x08 : 0x00;        // GLBRD5 state
    if (r == 0x00) return 0x24;                         // ID
    return reg[r];
  }
};
extern Aw86224 g_aw;

class TwoWire {
  uint8_t addr_ = 0;
  std::vector<uint8_t> tx_;
  uint8_t rd_reg_ = 0;
  std::vector<uint8_t> rx_;
  size_t rx_pos_ = 0;
public:
  void beginTransmission(uint8_t a) { assert(g_bus_held); addr_ = a; tx_.clear(); }
  size_t write(uint8_t b) { tx_.push_back(b); return 1; }
  size_t write(const uint8_t* d, size_t n) { tx_.insert(tx_.end(), d, d + n); return n; }
  uint8_t endTransmission(bool = true) {
    assert(g_bus_held);
    ++g_aw.transactions;
    if (addr_ != 0x58 || !g_aw.present || tx_.empty()) return 2;
    uint8_t r = tx_[0];
    rd_reg_ = r;
    for (size_t i = 1; i < tx_.size(); ++i) {
      g_aw.writeReg(r, tx_[i]);
      if (r != 0x42 && r != 0x40) ++r;   // auto-increment (the data port stays put)
      else if (r == 0x40) r = 0x41;
    }
    return 0;
  }
  uint8_t requestFrom(uint8_t a, uint8_t n) {
    assert(g_bus_held);
    rx_.clear(); rx_pos_ = 0;
    if (a != 0x58 || !g_aw.present) return 0;
    uint8_t r = rd_reg_;
    for (uint8_t i = 0; i < n; ++i) { rx_.push_back(g_aw.readReg(r)); if (r != 0x42) ++r; }
    return n;
  }
  int read() { return rx_pos_ < rx_.size() ? rx_[rx_pos_++] : -1; }
};
extern TwoWire Wire1;
