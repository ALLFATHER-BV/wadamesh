// Host test for the main loop's Wi-Fi rejoin decision (#685): src/helpers/WifiRejoinPolicy.h.
// Build + run from the repo root (scripts/run-host-tests.sh does this for every test):
//   c++ -std=c++17 -Wall -Wextra -I src test/test_wifi_rejoin_policy.cpp -o /tmp/t && /tmp/t
//
// The loop is simulated pass by pass with a fake clock: link loss, the driver's own reconnect,
// a scan or a join owning the radio, the screen sleeping overnight on a throttled loop (the
// battery saver parks it 50 ms per pass), the wake, and the millis() wrap.
#include <assert.h>
#include <stdio.h>
#include <vector>

#include "helpers/WifiRejoinPolicy.h"

using P = WifiRejoinPolicy;
static const uint32_t S = 1000;

// Run the loop from t0 to t1 in steps of `step` ms with fixed inputs; return the times a
// rejoin was asked for.
static std::vector<uint32_t> run(P& p, P::Inputs in, uint32_t t0, uint32_t t1, uint32_t step) {
  std::vector<uint32_t> at;
  for (uint32_t t = t0; (int32_t)(t1 - t) > 0; t += step)
    if (p.shouldRejoin(in, t)) at.push_back(t);
  return at;
}

// The retry this replaces: every 10 s while not connected, timer moved only when it fired.
struct OldRetry {
  uint32_t last = 0;
  bool tick(bool want, bool creds, bool connected, bool scan, uint32_t now) {
    if (!want || !creds || connected || scan) return false;
    if (now - last >= 10000) { last = now; return true; }
    return false;
  }
};

