#pragma once
#include <cstdint>
#define BD_STARTUP_RX_PACKET 1
class ESP32Board {
public:
  uint8_t startup_reason = 0;
  void begin() {}
};
