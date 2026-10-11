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
#pragma once

#include "../../src/helpers/WadaNmeaLocationProvider.h"
#include "TLoraPagerBoard.h"

extern TLoraPagerBoard board;

class PagerGps : public WadaNmeaLocationProvider {
  HardwareSerial& _uart;
  int _rx, _tx;
  uint32_t _default_baud;

public:
  // serial_rx is the pin the ESP32 receives on (the module's TX), as on the Pro.
  PagerGps(HardwareSerial& ser, mesh::RTCClock* clock, int serial_rx, int serial_tx,
           uint32_t default_baud)
      : WadaNmeaLocationProvider(ser, clock, -1 /*reset*/, -1 /*enable*/,
                                 serial_rx, serial_tx, default_baud),
        _uart(ser), _rx(serial_rx), _tx(serial_tx), _default_baud(default_baud) {
    setUbxStandby(true);
  }

  void begin() override {
    if (!board.gpsPowerIsOn()) railUp();
    WadaNmeaLocationProvider::begin();
  }

  void loop() override {
    if (!board.gpsPowerIsOn()) railUp();   // begin() found the expander busy: try again
    WadaNmeaLocationProvider::loop();
  }

  bool isEnabled() override { return board.gpsPowerIsOn(); }

  // Rail off. Idempotent and cheap: the UI calls it on every pass while GPS is off. A busy
  // or missing expander leaves everything as it was.
  void powerDown() {
    if (!board.gpsPowerIsOn() || !board.setGpsPower(false)) return;
    _uart.end();
    pinMode(_tx, INPUT_PULLDOWN);
    pinMode(_rx, INPUT_PULLDOWN);
  }

private:
  void railUp() {
    if (!board.setGpsPower(true)) return;
    _uart.setPins(_rx, _tx);   // the same order and baud as the core's initBasicGPS()
    _uart.begin(touchPrefsGetGpsBaud(_default_baud));
  }
};
