// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Michael A. Cojocari and contributors
#pragma once

#include <FS.h>
#include <sd_defines.h>

// SD-compatible filesystem facade for Camillia-MT's software-SPI card backend.
// Neither the display's SPI3 host nor the radio's SPI2 host is reconfigured.
class CrowPanel35SD : public fs::FS {
public:
  CrowPanel35SD();
  bool begin();
  void end();
  sdcard_type_t cardType() const;
  uint64_t cardSize() const;
  uint64_t totalBytes() const;
  uint64_t usedBytes() const;
  uint8_t driveNumber() const;
  bool probeAlive() const;
};

extern CrowPanel35SD SD;
