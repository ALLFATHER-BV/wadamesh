#pragma once
#include "WiFiClient.h"
class WiFiClientSecure : public WiFiClient {
public:
  void setInsecure() {}
  void setCACert(const char*) {}
  void setHandshakeTimeout(unsigned long) {}
};
