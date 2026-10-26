// SPDX-License-Identifier: GPL-3.0-or-later
// Fake SensorLib BHI260AP: begin() is the firmware upload (it records the bus clock it
// ran at), configure() records the rate, update() hands queued samples to the
// registered callback the way the real FIFO drain does.
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include "Wire.h"
#include "bosch/bhy2_parse.h"
typedef void (*SensorDataParseCallback)(uint8_t, uint8_t*, uint32_t, uint64_t*, void*);
struct FakeBhy {
  bool upload_ok = true;
  int begins = 0, configures = 0;
  float rate = -1;
  uint32_t upload_clock = 0;
  std::vector<bhy2_data_xyz> fifo;
};
extern FakeBhy g_bhy;
class SensorBHI260AP {
  SensorDataParseCallback cb_ = nullptr;
public:
  enum BoschSensorID { ACCEL_PASSTHROUGH = 1 };
  enum SensorRemap { BOTTOM_LAYER_TOP_LEFT_CORNER = 4 };
  void setFirmware(const uint8_t*, size_t, bool = false, bool = false) {}
  void setBootFromFlash(bool) {}
  bool begin(TwoWire& w, uint8_t) { ++g_bhy.begins; g_bhy.upload_clock = w.getClock(); return g_bhy.upload_ok; }
  bool setRemapAxes(SensorRemap) { return true; }
  bool onResultEvent(BoschSensorID, SensorDataParseCallback cb, void* = nullptr) { cb_ = cb; return true; }
  bool configure(uint8_t, float r, uint32_t) { ++g_bhy.configures; g_bhy.rate = r; return true; }
  float getScaling(uint8_t) { return 1.0f / 4096.0f; }
  void update() {
    for (auto& v : g_bhy.fifo) { uint8_t raw[6]; memcpy(raw, &v, 6); if (cb_) cb_(ACCEL_PASSTHROUGH, raw, 6, nullptr, nullptr); }
    g_bhy.fifo.clear();
  }
};
