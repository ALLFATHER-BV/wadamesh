#pragma once

// Wadamesh-owned NMEA location provider: the core's MicroNMEALocationProvider
// plus the motion fields it keeps private.
//
// WHY A COPY: MeshCore's MicroNMEALocationProvider (helpers/sensors/) holds its
// `MicroNMEA nmea` parser as a private member and the LocationProvider
// interface exposes position/altitude/satellites/time only — speed-over-ground
// and course-over-ground are parsed from every RMC sentence and then thrown
// away. A subclass cannot reach them, and patching the core header in libdeps
// is exactly the build-fragile dependency patch this repo avoids. So the
// provider is re-owned here: same behaviour line for line (claim/release, the
// time-sync rules from #89's follow-up, GPS_EN/GPS_RESET polarity macros from
// the core header), plus motion(). Boards opt in by constructing this class in
// their target.cpp instead of the core one and defining HAS_GPS_MOTION; the
// wada.sys.gps() binding then reports speed_kmh/course on that board.
//
// Kept in lock-step with the core header it mirrors (meshcomod core-v1.17.4);
// if the core provider gains behaviour, port it here too.

#include <helpers/sensors/MicroNMEALocationProvider.h>   // GPS_EN/GPS_RESET macros, LocationProvider
#include <MicroNMEA.h>
#include <RTClib.h>
#include <helpers/RefCountedDigitalPin.h>
#include <limits.h>

class WadaNmeaLocationProvider : public LocationProvider {
  char _nmeaBuffer[100];
  MicroNMEA nmea;
  mesh::RTCClock* _clock;
  Stream* _gps_serial;
  RefCountedDigitalPin* _peripher_power;
  int8_t _claims = 0;
  int _pin_reset;
  int _pin_en;
  unsigned long next_check = 0;
  long time_valid = 0;
  unsigned long _last_time_sync = 0;
  static const unsigned long TIME_SYNC_INTERVAL = 1800000;  // re-sync every 30 minutes

public:
  WadaNmeaLocationProvider(Stream& ser, mesh::RTCClock* clock = NULL, int pin_reset = GPS_RESET,
                           int pin_en = GPS_EN, RefCountedDigitalPin* peripher_power = NULL)
      : nmea(_nmeaBuffer, sizeof(_nmeaBuffer)), _clock(clock), _gps_serial(&ser),
        _peripher_power(peripher_power), _pin_reset(pin_reset), _pin_en(pin_en) {
    if (_pin_reset != -1) {
      pinMode(_pin_reset, OUTPUT);
      digitalWrite(_pin_reset, GPS_RESET_ACTIVE);
    }
    if (_pin_en != -1) {
      pinMode(_pin_en, OUTPUT);
      digitalWrite(_pin_en, !GPS_EN_ACTIVE);
    }
  }

  void claim() {
    _claims++;
    if (_peripher_power) _peripher_power->claim();
  }

  void release() {
    if (_claims == 0) return;  // avoid negative _claims
    _claims--;
    if (_peripher_power) _peripher_power->release();
  }

  void begin() override {
    claim();
    if (_pin_en != -1) digitalWrite(_pin_en, GPS_EN_ACTIVE);
    if (_pin_reset != -1) digitalWrite(_pin_reset, !GPS_RESET_ACTIVE);
  }

  void reset() override {
    if (_pin_reset != -1) {
      digitalWrite(_pin_reset, GPS_RESET_ACTIVE);
      delay(10);
      digitalWrite(_pin_reset, !GPS_RESET_ACTIVE);
    }
  }

  void stop() override {
    if (_pin_en != -1) digitalWrite(_pin_en, !GPS_EN_ACTIVE);
    if (_pin_reset != -1) digitalWrite(_pin_reset, GPS_RESET_ACTIVE);
    release();
  }

  bool isEnabled() override {
    // Read the enable pin directly if present: the GPS can be switched outside
    // of here.
    if (_pin_en != -1) return digitalRead(_pin_en) == GPS_EN_ACTIVE;
    return true;  // no enable pin, so it must be active
  }

  void syncTime() override { nmea.clear(); LocationProvider::syncTime(); }
  long getLatitude() override { return nmea.getLatitude(); }
  long getLongitude() override { return nmea.getLongitude(); }
  long getAltitude() override {
    long alt = 0;
    nmea.getAltitude(alt);
    return alt;
  }
  long satellitesCount() override { return nmea.getNumSatellites(); }
  bool isValid() override { return nmea.isValid(); }

  long getTimestamp() override {
    DateTime dt(nmea.getYear(), nmea.getMonth(), nmea.getDay(), nmea.getHour(), nmea.getMinute(),
                nmea.getSecond());
    return dt.unixtime();
  }

  void sendSentence(const char* sentence) override { nmea.sendSentence(*_gps_serial, sentence); }

  // ---- the addition -------------------------------------------------------
  // Speed and course over ground from the last RMC sentence. MicroNMEA stores
  // knots×1000 and degrees×1000, LONG_MIN after clear(). An EMPTY course field
  // (a stationary receiver emits one) parses as 0, which is indistinguishable
  // from "heading north" — so course is reported only while moving faster
  // than `min_course_kmh`. Returns false with nothing to report (no fix, or
  // no RMC yet); speed_kmh is always set when true, course_deg is NAN when
  // the device is not moving enough to trust it.
  bool motion(float* speed_kmh, float* course_deg, float min_course_kmh = 1.0f) {
    if (!nmea.isValid()) return false;
    const long sp = nmea.getSpeed();
    if (sp == LONG_MIN || sp < 0) return false;
    const float kmh = (float)sp * 1.852f / 1000.0f;
    if (speed_kmh) *speed_kmh = kmh;
    if (course_deg) {
      const long co = nmea.getCourse();
      *course_deg = (co != LONG_MIN && kmh >= min_course_kmh) ? (float)co / 1000.0f : NAN;
    }
    return true;
  }

  void loop() override {
    while (_gps_serial->available()) {
      char c = _gps_serial->read();
#ifdef GPS_NMEA_DEBUG
      Serial.print(c);
#endif
      nmea.process(c);
    }

    if (!isValid()) time_valid = 0;

    if ((long)(millis() - next_check) > 0) {
      next_check = millis() + 1000;
      // Re-enable time sync periodically when GPS has a valid fix.
      if (!_time_sync_needed && _clock != NULL && (millis() - _last_time_sync) > TIME_SYNC_INTERVAL) {
        _time_sync_needed = true;
      }
      if (_time_sync_needed && time_valid > 2) {
        // Only trust the GPS time once it carries a sane DATE. A position fix
        // can arrive before the date is decoded (nmea.getYear()==0), and
        // DateTime(0,...).unixtime() is ~1902-10-11 — pushing that into the
        // RTC stamps our adverts as decades old so peers reject them as stale,
        // and it clobbers a good NTP time (re-syncing every 30 min). Keep
        // _time_sync_needed set so we retry once a real date lands.
        if (_clock != NULL && nmea.getYear() >= 2020) {
          _clock->setCurrentTime(getTimestamp());
          _time_sync_needed = false;
          _last_time_sync = millis();
        }
      }
      if (isValid()) time_valid++;
    }
  }
};
