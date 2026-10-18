// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The web board's GPS: the location of the computer or phone the page runs on.
//
// The UI gets a position on every board the same way: SensorManager::getLocationProvider()
// (a LocationProvider), switched on and off with setSettingValue("gps", "1"/"0") by
// UITask::toggleGPS / setGpsPower, copied into sensors.node_lat/lon by
// UITask::updateGpsLocation. This board's provider is fed by the host instead of NMEA:
//
//   browser  navigator.geolocation.watchPosition (web/shell/app.js), started only while
//            the UI has the GPS switched on, and asked for only on a tap when the page
//            has no decision from the visitor yet (never on page load)
//   native   WADA_GPS="lat,lon" in the environment, or no fix at all
//
// Denied or unavailable is simply "no fix" (the UI's "searching"); nothing retries in a
// loop. Where the position may go from here is WebMesh.cpp's syncAdvertPosition (the
// same advert-location setting a device's GPS answers to; WEB_BUILD.md "Location").
#include <helpers/SensorManager.h>
#include <helpers/sensors/LocationProvider.h>
#include <stdint.h>

class WebLocationProvider : public LocationProvider {
public:
  long getLatitude() override;            // micro-degrees, as MicroNMEA reports them
  long getLongitude() override;
  long getAltitude() override;            // millimetres (0 when the host gives none)
  long satellitesCount() override { return 0; }   // a browser fix has no satellites to count
  bool isValid() override;
  long getTimestamp() override;           // epoch seconds of the fix
  void sendSentence(const char*) override {}
  void reset() override {}
  void begin() override;                  // the UI switched the GPS on (or woke it from a rest)
  void stop() override;                   // off, or resting: the last fix is kept, as a receiver's is
  void loop() override {}
  bool isEnabled() override { return _enabled; }
  void formatDiagnostics(char* out, size_t out_size) override;
private:
  bool _enabled = false;
};

class WebSensorManager : public SensorManager {
public:
  bool begin() override { return true; }
  int getNumSettings() const override { return 1; }
  const char* getSettingName(int i) const override { return i == 0 ? "gps" : nullptr; }
  const char* getSettingValue(int i) const override;
  bool setSettingValue(const char* name, const char* value) override;
  LocationProvider* getLocationProvider() override { return &_gps; }
  bool isGPSDetected() const override { return true; }
private:
  mutable WebLocationProvider _gps;
};

// ---- the host side ----------------------------------------------------------------------
// A position from the host (main thread). NaN = not reported (altitude, speed, heading).
void wadaHostLocationFix(double lat, double lon, double accuracy_m, double alt_m,
                         double speed_mps, double heading_deg, double epoch_s);
// The host will not give one: 1 = the visitor or the browser refused, 2 = unavailable.
void wadaHostLocationLost(int reason);
// The last fix this computer reported in this session (the demo builds its neighbourhood
// around it). Local only: nothing here sends it anywhere.
bool wadaHostLocation(double& lat, double& lon);
// Speed (km/h) and course (degrees, NaN when standing) of the last fix, as the M9's
// target.h declares it for HAS_GPS_MOTION; false = the host gave no speed.
bool wadaGpsMotion(float* speed_kmh, float* course_deg);
