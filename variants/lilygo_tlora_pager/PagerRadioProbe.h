// SPDX-License-Identifier: MIT
// Adapted from MeshCore's LR1121ChipProbe and PortableLoRaChipProbe.
// Copyright (c) 2025 Scott Powell / rippleradios.com
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum class PagerRadioFamily : uint8_t { None, SX1262, LR1121 };

// The IO object supplies bounded identification reads and a hardware reset.
// Derived from MeshCore's LR1121ChipProbe: neither probe starts the RF path.
template <typename IO>
PagerRadioFamily probePagerRadio(IO& io, uint32_t timeout_ms = 250) {
  auto waitReady = [&]() {
    const uint32_t start = io.now();
    while (!io.ready()) {
      if (uint32_t(io.now() - start) >= timeout_ms) return false;
      io.pause(1);
    }
    return true;
  };

  io.reset();
  if (!waitReady()) return PagerRadioFamily::None;
  char first[6] = {}, second[6] = {};
  // RadioLib expects this same signature for the SX1262.
  if (io.readSX126Version(first, sizeof(first)) &&
      memcmp(first, "SX1261", sizeof(first)) == 0 && io.ready() &&
      io.readSX126Version(second, sizeof(second)) &&
      memcmp(first, second, sizeof(first)) == 0) return PagerRadioFamily::SX1262;

  // The SX command is invalid on an LR11xx. Clear its error before GetVersion.
  io.reset();
  if (!waitReady()) return PagerRadioFamily::None;
  uint8_t version[4] = {}, confirm[4] = {};
  if (io.readLR1121Version(version, sizeof(version)) && version[1] == 0x03 &&
      io.ready() && io.readLR1121Version(confirm, sizeof(confirm)) &&
      memcmp(version, confirm, sizeof(version)) == 0) return PagerRadioFamily::LR1121;
  return PagerRadioFamily::None;
}
