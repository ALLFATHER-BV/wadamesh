// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in for the core header WadaNmeaLocationProvider.h includes: the pin macros
// (no enable/reset line, as on the T-Deck) and the LocationProvider interface.
#pragma once
#include "Arduino.h"
#include <stdio.h>

#ifndef GPS_EN
#define GPS_EN (-1)
#endif
#ifndef GPS_EN_ACTIVE
#define GPS_EN_ACTIVE HIGH
#endif
#ifndef GPS_RESET
#define GPS_RESET (-1)
#endif
#ifndef GPS_RESET_ACTIVE
#define GPS_RESET_ACTIVE LOW
#endif

namespace mesh {
struct RTCClock {
  uint32_t set_calls = 0;
  virtual ~RTCClock() {}
  virtual void setCurrentTime(uint32_t) { ++set_calls; }
};
}

class LocationProvider {
protected:
  bool _time_sync_needed = true;
public:
  virtual ~LocationProvider() {}
  virtual void syncTime() { _time_sync_needed = true; }
  virtual bool waitingTimeSync() { return _time_sync_needed; }
  virtual long getLatitude() = 0;
  virtual long getLongitude() = 0;
  virtual long getAltitude() = 0;
  virtual long satellitesCount() = 0;
  virtual bool isValid() = 0;
  virtual long getTimestamp() = 0;
  virtual void sendSentence(const char*) {}
  virtual void reset() = 0;
  virtual void begin() = 0;
  virtual void stop() = 0;
  virtual void loop() = 0;
  virtual bool isEnabled() = 0;
};
