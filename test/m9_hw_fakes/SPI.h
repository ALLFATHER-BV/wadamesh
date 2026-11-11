// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class SPIClass {
public:
  int sclk = -1, miso = -1, mosi = -1, ss = -1;
  bool begun = false;
  void begin(int sck = -1, int mi = -1, int mo = -1, int s = -1) {
    sclk = sck;
    miso = mi;
    mosi = mo;
    ss = s;
    begun = true;
  }
};

extern SPIClass SPI;
