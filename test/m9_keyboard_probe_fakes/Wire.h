// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in for TwoWire: one simulated keyboard controller per bus, or none.
#pragma once
#include <stdint.h>
#include <string.h>

struct TwoWire {
  // --- simulated controller (set by the test) ---
  uint8_t dev_addr = 0;          // 0 = nothing on this bus
  bool    nack_reads = false;    // controller present but every transfer NACKs
  uint8_t keys[8] = {0};         // keys the controller will report, in order
  uint8_t nkeys = 0, next_key = 0;
  // --- what the driver did to it (read by the test) ---
  uint8_t backlight_addr = 0, backlight_duty = 0;
  int     begun = 0;

  // --- driver-facing API ---
  void begin(int, int) { begun++; }
  void beginTransmission(uint8_t a) { tx_addr = a; tx_len = 0; }
  size_t write(uint8_t b) { if (tx_len < sizeof tx) tx[tx_len] = b; tx_len++; return 1; }
  uint8_t endTransmission() {
    if (!present(tx_addr)) return 2;                    // NACK on address
    if (tx_len >= 1) reg = tx[0];
    if (tx_len >= 2 && reg == 0x02) { backlight_addr = tx_addr; backlight_duty = tx[1]; }
    return 0;
  }
  uint8_t requestFrom(int a, int n) {
    if (!present((uint8_t)a) || n != 1) return 0;
    switch (reg) {
      case 0x00: val = 0x03; break;                     // HW version
      case 0xFE: val = 0x10; break;                     // FW version
      case 0x01: val = next_key < nkeys ? keys[next_key++] : 0; break;   // latched key
      default:   val = 0xFF; break;
    }
    return 1;
  }
  int read() { return val; }

private:
  bool present(uint8_t a) const { return dev_addr != 0 && a == dev_addr && !nack_reads; }
  uint8_t tx_addr = 0, tx[4] = {0}; size_t tx_len = 0; uint8_t reg = 0, val = 0;
};

inline TwoWire Wire;
inline TwoWire Wire1;