int main() {
  const P::Inputs down  = { true, true, false, false };
  const P::Inputs up    = { true, true, true,  false };
  const P::Inputs busy  = { true, true, false, true  };
  const P::Inputs off   = { false, true, false, false };

  // ---- 1. switched on, the first begin never takes: 10 s apart six times, then 30 s ----
  {
    P p;
    p.noteAttempt(0);
    auto at = run(p, down, 0, 200 * S, 10);
    const uint32_t want[] = { 10, 20, 30, 40, 50, 60, 90, 120, 150, 180 };
    assert(at.size() == sizeof(want) / sizeof(want[0]));
    for (size_t i = 0; i < at.size(); i++) assert(at[i] == want[i] * S);
  }

  // ---- 2. a long connected stretch, then the link drops and the driver rejoins by itself ----
  // The old retry fired on the very pass that saw the drop, on top of the driver's reconnect.
  {
    P p; OldRetry old;
    p.noteAttempt(0);
    assert(run(p, up, 0, 7200 * S, 50).empty());
    for (uint32_t t = 0; t < 7200 * S; t += 50) old.tick(true, true, true, false, t);
    const uint32_t T = 7200 * S;
    assert(old.tick(true, true, false, false, T));        // old: immediate disconnect + begin
    assert(!p.shouldRejoin(down, T));                     // new: the driver goes first
    assert(run(p, down, T, T + 3 * S, 50).empty());       // 3 s to reassociate on its own
    assert(run(p, up, T + 3 * S, T + 60 * S, 50).empty());
  }

  // ---- 3. the link drops and the driver does not get it back: first rejoin after the grace ----
  {
    P p;
    p.noteAttempt(0);
    run(p, up, 0, 100 * S, 50);
    auto at = run(p, down, 100 * S, 125 * S, 50);
    assert(at.size() == 2 && at[0] == 110 * S && at[1] == 120 * S);
  }

  // ---- 4. a scan owns the radio: nothing while it runs, the window restarts when it ends ----
  {
    P p;
    p.noteAttempt(0);
    assert(run(p, down, 0, 8 * S, 50).empty());
    assert(run(p, busy, 8 * S, 20 * S, 50).empty());     // a scan from 8 s to 20 s
    auto at = run(p, down, 20 * S, 31 * S, 50);
    assert(at.size() == 1 && at[0] == 20 * S - 50 + 10 * S);   // 10 s after the scan's last pass, not mid-scan
  }

  // ---- 5. a join already in flight (C6 CWJAP, up to 15 s) or a C6 reset: no second join ----
  {
    P p;
    p.noteAttempt(0);
    assert(run(p, busy, 0, 15 * S, 150).empty());
    auto at = run(p, down, 15 * S, 26 * S, 150);
    assert(at.size() == 1 && at[0] - (15 * S - 150) >= 10 * S && at[0] - (15 * S - 150) < 10 * S + 150);   // 10 s after the join's last busy pass
  }

  // ---- 6. overnight: AP gone, screen off, battery-saver loop at 50 ms; then the wake ----
  {
    P p; OldRetry old;
    p.noteAttempt(0);
    run(p, up, 0, 600 * S, 50);
    const uint32_t lost = 600 * S, wake = lost + 8 * 3600 * S + 7 * S;
    auto at = run(p, down, lost, wake, 50);
    size_t old_n = 0;
    for (uint32_t t = lost; t < wake; t += 50) old_n += old.tick(true, true, false, false, t);
    printf("overnight 8 h: %zu rejoins (the old fixed retry: %zu)\n", at.size(), old_n);
    assert(at.size() < old_n / 2);
    assert(at.size() >= 900);                              // still trying all night
    for (size_t i = P::FAST_ATTEMPTS; i < at.size(); i++) assert(at[i] - at[i - 1] == P::RETRY_SLOW_MS);
    // The wake: an attempt within a pass, not up to 30 s later.
    const uint32_t since = wake - at.back();
    p.noteWake();
    bool fired = p.shouldRejoin(down, wake);
    if (since >= P::WAKE_MIN_GAP_MS) assert(fired);
    else {
      assert(!fired);
      auto w = run(p, down, wake + 50, at.back() + P::WAKE_MIN_GAP_MS + 100, 50);
      assert(w.size() == 1 && w[0] - at.back() >= P::WAKE_MIN_GAP_MS);
    }
    // The AP is back and the join takes: the back-off is forgotten.
    assert(!p.shouldRejoin(up, wake + 4 * S));
    assert(p.failures() == 0);
  }

  // ---- 7. a wake right after an attempt does not cut it short ----
  {
    P p;
    p.noteAttempt(0);
    assert(run(p, down, 0, 10 * S + 1, 50).size() == 1);  // attempt at 10 s
    p.noteWake();
    assert(!p.shouldRejoin(down, 11 * S));
    assert(p.shouldRejoin(down, 13 * S));                  // 3 s after the attempt, not 20 s
    assert(!p.shouldRejoin(down, 14 * S));                 // one wake = one attempt
  }

  // ---- 8. a wake while a scan runs waits for the scan ----
  {
    P p;
    p.noteAttempt(0);
    p.noteWake();
    assert(run(p, busy, 0, 9 * S, 50).empty());
    auto at = run(p, down, 9 * S, 13 * S, 50);
    assert(at.size() == 1 && at[0] == 9 * S - 50 + P::WAKE_MIN_GAP_MS);   // 3 s after the scan
  }

  // ---- 9. Wi-Fi switched off: never; switched on again: counts from the new begin ----
  {
    P p;
    p.noteAttempt(0);
    run(p, down, 0, 100 * S, 50);
    assert(run(p, off, 100 * S, 200 * S, 50).empty());
    p.noteAttempt(200 * S);
    auto at = run(p, down, 200 * S, 221 * S, 50);
    assert(at.size() == 2 && at[0] == 210 * S && at[1] == 220 * S);
  }

  // ---- 10. no begin seen (the driver came up some other way): starts counting itself ----
  {
    P p;
    auto at = run(p, down, 5 * S, 16 * S, 50);
    assert(at.size() == 1 && at[0] == 15 * S);
  }

  // ---- 11. millis() wraps mid-window ----
  {
    P p;
    const uint32_t t0 = 0xFFFFFFFFu - 4 * S;
    p.noteAttempt(t0);
    auto at = run(p, down, t0, t0 + 12 * S, 50);
    assert(at.size() == 1 && at[0] == t0 + 10 * S);
  }

  printf("test_wifi_rejoin_policy: all passed\n");
  return 0;
}
