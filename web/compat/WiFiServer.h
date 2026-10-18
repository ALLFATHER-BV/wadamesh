#pragma once
// A TCP server that never accepts (the web board serves nothing in phase 1).
#include "WiFiClient.h"
class WiFiServer {
public:
  explicit WiFiServer(uint16_t port = 80, uint8_t max_clients = 4) { (void)port; (void)max_clients; }
  void begin(uint16_t port = 0) { (void)port; }
  void end() {}
  void close() {}
  void stop() {}
  void setNoDelay(bool) {}
  bool hasClient() { return false; }
  WiFiClient available() { return WiFiClient(); }
  WiFiClient accept() { return WiFiClient(); }
  operator bool() { return false; }
};
