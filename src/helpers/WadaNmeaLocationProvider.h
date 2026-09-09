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
// the core header), plus motion() and optional checksum-driven UART baud
// recovery. Boards opt in by constructing this class in their target.cpp
// instead of the core one. HAS_GPS_MOTION exposes speed/course to Lua; passing
// the UART probe arguments enables baud recovery independently.
//
// Kept in lock-step with the core header it mirrors (meshcomod core-v1.17.4);
// if the core provider gains behaviour, port it here too. ONE deliberate
// deviation (2026-09-02 battery pass): the reset line is parked LOW — not
// asserted — while the GPS is stopped; see the comment in the constructor.

#include <helpers/sensors/MicroNMEALocationProvider.h>   // GPS_EN/GPS_RESET macros, LocationProvider
#include <MicroNMEA.h>
#include <RTClib.h>
#include <helpers/RefCountedDigitalPin.h>
#include "esp32/TouchPrefsStore.h"
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
  HardwareSerial* _probe_serial = nullptr;
  int8_t _probe_rx = -1;
  int8_t _probe_tx = -1;
  uint8_t _probe_cursor = 0;
  uint32_t _probe_default_baud = 0;
  uint32_t _active_baud = 0;
  uint32_t _probe_deadline_ms = 0;
  bool _stream_locked = false;
  static const unsigned long TIME_SYNC_INTERVAL = 1800000;  // re-sync every 30 minutes
  static const unsigned long BAUD_PROBE_START_MS = 9000;
  static const unsigned long BAUD_PROBE_INTERVAL_MS = 5000;

  void nudgeReceiver() {
    if (!_probe_serial) return;
    _probe_serial->write((uint8_t)'\r');
    _probe_serial->write((uint8_t)'\n');
    _probe_serial->flush();
  }

  void probeNextBaud() {
    if (!_probe_serial || _probe_rx < 0) return;
    const uint32_t candidates[] = {
      _probe_default_baud, 9600u, 19200u, 38400u, 57600u, 115200u
    };
    for (size_t attempt = 0; attempt < sizeof(candidates) / sizeof(candidates[0]); ++attempt) {
      const uint32_t candidate = candidates[_probe_cursor];
      _probe_cursor = (uint8_t)((_probe_cursor + 1) %
                                (sizeof(candidates) / sizeof(candidates[0])));
      if (candidate == 0 || candidate == _active_baud) continue;
      _probe_serial->end();
      _probe_serial->setPins(_probe_rx, _probe_tx);
      _probe_serial->begin(candidate);
      nmea.clear();
      time_valid = 0;
      _active_baud = candidate;
      _probe_deadline_ms = millis() + BAUD_PROBE_INTERVAL_MS;
      nudgeReceiver();
      return;
    }
  }

  void noteValidSentence() {
    if (!_probe_serial || _stream_locked) return;
    _stream_locked = true;
    _probe_deadline_ms = 0;
    if (touchPrefsGetGpsBaud(_probe_default_baud) != _active_baud)
      (void)touchPrefsSetGpsBaud(_active_baud);
  }

public:
  WadaNmeaLocationProvider(Stream& ser, mesh::RTCClock* clock = NULL, int pin_reset = GPS_RESET,
                           int pin_en = GPS_EN, RefCountedDigitalPin* peripher_power = NULL)
      : nmea(_nmeaBuffer, sizeof(_nmeaBuffer)), _clock(clock), _gps_serial(&ser),
        _peripher_power(peripher_power), _pin_reset(pin_reset), _pin_en(pin_en) {
    if (_pin_reset != -1) {
      pinMode(_pin_reset, OUTPUT);
      // Park the reset line LOW while the GPS is off — NOT "asserted" (the
      // core's convention). On a direct-wired active-LOW reset LOW *is*
      // asserted, so this changes nothing there. On the M9 the polarity is
      // inverted by a transistor (GPS_RST1 -> R46 -> NPN Q16: GPIO HIGH =
      // reset asserted), so the core convention holds the GPIO HIGH and
      // sources (3.3 V - Vbe)/R46 of Q16 base current the entire time the
      // GPS is OFF — its default state — for a module that is unpowered
      // anyway (EN inactive cuts its rail; the reset level is moot). LOW is
      // the zero-current level for both circuits. A clean reset at power-on
      // is still guaranteed: every start path (initBasicGPS / start_gps)
      // calls begin() then reset(), and reset() pulses the line explicitly.
      digitalWrite(_pin_reset, LOW);
    }
    if (_pin_en != -1) {
      pinMode(_pin_en, OUTPUT);
      digitalWrite(_pin_en, !GPS_EN_ACTIVE);
    }
  }

  WadaNmeaLocationProvider(HardwareSerial& ser, mesh::RTCClock* clock,
                           int pin_reset, int pin_en, int serial_rx, int serial_tx,
                           uint32_t default_baud)
      : WadaNmeaLocationProvider(static_cast<Stream&>(ser), clock, pin_reset, pin_en) {
    _probe_serial = &ser;
    _probe_rx = (int8_t)serial_rx;
    _probe_tx = (int8_t)serial_tx;
    _probe_default_baud = default_baud;
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
    if (_probe_serial) {
      _active_baud = touchPrefsGetGpsBaud(_probe_default_baud);
      _probe_cursor = 0;
      _stream_locked = false;
      _probe_deadline_ms = millis() + BAUD_PROBE_START_MS;
      nudgeReceiver();
    }
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
    // LOW, not GPS_RESET_ACTIVE — the zero-current park level for both reset
    // circuits (see the constructor comment; on the M9 "asserted" = GPIO HIGH
    // through R46 into Q16's base = a constant drain while the GPS is off).
    if (_pin_reset != -1) digitalWrite(_pin_reset, LOW);
    _probe_deadline_ms = 0;
    _stream_locked = false;
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
      const bool sentence_end = c == '\r' || c == '\n';
      nmea.process(c);
      if (sentence_end) {
        const char* sentence = nmea.getSentence();
        if (sentence && sentence[0] == '$' && MicroNMEA::testChecksum(sentence))
          noteValidSentence();
      }
    }

    if (_probe_serial && !_stream_locked && _probe_deadline_ms &&
        (int32_t)(millis() - _probe_deadline_ms) >= 0)
      probeNextBaud();

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
