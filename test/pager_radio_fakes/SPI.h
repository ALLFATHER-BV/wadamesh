// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <vector>

#define MSBFIRST 1
#define SPI_MODE0 0
struct SPISettings { SPISettings(uint32_t, int, int) {} };
struct SPIClass {
  std::vector<std::vector<uint8_t>> writes;
  std::vector<uint8_t> responses;
  size_t cursor = 0;
  void beginTransaction(SPISettings) { writes.emplace_back(); }
  void endTransaction() {}
  uint8_t transfer(uint8_t value) {
    writes.back().push_back(value);
    return cursor < responses.size() ? responses[cursor++] : 0;
  }
};
