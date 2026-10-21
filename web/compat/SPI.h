#pragma once
// SPI: no bus on a host.
#include "Arduino.h"
#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3
#define FSPI 0
#define HSPI 1
class SPISettings {
public:
  SPISettings(uint32_t clock = 1000000, uint8_t bitOrder = MSBFIRST, uint8_t dataMode = SPI_MODE0) { (void)clock; (void)bitOrder; (void)dataMode; }
};
class SPIClass {
public:
  explicit SPIClass(uint8_t bus = 0) { (void)bus; }
  void begin(int8_t sck = -1, int8_t miso = -1, int8_t mosi = -1, int8_t ss = -1) { (void)sck; (void)miso; (void)mosi; (void)ss; }
  void end() {}
  void beginTransaction(SPISettings) {}
  void endTransaction() {}
  uint8_t transfer(uint8_t) { return 0xFF; }
  void transfer(void* buf, uint32_t n) { memset(buf, 0xFF, n); }
  void setFrequency(uint32_t) {}
};
extern SPIClass SPI;
