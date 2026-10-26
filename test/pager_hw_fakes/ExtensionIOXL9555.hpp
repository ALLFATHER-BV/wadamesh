#pragma once
#include <cstdint>
#include "Wire.h"
class ExtensionIOXL9555 {
public:
  enum { PORT0 = 0, PORT1 = 1 };
  bool present = true;
  bool begin(TwoWire&, uint8_t) { return present; }
  void pinMode(uint8_t, int) {}
  void digitalWrite(uint8_t, int) {}
  int readPort(int) { return 0x04; }
};
