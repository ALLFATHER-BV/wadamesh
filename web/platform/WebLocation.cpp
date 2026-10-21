// SPDX-License-Identifier: GPL-3.0-or-later
// The web board's GPS (see WebLocation.h): the host's location behind the UI's
// LocationProvider. Everything here runs on the UI thread (the browser main thread:
// geolocation callbacks and the UI loop share it).
#include "WebLocation.h"
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

// The core declares this one non-pure virtual without a body (a board's NMEA provider
// .cpp supplies it); it is LocationProvider's key function, so its vtable lives here.
void LocationProvider::sendSentence(const char*) {}

namespace {
struct HostFix {
  bool valid = false;
  double lat = 0, lon = 0;
  double acc_m = NAN, alt_m = NAN, speed_mps = NAN, heading_deg = NAN;
  double epoch = 0;
};
HostFix s_fix;
bool s_seen = false;   // a fix arrived this session (wadaHostLocation)
}  // namespace

#if defined(__EMSCRIPTEN__)
// The page owns navigator.geolocation (web/shell/app.js Module.wadaGeoStart / Stop),
// because only it sees the visitor's taps: a permission prompt is shown on one.
EM_JS(void, wada_js_geo_start, (), { if (Module["wadaGeoStart"]) Module["wadaGeoStart"](); });
EM_JS(void, wada_js_geo_stop, (), { if (Module["wadaGeoStop"]) Module["wadaGeoStop"](); });
extern "C" {
EMSCRIPTEN_KEEPALIVE void wadaWebGeoFix(double lat, double lon, double acc, double alt,
                                        double speed, double heading, double epoch) {
  wadaHostLocationFix(lat, lon, acc, alt, speed, heading, epoch);
}
EMSCRIPTEN_KEEPALIVE void wadaWebGeoLost(int reason) { wadaHostLocationLost(reason); }
}
#endif

void wadaHostLocationFix(double lat, double lon, double accuracy_m, double alt_m,
                         double speed_mps, double heading_deg, double epoch_s) {
  if (!(lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0)) return;
  if (lat == 0.0 && lon == 0.0) return;   // the UI reads 0,0 as "no position"
  s_fix.valid = true;
  s_fix.lat = lat; s_fix.lon = lon;
  s_fix.acc_m = accuracy_m; s_fix.alt_m = alt_m;
  s_fix.speed_mps = speed_mps; s_fix.heading_deg = heading_deg;
  s_fix.epoch = epoch_s > 0 ? epoch_s : (double)time(nullptr);
  s_seen = true;
}
void wadaHostLocationLost(int reason) {
  // Refused: no position at all (an earlier fix must not linger as if it were current).
  // Unavailable: the same to the UI, which keeps "searching".
  s_fix.valid = false;
  Serial.printf("[web] location: %s\n", reason == 1 ? "not allowed" : "unavailable");
}
bool wadaHostLocation(double& lat, double& lon) {
  if (!s_seen) return false;
  lat = s_fix.lat; lon = s_fix.lon;
  return true;
}

// ---- the provider -------------------------------------------------------------------------
long WebLocationProvider::getLatitude() { return s_fix.valid ? (long)llround(s_fix.lat * 1.0e6) : 0; }
long WebLocationProvider::getLongitude() { return s_fix.valid ? (long)llround(s_fix.lon * 1.0e6) : 0; }
long WebLocationProvider::getAltitude() {
  return (s_fix.valid && !isnan(s_fix.alt_m)) ? (long)llround(s_fix.alt_m * 1000.0) : 0;
}
bool WebLocationProvider::isValid() { return s_fix.valid; }
long WebLocationProvider::getTimestamp() { return s_fix.valid ? (long)s_fix.epoch : 0; }

void WebLocationProvider::begin() {
  if (_enabled) return;
  _enabled = true;
#if defined(__EMSCRIPTEN__)
  wada_js_geo_start();
#else
  // Native: a fixed position for development, e.g. WADA_GPS="51.05,3.72".
  if (const char* e = getenv("WADA_GPS")) {
    double la = 0, lo = 0;
    if (sscanf(e, "%lf,%lf", &la, &lo) == 2) wadaHostLocationFix(la, lo, 0, NAN, NAN, NAN, 0);
  }
#endif
}
void WebLocationProvider::stop() {
  if (!_enabled) return;
  _enabled = false;
#if defined(__EMSCRIPTEN__)
  wada_js_geo_stop();
#endif
}
void WebLocationProvider::formatDiagnostics(char* out, size_t out_size) {
  if (!out_size) return;
  if (s_fix.valid && !isnan(s_fix.acc_m))
    snprintf(out, out_size, "en:%u fix:1 acc:%.0fm (host location)", _enabled ? 1U : 0U, s_fix.acc_m);
  else
    snprintf(out, out_size, "en:%u fix:%u (host location)", _enabled ? 1U : 0U, s_fix.valid ? 1U : 0U);
}

const char* WebSensorManager::getSettingValue(int i) const {
  return i == 0 ? (_gps.isEnabled() ? "1" : "0") : nullptr;
}
bool WebSensorManager::setSettingValue(const char* name, const char* value) {
  if (!name || strcmp(name, "gps") != 0 || !value) return false;
  if (strcmp(value, "0") == 0) _gps.stop(); else _gps.begin();
  return true;
}

// Speed and course for wada.sys.gps() and the GPS Home card (HAS_GPS_MOTION, as the M9
// provides from its RMC fields): the browser's coords.speed / coords.heading, when given.
bool wadaGpsMotion(float* speed_kmh, float* course_deg) {
  if (!s_fix.valid || isnan(s_fix.speed_mps)) return false;
  if (speed_kmh) *speed_kmh = (float)(s_fix.speed_mps * 3.6);
  // heading is NaN when standing still (the spec's "no course"), which the callers omit
  if (course_deg) *course_deg = isnan(s_fix.heading_deg) ? NAN : (float)s_fix.heading_deg;
  return true;
}
