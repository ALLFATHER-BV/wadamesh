// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint32_t g_now = 1000;
static uint32_t g_rand = 123456;
static uint32_t millis() { return g_now; }
static uint32_t esp_random() { return g_rand; }

#include "helpers/esp32/WebAuthCode.h"

static bool tryCode(WebAuthCode& a, const char* text) {
  return a.matches(reinterpret_cast<const uint8_t*>(text), strlen(text));
}

int main() {
  WebAuthCode a;
  assert(!tryCode(a, "000000"));  // nothing armed yet
  assert(a.code() == 0);

  a.ensure();
  assert(a.code() == 100000u + 123456u % 900000u);
  assert(tryCode(a, "223456"));
  assert(!tryCode(a, "223457"));
  assert(!tryCode(a, "22345"));
  assert(!tryCode(a, "2234567"));
  assert(!tryCode(a, "22345a"));

  g_rand = 777777;  // a second feature turning on must not rotate the code
  a.ensure();
  assert(a.code() == 223456u);

  assert(a.allowed());
  a.noteFailure();
  a.noteFailure();
  assert(a.allowed());
  a.noteFailure();  // third wrong try locks for 30 s
  assert(!a.allowed());
  g_now += 29999;
  assert(!a.allowed());
  g_now += 1;
  assert(a.allowed());

  a.noteFailure();
  a.noteFailure();
  a.noteSuccess();  // success clears the failure count
  a.noteFailure();
  a.noteFailure();
  assert(a.allowed());

  a.disarm();
  assert(a.code() == 0);
  assert(!tryCode(a, "223456"));
  a.ensure();  // next session gets a fresh code and no leftover lockout
  assert(a.code() == 100000u + 777777u % 900000u);
  assert(a.allowed());

  // Fixed code (#837)
  WebAuthCode f;
  f.setFixed(42);  // while disarmed: nothing armed yet
  assert(f.code() == 0);
  f.ensure();
  assert(f.code() == 42u);
  assert(tryCode(f, "000042"));
  f.disarm();
  g_rand = 555;
  f.ensure();  // every session gets the same code
  assert(f.code() == 42u);

  // Weak codes are refused (back to random): repeats, runs, pairs, triples, 000000
  const uint32_t weak[] = {0, 111111, 999999, 123456, 234567, 890123, 654321, 987654,
                           210987, 121212, 909090, 123123, 457457};
  for (uint32_t w : weak) assert(WebAuthCode::weakFixed(w));
  const uint32_t ok[] = {42, 482915, 100200, 120120 + 1, 314159, 271828};
  for (uint32_t o : ok) assert(!WebAuthCode::weakFixed(o));
  assert(WebAuthCode::weakFixed(1000000));
  {
    WebAuthCode w; g_rand = 1;
    w.setFixed(123456);
    assert(w.fixedCode() == 0);
    w.ensure();
    assert(w.code() == 100000u + 1u);
  }

  // With a fixed code each lockout doubles, capped at an hour, and survives re-arming
  uint32_t expect = WebAuthCode::kLockMs;
  for (int round = 0; round < 10; ++round) {
    f.noteFailure(); f.noteFailure();
    assert(f.allowed());
    f.noteFailure();
    assert(!f.allowed());
    assert(f.retryAfterSecs() == expect / 1000u + 1u || f.retryAfterSecs() == expect / 1000u);
    if (round == 3) { f.disarm(); f.ensure(); assert(f.code() == 42u && !f.allowed()); }
    g_now += expect - 1;
    assert(!f.allowed());
    g_now += 1;
    assert(f.allowed() && f.retryAfterSecs() == 0);
    expect = expect * 2u > WebAuthCode::kMaxLockMs ? WebAuthCode::kMaxLockMs : expect * 2u;
  }
  assert(expect == WebAuthCode::kMaxLockMs);
  f.noteSuccess();  // the right code resets the ladder
  f.noteFailure(); f.noteFailure(); f.noteFailure();
  g_now += WebAuthCode::kLockMs;
  assert(f.allowed());

  // A random code keeps the flat 30 s lockout
  {
    WebAuthCode r; g_rand = 7; r.ensure();
    for (int round = 0; round < 4; ++round) {
      r.noteFailure(); r.noteFailure(); r.noteFailure();
      assert(!r.allowed());
      g_now += WebAuthCode::kLockMs;
      assert(r.allowed());
    }
  }

  f.noteFailure(); f.noteFailure(); f.noteFailure();
  assert(!f.allowed());
  f.setFixed(482915);  // a NEW code swaps the live code and starts a clean lockout
  assert(f.code() == 482915u);
  assert(f.allowed());
  assert(!tryCode(f, "000042"));
  assert(tryCode(f, "482915"));

  g_rand = 555;
  f.setFixed(0);  // back to random
  assert(f.code() == 100000u + 555u % 900000u);
  f.setFixed(1000000);  // out of range = random
  assert(f.fixedCode() == 0);
  return 0;
}
