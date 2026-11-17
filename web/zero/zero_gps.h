// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The Cap LoRa-1262 GPS (NMEA on /dev/serial0) as the UI's GPS fix, plus the GPX track
// and WiGLE Wi-Fi logs (zero_gps.cpp).
#include <string>
void zeroGpsBegin();
void zeroGpsPoll();
struct ZeroGpsStatus {
  bool port = false, nmea = false, fix = false;
  std::string path;
  double lat = 0, lon = 0, alt = 0, speed_mps = 0;
  int sats = 0;
  bool gpx_on = false, wifi_on = false;
  int gpx_points = 0, wifi_seen = 0;
  std::string gpx_path;
};
ZeroGpsStatus zeroGpsStatus();
void zeroGpsSetGpx(bool on);
void zeroGpsSetWifiLog(bool on);
void zeroGpsShutdown();
