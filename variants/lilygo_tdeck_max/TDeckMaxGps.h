// SPDX-License-Identifier: GPL-3.0-or-later
//
// T-Deck Max location provider: the wadamesh NMEA provider with the GPS
// power rail routed through the XL9555 expander instead of a GPIO.
//
// On the Pro the provider owns the rail directly (PIN_GPS_EN = GPIO 39): off
// in the constructor, on in begin(), off in stop(). The Max has the same rail
// on XL9555 line 2, which the provider's GPIO code cannot drive, so this
// subclass hands the base provider -1 for both enable and reset (it then
// touches no GPIO) and switches the expander line itself around the base
// calls. begin()/stop()/isEnabled() are virtual on the core LocationProvider,
// and EnvironmentSensorManager only ever calls them through that interface,
// so GPS on/off in the UI powers the module on and off exactly as on the Pro.
#pragma once

#include "../../src/helpers/WadaNmeaLocationProvider.h"
#include "TDeckMaxBoard.h"

extern TDeckMaxBoard board;

class TDeckMaxGps : public WadaNmeaLocationProvider {
public:
  // Same argument order as the Pro's target.cpp uses for the base class:
  // serial_rx is the pin the ESP32 receives on (the module's TX).
  TDeckMaxGps(HardwareSerial& ser, mesh::RTCClock* clock,
                 int serial_rx, int serial_tx, uint32_t default_baud)
      : WadaNmeaLocationProvider(ser, clock, -1 /*reset*/, -1 /*enable*/,
                                 serial_rx, serial_tx, default_baud) {}

  void begin() override {
    board.gpsPowerOn();
    WadaNmeaLocationProvider::begin();
  }

  void stop() override {
    WadaNmeaLocationProvider::stop();
    board.gpsPowerOff();
  }

  bool isEnabled() override { return board.gpsPowerIsOn(); }
};
