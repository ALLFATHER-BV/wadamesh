// SPDX-License-Identifier: GPL-3.0-or-later
// Execute the production driver against both documented M9 I2C addresses.
#include <cassert>
#include <cstdio>
#include "m9_keyboard_fakes/Arduino.h"
#include "m9_keyboard_fakes/Wire.h"
#define HAS_M9_KEYBOARD 1
#define ESP32 1
#define PIN_KB_SDA 20
#define PIN_KB_SCL 21
uint32_t testMillis = 0;
TestSerial Serial;
TwoWire Wire, Wire1;
#include "../variants/thinknode_m9/M9Keyboard.cpp"

static void resetDriver() {
  Wire = TwoWire{};
  Wire1 = TwoWire{};
  Serial.output.clear();
  s_bus = nullptr;
  s_inited = false;
  s_head = s_tail = 0;
  // Keep time monotonic across fixtures, including the poll's local timer.
  testMillis += 2000;
}

static void checkController(uint8_t address, bool sensorBus) {
  std::fprintf(stderr, "Testing address 0x%02x on %s\n", address, sensorBus ? "Wire" : "Wire1");
  resetDriver();
  TwoWire& keyboard = sensorBus ? Wire : Wire1;
  keyboard.device = address;
  m9KeyboardBegin();
  assert(Wire1.sda == 20 && Wire1.scl == 21);
  keyboard.keys = {M9_KEY_DOWN, M9_KEY_ENTER, 'a'};
  m9KeyboardPoll();
  assert(m9KeyboardReadKey() == M9_KEY_DOWN);
  assert(m9KeyboardReadKey() == M9_KEY_ENTER);
  assert(m9KeyboardReadKey() == 'a');
  assert(m9KeyboardReadKey() == 0);
  assert(m9KeyboardSetBacklight(127));
  assert(keyboard.backlight == 127);
  keyboard.readFails = true;
  testMillis += 20;
  m9KeyboardPoll();
  assert(m9KeyboardReadKey() == 0);
  keyboard.readFails = false;
  keyboard.keys = {M9_KEY_UP};
  testMillis += 20;
  m9KeyboardPoll();
  assert(m9KeyboardReadKey() == M9_KEY_UP);
}

int main() {
  checkController(0x6c, false);
  checkController(0x6c, true);
  checkController(0x6d, false);
  checkController(0x6d, true);
  resetDriver();
  m9KeyboardBegin();
  assert(!m9KeyboardSetBacklight(255));
  m9KeyboardPoll();
  assert(m9KeyboardReadKey() == 0);
  // A controller booting late must be found without rebooting the main MCU.
  Wire1.device = 0x6d;
  Wire1.keys = {M9_KEY_ENTER};
  testMillis += 1001;
  m9KeyboardPoll();
  assert(m9KeyboardReadKey() == M9_KEY_ENTER);
  assert(m9KeyboardSetBacklight(255));
  std::puts("M9 keyboard: both revisions/buses, navigation, text, backlight, late boot and read failure PASS");
}
