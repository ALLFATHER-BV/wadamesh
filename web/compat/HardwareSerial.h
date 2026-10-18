#pragma once
// Serial on a host: output goes to stdout (line-buffered), input is empty. Serial1/2
// exist (a GPS UART on some boards) but have nothing attached.
#include "arduino/Stream.h"
class HardwareSerial : public Stream {
public:
  explicit HardwareSerial(int num) : _num(num) {}
  void begin(unsigned long baud, uint32_t config = 0, int8_t rx = -1, int8_t tx = -1,
             bool invert = false, unsigned long timeout_ms = 20000UL, uint8_t thresh = 112) {
    (void)baud; (void)config; (void)rx; (void)tx; (void)invert; (void)timeout_ms; (void)thresh;
  }
  void end(bool = true) {}
  void updateBaudRate(unsigned long) {}
  size_t setRxBufferSize(size_t n) { return n; }
  size_t setTxBufferSize(size_t n) { return n; }
  void setTimeout(unsigned long) {}
  void setDebugOutput(bool) {}
  void setTxTimeoutMs(uint32_t) {}
  int available() override { return 0; }
  int availableForWrite() override { return 4096; }
  int peek() override { return -1; }
  int read() override { return -1; }
  size_t read(uint8_t* buf, size_t size) { (void)buf; (void)size; return 0; }
  void flush() override;
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buf, size_t size) override;
  using Print::write;
  operator bool() const { return true; }
  uint32_t baudRate() { return 115200; }
private:
  int _num;
};
extern HardwareSerial Serial;
extern HardwareSerial Serial1;
extern HardwareSerial Serial2;
#define SERIAL_8N1 0x800001c
