#pragma once

#include <helpers/AutoDiscoverRTCClock.h>
#include <sys/time.h>

// Monotonic clock floor for outgoing protocol timestamps (issue #89 follow-up).
//
// MeshCore servers keep a per-client "newest timestamp seen" high-water mark and
// silently drop anything at-or-below it (replay guard) — and since beta_28 our
// room keep-alives refresh that mark every ~2 minutes. A device clock that steps
// BACKWARD (reboot without GPS/phone around, or a sync correcting a fast clock)
// therefore black-holes every login/post/keep-alive with zero feedback. On a
// board with no working battery-backed clock (a T-Deck, or any board whose chip
// came back with its integrity bit latched — see HardwareRtcClock.h) a power
// cycle without a time source is exactly such a step.
//
// getCurrentTime() ratchets: it never returns less than the highest value it has
// ever returned — and getCurrentTimeUnique() (which stamps every login, post and
// keep-alive in the core) builds directly on it. The ratchet is seeded from a
// persisted copy at boot and written back rate-capped (see the UITask wiring +
// touchPrefsGet/SetClockFloor), so it survives power loss.
//
// Two escape hatches keep a WRONG-future clock from sticking forever:
  //   • setCurrentTime() rejects anything at or below MIN_VALID_EPOCH, whatever the
//     source — this centrally kills the 1902/1970-class garbage sets (GPS date
//     bugs, unset system clocks) for every path: GPS NMEA, phone CMD, NTP.
//   • a set that lands more than TRUSTED_BACK_CAP below the floor pulls the
//     floor down to it: a >10 min backward correction means the floor itself was
//     built on a bad clock, and freezing time for hours to bridge it would be
//     worse than the one-time server re-lock it avoids (which the login skew
//     warning surfaces to the user anyway).
class ClockFloorRTC : public AutoDiscoverRTCClock {
public:
  // How good the current time actually is (issue #383). RAM-only — it describes
  // THIS boot, not anything persisted. Treating "above MIN_VALID_EPOCH" as
  // "current" is what let a device with a frozen clock look authoritative; this
  // makes the difference explicit, both for the user (diagnostics) and for the
  // cold-boot network sync, which only has a reason to run below HardwareRTC.
  //
  // Ordered worst-to-best: comparisons like `< ClockSource::HardwareRTC` are
  // the intended way to ask "is my wall time actually current?".
  enum class ClockSource : uint8_t {
    Seed = 0,       // compile-time ESP32 fallback — no idea what time it is
    Floor,          // persisted monotonic floor: safe for replay ordering, stale as wall time
    HardwareRTC,    // battery-backed chip that passed integrity + plausibility checks
    ExternalSync,   // NTP / GPS / companion / CLI / accepted peer time, this boot
  };

private:
  uint32_t    _floor = 0;
  ClockSource _source = ClockSource::Seed;
  bool        _hw_present = false;

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
    if (time <= MIN_VALID_EPOCH || time > MAX_PLAUSIBLE_EPOCH) return;  // garbage set — ignore, whatever the source
    AutoDiscoverRTCClock::setCurrentTime(time);
    pushSystemClock(time);
    if (_floor > time + TRUSTED_BACK_CAP) _floor = time;
    // Every caller of this is a real time source (NTP, GPS, companion, CLI,
    // accepted mesh time) — the boot-time hardware adoption goes through the
    // board adapter's own software clock instead, precisely so it does not
    // claim to be an external sync.
    _source = ClockSource::ExternalSync;
  }

  // The UI reads the ESP32 *system* clock for every displayed time (status bar, chat
  // bubbles, logs — see the note at addMessage()), while protocol timestamps come from
  // this class. On a board with NO RTC chip they are the same clock: AutoDiscoverRTCClock
  // falls through to ESP32RTCClock, whose set IS settimeofday(), so the two can never
  // disagree. The T-LoRa Pager and the ThinkNode M9 now go through HardwareRtcClock,
  // which is also settimeofday()-backed on the read path and writes the chip on the
  // side, so the same holds there. Only the T-Display P4 still calls the core's
  // rtc_clock.begin() probe, and with a chip detected that way the base class writes
  // ONLY the chip, so the system clock keeps
  // ESP32RTCClock::begin()'s power-on seed forever. That seed is
  // exactly MIN_VALID_EPOCH, which is why the P4 showed 15 May 2024 in the UI while sent
  // messages carried the correct time — and why "Sync clock from system" then poisoned
  // the good clock: it fed that seed back in as a real set, clearing the MIN check by
  // being precisely equal to it.
  //
  // So mirror every ACCEPTED value into the system clock. The validation above has
  // already run, so 1902/2043-class garbage never reaches the display either.
  static void pushSystemClock(uint32_t t) {
    struct timeval tv;
    tv.tv_sec  = (time_t)t;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
  }

  // Boot seed. An RTC chip is battery-backed, so it already knows the time while the
  // system clock is still on the power-on seed — pull it across once so the UI reads
  // correctly with no network and no GPS fix. Goes through getCurrentTime(), so the
  // garbage-future hatch and the persisted floor both apply. No-op on the chipless
  // boards (it reads the system clock and writes the same value back).
  void seedSystemClock() {
    const uint32_t before = _floor;
    const uint32_t t = getCurrentTime();
    if (t < MIN_VALID_EPOCH || t > MAX_PLAUSIBLE_EPOCH) return;
    pushSystemClock(t);
    if (_source >= ClockSource::HardwareRTC) return;   // a real sync already landed
    // A battery-backed chip is the only thing that can be AHEAD of the floor at
    // boot without a network or a GPS fix, so that is what distinguishes real
    // retention from replaying the persisted high-water mark.
        _source = (t <= MIN_VALID_EPOCH)       ? ClockSource::Seed
          : (_hw_present && t > before)  ? ClockSource::HardwareRTC
                 : ClockSource::Floor;
  }

  // Declared by the board when its RTC adapter actually ADOPTED a validated
  // hardware time at boot. Called from radio_init(), long before the floor is
  // restored or the UI starts, so anything that asks "do I know what time it
  // is?" early in boot — the cold-boot network sync above all — gets the right
  // answer instead of waiting for seedSystemClock().
  void noteHardwareTime() {
    if (_source < ClockSource::HardwareRTC) _source = ClockSource::HardwareRTC;
  }

  // Declared by the board after its RTC adapter probed (variants/*/target.cpp).
  // Nothing infers this from "the generic probe happened to find something":
  // that guess is exactly what made a chipless T-Deck look RTC-backed.
  void noteHardwareClock(bool present) { _hw_present = present; }
  bool hardwareClockPresent() const { return _hw_present; }

  ClockSource source() const { return _source; }
  const char* sourceName() const {
    switch (_source) {
      case ClockSource::Seed:         return "seed";
      case ClockSource::Floor:        return "floor";
      case ClockSource::HardwareRTC:  return "hardware RTC";
      case ClockSource::ExternalSync: return "external sync";
    }
    return "?";
  }
  // "Do I actually know what time it is right now?" — the question the cold-boot
  // network sync and the UI's stale-clock treatment both need answered.
  bool timeIsCurrent() const { return _source >= ClockSource::HardwareRTC; }

  // Boot-time seed from the persisted floor (only ever raises), and the getter
  // the persister reads back. Benign u32 races: both cores may touch _floor.
  // A poisoned persisted value (saved while the hw clock asserted garbage) is refused.
  void seedFloor(uint32_t persisted) {
    if (persisted > MAX_PLAUSIBLE_EPOCH) return;
    if (persisted > _floor) _floor = persisted;
  }
  uint32_t getFloor() const { return _floor; }
};
