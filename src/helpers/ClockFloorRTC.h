#pragma once

#include <helpers/AutoDiscoverRTCClock.h>

// Monotonic clock floor for outgoing protocol timestamps (issue #89 follow-up).
//
// MeshCore servers keep a per-client "newest timestamp seen" high-water mark and
// silently drop anything at-or-below it (replay guard) — and since beta_28 our
// room keep-alives refresh that mark every ~2 minutes. A device clock that steps
// BACKWARD (reboot without GPS/phone around, or a sync correcting a fast clock)
// therefore black-holes every login/post/keep-alive with zero feedback. None of
// our boards has a battery-backed RTC chip, so a power cycle without a time
// source is exactly such a step.
//
// getCurrentTime() ratchets: it never returns less than the highest value it has
// ever returned — and getCurrentTimeUnique() (which stamps every login, post and
// keep-alive in the core) builds directly on it. The ratchet is seeded from a
// persisted copy at boot and written back rate-capped (see the UITask wiring +
// touchPrefsGet/SetClockFloor), so it survives power loss.
//
// Two escape hatches keep a WRONG-future clock from sticking forever:
//   • setCurrentTime() rejects anything below MIN_VALID_EPOCH, whatever the
//     source — this centrally kills the 1902/1970-class garbage sets (GPS date
//     bugs, unset system clocks) for every path: GPS NMEA, phone CMD, NTP.
//   • a set that lands more than TRUSTED_BACK_CAP below the floor pulls the
//     floor down to it: a >10 min backward correction means the floor itself was
//     built on a bad clock, and freezing time for hours to bridge it would be
//     worse than the one-time server re-lock it avoids (which the login skew
//     warning surfaces to the user anyway).
class ClockFloorRTC : public AutoDiscoverRTCClock {
  uint32_t _floor = 0;
public:
  ClockFloorRTC(mesh::RTCClock& fallback) : AutoDiscoverRTCClock(fallback) {}

  static constexpr uint32_t MIN_VALID_EPOCH  = 1715770351UL;  // 15 May 2024 — the core's own unset-clock seed
  static constexpr uint32_t TRUSTED_BACK_CAP = 10UL * 60UL;

  // High-side twin of MIN_VALID_EPOCH: an external RTC chip with corrupted/unset date
  // registers can read as a *future* date (seen live: a T-Display P4's PCF8563 came back
  // from a reboot asserting 2043). Future garbage is worse than past garbage — it passes
  // the MIN check, latches the ratchet, and every timestamp sticks years ahead until a
  // >10 min backward set arrives. Anything past ~(build year + 3) cannot be a real clock.
  static constexpr uint32_t BUILD_YEAR =
      (uint32_t)(__DATE__[7]-'0')*1000u + (uint32_t)(__DATE__[8]-'0')*100u +
      (uint32_t)(__DATE__[9]-'0')*10u   + (uint32_t)(__DATE__[10]-'0');
  static constexpr uint32_t MAX_PLAUSIBLE_EPOCH = (BUILD_YEAR + 3u - 1970u) * 31557600u;

  uint32_t getCurrentTime() override {
    uint32_t t = AutoDiscoverRTCClock::getCurrentTime();
    if (t > MAX_PLAUSIBLE_EPOCH) {
      // Hardware clock is asserting garbage-future: ignore the read entirely and hold at
      // the last trusted point (or the core's unset seed) until a real source sets us.
      return (_floor >= MIN_VALID_EPOCH) ? _floor : MIN_VALID_EPOCH;
    }
    if (t < _floor) return _floor;
    _floor = t;
    return t;
  }

  void setCurrentTime(uint32_t time) override {
    if (time < MIN_VALID_EPOCH || time > MAX_PLAUSIBLE_EPOCH) return;   // garbage set — ignore, whatever the source
    AutoDiscoverRTCClock::setCurrentTime(time);
    if (_floor > time + TRUSTED_BACK_CAP) _floor = time;
  }

  // Boot-time seed from the persisted floor (only ever raises), and the getter
  // the persister reads back. Benign u32 races: both cores may touch _floor.
  // A poisoned persisted value (saved while the hw clock asserted garbage) is refused.
  void seedFloor(uint32_t persisted) {
    if (persisted > MAX_PLAUSIBLE_EPOCH) return;
    if (persisted > _floor) _floor = persisted;
  }
  uint32_t getFloor() const { return _floor; }
};
