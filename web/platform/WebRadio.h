#pragma once
// The web board has no LoRa radio of its own: the mesh lives behind the backend
// (web/mesh). This is the radio_driver global the UI's radio tools read (Spectrum,
// RF monitor, airtime). Like the stand-in it declares only what the UI calls, so a new
// radio_driver.foo() in the UI fails to compile here too. Every reading reports
// "no radio" (0 / false / -128 dBm) and every setter is a no-op.
#include <stdint.h>
class WebRadio {
public:
  const char* chipName() const { return "none (web)"; }
  void setParams(float freq, float bw, uint8_t sf, uint8_t cr) { (void)freq; (void)bw; (void)sf; (void)cr; }
  void setTxPower(int8_t dbm) { (void)dbm; }
  int16_t setRxBoostedGainMode(bool en) { (void)en; return -1; }
  void rxQueueEnable(bool en) { (void)en; }
  void rxQueueSuspend(bool s) { (void)s; }
  bool rxQueueEnabled() const { return false; }
  uint32_t getRxQueueDrops() const { return 0; }
  uint32_t getRxEvents() const { return 0; }
  uint32_t getPacketsRecv() const { return 0; }
  uint32_t getPacketsRecvErrors() const { return 0; }
  uint32_t getPacketsSent() const { return 0; }
  int getNoiseFloor() const { return 0; }
  float getCurrentRSSI() { return -128.0f; }
  int recvRaw(uint8_t* bytes, int sz) { (void)bytes; (void)sz; return 0; }
  void radioAcquire() {}
  void radioRelease() {}
};
