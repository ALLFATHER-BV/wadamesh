// SPDX-License-Identifier: GPL-3.0-or-later
//
// ThinkNode M9 keyboard probe: the controller is at 0x6C on the original board and at
// 0x6D on the newer revision. This builds the real driver (variants/thinknode_m9/
// M9Keyboard.cpp) against simulated I2C buses and checks that it finds the controller at
// either address, on either bus, and then reads keys and writes the backlight at the
// address it found.
//
// The driver keeps its state in file-scope statics, so every scenario runs in its own
// forked process; a failed assert aborts the child and fails the test.
//
// Build: g++ -std=c++17 -I src -I test/m9_keyboard_probe_fakes -o /tmp/t test/test_m9_keyboard_probe.cpp

#include <assert.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define HAS_M9_KEYBOARD 1
#define ESP32 1
#define PIN_KB_SDA 20
#define PIN_KB_SCL 21

#include "Arduino.h"
#include "Wire.h"
#include "../variants/thinknode_m9/M9Keyboard.cpp"

// Poll once, stepping the clock past both the 15 ms key-poll limit and the 1 s re-probe limit.
static void pollOnce() { g_fake_ms += 2000; m9KeyboardPoll(); }

static void controllerAt(TwoWire& bus, uint8_t addr, uint8_t key = 0) {
  bus.dev_addr = addr;
  if (key) { bus.keys[0] = key; bus.nkeys = 1; }
}

static void expectFoundAt(TwoWire& bus, uint8_t addr, const char* name) {
  m9KeyboardBegin();
  assert(Wire1.begun == 1);                       // bus 20/21 was brought up
  // Boot log names the address and the bus it was found on.
  char want[64];
  snprintf(want, sizeof want, "controller on %s addr 0x%02X", name, addr);
  assert(strstr(Serial.log, want) != nullptr);
  // A key reaches the ring, so reads are addressed to the found address.
  pollOnce();
  assert(m9KeyboardReadKey() == M9_KEY_ENTER);
  // The backlight write goes to the found address too.
  assert(m9KeyboardSetBacklight(77));
  assert(bus.backlight_addr == addr && bus.backlight_duty == 77);
}

static void legacyAddressOnOwnBus()   { controllerAt(Wire1, 0x6C, M9_KEY_ENTER); expectFoundAt(Wire1, 0x6C, "Wire1 (20/21)"); }
static void newAddressOnOwnBus()      { controllerAt(Wire1, 0x6D, M9_KEY_ENTER); expectFoundAt(Wire1, 0x6D, "Wire1 (20/21)"); }
static void newAddressOnFallbackBus() { controllerAt(Wire,  0x6D, M9_KEY_ENTER); expectFoundAt(Wire,  0x6D, "Wire (7/6)"); }
static void legacyAddressOnFallback() { controllerAt(Wire,  0x6C, M9_KEY_ENTER); expectFoundAt(Wire,  0x6C, "Wire (7/6)"); }

// Controller still booting at m9KeyboardBegin(): nothing found, backlight refused, then the
// 1 s re-probe finds it at 0x6D and everything works from then on.
static void lateStartAtNewAddress() {
  m9KeyboardBegin();
  assert(strstr(Serial.log, "no controller yet") != nullptr);
  assert(!m9KeyboardSetBacklight(10));            // not found: nothing written
  controllerAt(Wire1, 0x6D, M9_KEY_ENTER);
  pollOnce();                                     // re-probe finds it
  assert(strstr(Serial.log, "controller on Wire1 (20/21) addr 0x6D") != nullptr);
  pollOnce();
  assert(m9KeyboardReadKey() == M9_KEY_ENTER);
  assert(m9KeyboardSetBacklight(10) && Wire1.backlight_addr == 0x6D);
}

// Nothing at either address: no keys, no backlight write, and no crash.
static void noController() {
  m9KeyboardBegin();
  pollOnce(); pollOnce();
  assert(m9KeyboardReadKey() == 0);
  assert(!m9KeyboardSetBacklight(10));
}

// An address that is not one of the two controller addresses is never claimed.
static void foreignAddressIgnored() {
  controllerAt(Wire1, 0x55, M9_KEY_ENTER);
  m9KeyboardBegin();
  pollOnce();
  assert(m9KeyboardReadKey() == 0);
  assert(!m9KeyboardSetBacklight(10));
}

// Found at 0x6D, then the bus starts NACKing: the key read fails quietly (no stale key) and a
// backlight write reports failure instead of pretending the duty was taken.
static void readFailureAfterFound() {
  controllerAt(Wire1, 0x6D, M9_KEY_ENTER);
  m9KeyboardBegin();
  Wire1.nack_reads = true;
  pollOnce();
  assert(m9KeyboardReadKey() == 0);
  assert(!m9KeyboardSetBacklight(99));
}

static void run(const char* name, void (*fn)()) {
  pid_t pid = fork();
  assert(pid >= 0);
  if (pid == 0) { fn(); _exit(0); }
  int st = 0;
  waitpid(pid, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
    printf("FAILED scenario: %s\n", name);
    _exit(1);
  }
}

int main() {
  run("legacy 0x6C on Wire1", legacyAddressOnOwnBus);
  run("new 0x6D on Wire1", newAddressOnOwnBus);
  run("new 0x6D on Wire fallback", newAddressOnFallbackBus);
  run("legacy 0x6C on Wire fallback", legacyAddressOnFallback);
  run("late start at 0x6D", lateStartAtNewAddress);
  run("no controller", noController);
  run("foreign address ignored", foreignAddressIgnored);
  run("read failure after found", readFailureAfterFound);
  return 0;
}
