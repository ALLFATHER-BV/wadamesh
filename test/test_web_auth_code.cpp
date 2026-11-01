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
  return 0;
}
