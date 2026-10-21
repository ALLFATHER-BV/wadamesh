#pragma once
// I2C: no bus on a host. Every transaction NACKs, so probes find nothing.
#include "Arduino.h"
class TwoWire : public Stream {
public:
  explicit TwoWire(uint8_t bus = 0) : _bus(bus) {}
  bool begin(int sda = -1, int scl = -1, uint32_t freq = 0) { (void)sda; (void)scl; (void)freq; return true; }
  bool end() { return true; }
  bool setClock(uint32_t) { return true; }
  void setTimeOut(uint16_t) {}
  void beginTransmission(uint8_t) {}
  uint8_t endTransmission(bool stop = true) { (void)stop; return 2; }   // 2 = address NACK
  uint8_t requestFrom(uint8_t addr, uint8_t n, bool stop = true) { (void)addr; (void)n; (void)stop; return 0; }
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) override { return 0; }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  using Print::write;
private:
  uint8_t _bus;
};
extern TwoWire Wire;
extern TwoWire Wire1;
