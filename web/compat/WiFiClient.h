#pragma once
// A TCP client that never connects (no sockets in phase 1; WebSocket/fetch in phase 2/3).
#include "Arduino.h"
#include "arduino/Client.h"
class WiFiClient : public Client {
public:
  WiFiClient() {}
  int connect(IPAddress ip, uint16_t port) override { (void)ip; (void)port; return 0; }
  int connect(IPAddress ip, uint16_t port, int32_t timeout) { (void)ip; (void)port; (void)timeout; return 0; }
  int connect(const char* host, uint16_t port) override { (void)host; (void)port; return 0; }
  int connect(const char* host, uint16_t port, int32_t timeout) { (void)host; (void)port; (void)timeout; return 0; }
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) override { return 0; }
  int available() override { return 0; }
  int read() override { return -1; }
  int read(uint8_t*, size_t) override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  void stop() override {}
  uint8_t connected() override { return 0; }
  operator bool() override { return false; }
  int fd() const { return -1; }
  int setNoDelay(bool) { return 0; }
  int setTimeout(uint32_t) { return 0; }
  IPAddress remoteIP() const { return IPAddress((uint32_t)0); }
  uint16_t remotePort() const { return 0; }
  using Print::write;
};
