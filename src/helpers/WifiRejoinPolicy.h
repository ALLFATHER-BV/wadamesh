#pragma once
// WifiRejoinPolicy: when the main loop's Wi-Fi state machine should step in and rejoin the saved
// network (#685). Pure logic, no Arduino or IDF headers, so it is unit-tested on the host
// (test/test_wifi_rejoin_policy.cpp) exactly as the boards run it.
//
// What it replaces: a fixed "every 10 s while not connected" retry whose timer only moved when it
// fired. After a long connected stretch that timer was ancient, so the very loop pass that saw the
// link drop ran a disconnect + begin, on top of the reconnect the Wi-Fi driver (Arduino's auto
// reconnect on the S3, the C6's own AT+CWRECONNCFG on the T-Display P4) had just started for the
// same loss. Two owners racing one association is what leaves the station "disconnected" until a
// reboot. The rules now:
//   - a link that drops gets a grace window (LOSS_GRACE_MS) for the driver's own reconnect first;
//   - while a scan, a join already in flight, or a C6 reset owns the radio, the window restarts:
//     a rejoin then would only abort that work;
//   - a rejoin is a plain disconnect + begin on the running driver, never an erase of the stored
//     network (the caller's side of this; the policy only decides WHEN);
//   - attempts that keep failing back off from RETRY_FAST_MS to RETRY_SLOW_MS, so an AP that is
//     gone overnight is not re-searched every 10 s;
//   - waking the screen asks for one prompt attempt (after WAKE_MIN_GAP_MS since the last one),
//     so a device picked up after sleeping is not waiting out a long back-off before it joins.
#include <stdint.h>

class WifiRejoinPolicy {
public:
  static constexpr uint32_t LOSS_GRACE_MS    = 10000;  // the driver's own reconnect goes first
  static constexpr uint32_t RETRY_FAST_MS    = 10000;  // the first attempts, as before
  static constexpr uint32_t RETRY_SLOW_MS    = 30000;  // after FAST_ATTEMPTS failed in a row
  static constexpr uint8_t  FAST_ATTEMPTS    = 6;      // about a minute of fast tries
  static constexpr uint32_t WAKE_MIN_GAP_MS  = 3000;   // a wake never cuts a fresh attempt short

  struct Inputs {
    bool want;          // the user has Wi-Fi switched on
    bool has_creds;     // a saved network to join
    bool connected;     // the station has its link (and address)
    bool radio_busy;    // a scan, a join in flight, or a C6 reset owns the radio right now
  };

  // A join the caller started outside the policy (the first begin() after switching on, or a
  // credential/radio apply). The retry window counts from it, at the fast pace again.
  void noteAttempt(uint32_t now_ms) {
    _last_ms = now_ms;
    _armed = true;
    _gap_ms = RETRY_FAST_MS;
    _failures = 0;
    _was_connected = false;   // a fresh begin, not a link that dropped
  }

  // The screen came back on.
  void noteWake() { _wake = true; }

  // Called once per loop pass. True = run one rejoin (disconnect without erase, then begin) now.
  bool shouldRejoin(const Inputs& in, uint32_t now_ms) {
    if (!in.want || !in.has_creds) {     // nothing to join: forget the history
      _was_connected = false;
      _failures = 0;
      _wake = false;
      _armed = false;
      return false;
    }
    if (in.connected) {
      _was_connected = true;
      _failures = 0;
      _wake = false;
      return false;
    }
    if (_was_connected) {                // the link just dropped: the driver's reconnect goes first
      _was_connected = false;
      _failures = 0;
      _last_ms = now_ms;
      _armed = true;
      _gap_ms = LOSS_GRACE_MS;
      return false;
    }
    if (!_armed) {                       // switched on with no attempt seen yet: start counting now
      _last_ms = now_ms;
      _armed = true;
      _gap_ms = RETRY_FAST_MS;
      return false;
    }
    if (in.radio_busy) {                 // a scan / join / reset owns the radio: wait it out
      _last_ms = now_ms;
      return false;
    }
    const uint32_t since = now_ms - _last_ms;   // wrap-safe
    const bool wake_due = _wake && since >= WAKE_MIN_GAP_MS;
    if (!wake_due && since < _gap_ms) return false;
    _wake = false;
    _last_ms = now_ms;
    if (_failures < 255) _failures++;
    _gap_ms = (_failures >= FAST_ATTEMPTS) ? RETRY_SLOW_MS : RETRY_FAST_MS;
    return true;
  }

  uint8_t failures() const { return _failures; }

private:
  uint32_t _last_ms = 0;
  uint32_t _gap_ms = RETRY_FAST_MS;
  uint8_t  _failures = 0;
  bool     _armed = false;
  bool     _was_connected = false;
  bool     _wake = false;
};
