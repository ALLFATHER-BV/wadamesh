// SPDX-License-Identifier: GPL-3.0-or-later
//
// T-LoRa Pager location provider: the wadamesh NMEA provider with the u-blox
// MIA-M10Q's two ways to rest.
//
// The core provider this board used before did nothing on stop(): the module
// kept tracking with GPS switched off and through every rest of the GPS saver.
// Now:
//  - stop() (a saver rest, or the first step of switching off) puts the receiver
//    in software standby. It keeps its ephemeris and time, so the next begin()
//    is a hot start of a second or two.
//  - powerDown() takes the module's rail down (XL9555 GPS_EN, with GPS_RST held
//    LOW), for GPS switched off and for power-off. The UART is released and its
//    lines parked LOW, so the ESP32 does not feed the unpowered module through
//    its RX pin.
// begin() brings the rail back when it is down. GPS_EN has no GPIO, so the base
// provider gets -1 for enable and reset, as on the T-Deck Max.
//
// A rail that just came up is a cold start: the receiver knows neither the time
// nor where it is, and searches the whole sky for every satellite. Once it is
// talking, it is told both (UBX-MGA-INI-TIME_UTC from the RTC, UBX-MGA-INI-POS_LLH
// from the node's last position), when this side knows them. That narrows the
// search to the satellites actually overhead; it still has to download their
// orbits, but the first fix comes noticeably sooner, most of all under a weak sky.
#pragma once

#include "../../src/helpers/WadaNmeaLocationProvider.h"
#include "TLoraPagerBoard.h"

extern TLoraPagerBoard board;

class PagerGps : public WadaNmeaLocationProvider {
  HardwareSerial& _uart;
  int _rx, _tx;
  uint32_t _default_baud;
  mesh::RTCClock* _rtc;
  uint32_t _aid_at = 0;          // millis() to send the aiding, 0 = nothing pending
  bool     _need_aid = true;     // the rail came up (at boot too) and nothing was sent since
  bool   (*_aid_pos)(double* lat, double* lon) = nullptr;   // where the node last was
  bool   (*_aid_time_ok)() = nullptr;                       // the clock holds a real time

public:
  // serial_rx is the pin the ESP32 receives on (the module's TX), as on the Pro.
  PagerGps(HardwareSerial& ser, mesh::RTCClock* clock, int serial_rx, int serial_tx,
           uint32_t default_baud)
      : WadaNmeaLocationProvider(ser, clock, -1 /*reset*/, -1 /*enable*/,
                                 serial_rx, serial_tx, default_baud),
        _uart(ser), _rx(serial_rx), _tx(serial_tx), _default_baud(default_baud), _rtc(clock) {
    setUbxStandby(true);
  }

  // Where the node last was and whether the clock can be trusted, asked for when the
  // aiding goes out (target.cpp: the sensor manager's stored position, and
  // ClockFloorRTC::timeIsCurrent()).
  void setAidSource(bool (*pos)(double* lat, double* lon), bool (*time_ok)()) {
    _aid_pos = pos;
    _aid_time_ok = time_ok;
  }

  void begin() override {
    if (!board.gpsPowerIsOn()) railUp();
    WadaNmeaLocationProvider::begin();
    scheduleAid();
  }

  void loop() override {
    if (!board.gpsPowerIsOn()) { railUp(); scheduleAid(); }   // begin() found the expander busy: try again
    if (_aid_at && (int32_t)(millis() - _aid_at) >= 0) { _aid_at = 0; sendAiding(); }
    WadaNmeaLocationProvider::loop();
  }

  bool isEnabled() override { return board.gpsPowerIsOn(); }

  // Rail off. Idempotent and cheap: the UI calls it on every pass while GPS is off. A busy
  // or missing expander leaves everything as it was.
  void powerDown() {
    if (!board.gpsPowerIsOn() || !board.setGpsPower(false)) return;
    _aid_at = 0;
    _uart.end();
    pinMode(_tx, INPUT_PULLDOWN);
    pinMode(_rx, INPUT_PULLDOWN);
  }

private:
  void railUp() {
    if (!board.setGpsPower(true)) return;
    _uart.setPins(_rx, _tx);   // the same order and baud as the core's initBasicGPS()
    _uart.begin(touchPrefsGetGpsBaud(_default_baud));
    _need_aid = true;
  }
  void scheduleAid() {
    if (!_need_aid || !board.gpsPowerIsOn()) return;
    _need_aid = false;
    // The module boots for about a second before it listens, and at boot the stored
    // position is only loaded a moment after the GPS starts.
    _aid_at = millis() + 3000;
    if (!_aid_at) _aid_at = 1;
  }

  void ubxSend(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len) {
    uint8_t hdr[6] = { 0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6; ++i) { a += hdr[i]; b += a; }
    for (uint16_t i = 0; i < len; ++i) { a += payload[i]; b += a; }
    _uart.write(hdr, sizeof hdr);
    _uart.write(payload, len);
    _uart.write(a);
    _uart.write(b);
  }
  static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

  void sendAiding() {
    bool sent = false;
    // Only a time that is known to be right (NTP, GPS, the phone, or a hardware clock
    // that kept it): the clock floor never reads less than its seed or the saved floor,
    // so after a power-off a plain "plausible" check passes a stale time, and a wrong
    // time with a tight accuracy slows the first fix instead of speeding it.
    const bool time_ok = _aid_time_ok && _aid_time_ok();
    const uint32_t now = (time_ok && _rtc) ? _rtc->getCurrentTime() : 0;
    if (now > 1700000000u) {
      DateTime dt(now);
      uint8_t t[24] = { 0 };
      t[0] = 0x10;                          // type: TIME_UTC
      t[2] = 0;                             // ref: none (no time pulse), applies on receipt
      t[3] = (uint8_t)(int8_t)-128;         // leap seconds unknown
      t[4] = dt.year() & 0xFF; t[5] = dt.year() >> 8;
      t[6] = dt.month(); t[7] = dt.day(); t[8] = dt.hour(); t[9] = dt.minute(); t[10] = dt.second();
      t[16] = 5;                            // tAccS: 5 s, generous for the PCF85063A
      ubxSend(0x13, 0x40, t, sizeof t);
      sent = true;
    }
    double la = 0, lo = 0;
    const bool have_pos = _aid_pos && _aid_pos(&la, &lo) && la >= -90 && la <= 90 && lo >= -180 &&
                          lo <= 180 && !(la == 0 && lo == 0);
    if (have_pos) {
      uint8_t p[20] = { 0 };
      p[0] = 0x01;                          // type: POS_LLH
      put32(p + 4, (uint32_t)(int32_t)(la * 1e7));
      put32(p + 8, (uint32_t)(int32_t)(lo * 1e7));
      put32(p + 12, 0);                     // altitude 0 cm, covered by the accuracy below
      put32(p + 16, 10000000u);             // 100 km: the last position may be from elsewhere
      ubxSend(0x13, 0x40, p, sizeof p);
      sent = true;
    }
    if (sent) {
      _uart.flush();
      Serial.printf("[GPS] cold start aided:%s%s\n", now > 1700000000u ? " time" : "",
                    have_pos ? " position" : "");
    }
  }
};
