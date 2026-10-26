// SPDX-License-Identifier: GPL-3.0-or-later
// Execute the production driver against both documented M9 I2C addresses.
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
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
  s_guard = M9KeyGuard{};
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
  // One read per poll (#625: repeat reads in one poll made keys type twice), so each
  // queued key arrives on its own poll.
  keyboard.keys = {M9_KEY_DOWN, M9_KEY_ENTER, 'a'};
  m9KeyboardPoll();
  assert(m9KeyboardReadKey() == M9_KEY_DOWN);
  assert(m9KeyboardReadKey() == 0);
  testMillis += 20;
  m9KeyboardPoll();
  assert(m9KeyboardReadKey() == M9_KEY_ENTER);
  testMillis += 20;
  m9KeyboardPoll();
  assert(m9KeyboardReadKey() == 'a');
  assert(m9KeyboardReadKey() == 0);
  // The same key again within 70 ms of the last accepted one is the same press (a stale
  // read or a bounce); after that it is a new press.
  keyboard.keys = {'b', 'b'};
  testMillis += 20;
  m9KeyboardPoll();
  assert(m9KeyboardReadKey() == 'b');
  testMillis += 20;
  m9KeyboardPoll();
  assert(m9KeyboardReadKey() == 0);
  keyboard.keys = {'b'};
  testMillis += 300;   // past the same-character window (kM9GuardDefaultMs before a pace is known)
  m9KeyboardPoll();
  assert(m9KeyboardReadKey() == 'b');
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

// ---- #671: the controller, simulated -------------------------------------------
// Elecrow's controller firmware (ThinkNode-M9-KB-platformio, src/main.cpp): a scan
// task reads the matrix every 10 ms with no debounce (readMatrix sets keys[] and a
// changed flag on any difference), and processKeys(), every 10 ms in loop(), latches
// the key byte on a changed-and-pressed key (sendKeyData: one slot, keyvalue +
// comdata_flag), the letters on the press itself. An I2C read of reg 0x01 hands
// the latched byte over once and clears it.
struct Stroke { uint8_t key; uint32_t down; uint32_t hold; uint32_t chatter; };
// The contact as the scan sees it: closed while held; `chatter` > 0 makes it open
// for one scan and close again for one scan as the key comes up (a slow lift).
static bool contactClosed(const Stroke& s, uint32_t t) {
  if (t < s.down) return false;
  const uint32_t up = s.down + s.hold;
  if (t < up) return true;
  if (s.chatter && t >= up + 10 && t < up + 10 + s.chatter) return true;
  return false;
}
struct ControllerSim {
  std::vector<Stroke> strokes;
  std::vector<bool> keys, changed;
  std::vector<uint32_t> sent_ms;   // when the controller latched each byte
  void run(TwoWire& bus, uint32_t t0, uint32_t until, std::string& typed, bool real_driver,
           bool (*old_rule)(uint8_t, uint32_t)) {
    keys.assign(strokes.size(), false);
    changed.assign(strokes.size(), false);
    for (uint32_t t = 0; t < until; ++t) {
      if (t % 10 == 0) {   // KeyboardScanTask: readMatrix()
        for (size_t i = 0; i < strokes.size(); ++i) {
          const bool p = contactClosed(strokes[i], t);
          if (p != keys[i]) changed[i] = true;
          keys[i] = p;
        }
      }
      if (t % 10 == 5) {   // loop(): processKeys()
        for (size_t i = 0; i < strokes.size(); ++i) {
          if (changed[i] && keys[i]) {   // State 1: sendKeyData(normalVal)
            bus.keys.clear();            // one slot: a newer key replaces an unread one
            bus.keys.push_back(strokes[i].key);
            sent_ms.push_back(t);
            changed[i] = false;
          }
          if (!keys[i] && changed[i]) changed[i] = false;   // State 3: letters send nothing
        }
      }
      testMillis = t0 + t;
      if (real_driver) {
        m9KeyboardPoll();
        for (int k; (k = m9KeyboardReadKey()) != 0;) typed += (char)k;
      } else if (t % 15 == 0 && !bus.keys.empty()) {   // a 15 ms poll with the old rule
        const uint8_t k = bus.keys.front();
        bus.keys.pop_front();
        if (old_rule(k, t0 + t)) typed += (char)k;
      }
    }
  }
};
// #628's rule: the same key within 70 ms of the last accepted one is the same press.
static uint8_t  s_old_last = 0;
static uint32_t s_old_ms = 0;
static bool oldRule(uint8_t k, uint32_t now) {
  if (k == s_old_last && now - s_old_ms < 70) return false;
  s_old_last = k; s_old_ms = now;
  return true;
}
// Type `text` at one keystroke every `iki` ms, each key held `hold` ms; every key
// chatters on the way up when `chatter` is set. Returns what came out.
static std::string typeText(const char* text, uint32_t iki, uint32_t hold, uint32_t chatter, bool real) {
  resetDriver();
  Wire1.device = 0x6c;
  m9KeyboardBegin();
  s_old_last = 0; s_old_ms = 0;
  ControllerSim sim;
  uint32_t t = 7;
  for (const char* p = text; *p; ++p, t += iki) sim.strokes.push_back({(uint8_t)*p, t, hold, chatter});
  std::string typed;
  sim.run(Wire1, testMillis, t + 400, typed, real, oldRule);
  return typed;
}
static std::string typeKeys(const std::vector<uint8_t>& keys, uint32_t iki, uint32_t hold, uint32_t chatter) {
  resetDriver();
  Wire1.device = 0x6c;
  m9KeyboardBegin();
  ControllerSim sim;
  uint32_t t = 7;
  for (uint8_t k : keys) { sim.strokes.push_back({k, t, hold, chatter}); t += iki; }
  std::string typed;
  sim.run(Wire1, testMillis, t + 400, typed, true, oldRule);
  return typed;
}

static void checkChatter() {
  // Reproduced: a slow, deliberate press (held 180 ms) whose contact chatters once on
  // the way up. The controller sends the letter twice, ~190 ms apart, and #628's
  // 70 ms rule lets both through: "hheelllloo".
  const std::string old_slow = typeText("hello", 450, 180, 10, false);
  std::fprintf(stderr, "old rule, slow keys with chatter: \"%s\"\n", old_slow.c_str());
  assert(old_slow == "hheelllloo");
  // The guard now: one character per press, the "ll" typed on purpose kept.
  assert(typeText("hello", 450, 180, 10, true) == "hello");
  assert(typeText("bookkeeper", 450, 200, 10, true) == "bookkeeper");
  // Very slow: a keystroke every 0.6 s, each held 220 ms.
  assert(typeText("deliberate", 600, 220, 10, true) == "deliberate");
  // The limit, kept honest: a key held longer than the 250 ms ceiling that then
  // chatters still types twice. A longer ceiling would eat letters typed twice on
  // purpose; only the controller (it sees the key go up) can debounce that.
  assert(typeText("ab", 600, 300, 10, true) == "aabb");
  // Fast typing, short holds, chatter on every key: still one each, doubles kept.
  assert(typeText("bookkeeper", 170, 70, 10, true) == "bookkeeper");
  assert(typeText("bookkeeper", 170, 70, 0, true) == "bookkeeper");
  // Moderate pace.
  assert(typeText("aardvark scissors", 300, 120, 10, true) == "aardvark scissors");
  // Arrows tapped quickly keep every tap (stepping down a list), as before.
  const std::string downs = typeKeys({M9_KEY_DOWN, M9_KEY_DOWN, M9_KEY_DOWN, M9_KEY_DOWN, M9_KEY_DOWN}, 110, 50, 0);
  assert(downs.size() == 5);
  // A held Delete: the controller repeats it every ~110 ms (REPEAT_DELAY_MS 100 plus
  // its loop); every repeat gets through.
  resetDriver();
  Wire1.device = 0x6c;
  m9KeyboardBegin();
  int dels = 0;
  for (int i = 0; i < 10; ++i) {
    Wire1.keys = {M9_KEY_DEL};
    testMillis += 110;
    m9KeyboardPoll();
    if (m9KeyboardReadKey() == M9_KEY_DEL) ++dels;
  }
  assert(dels == 10);
  // The window: 3/5 of the pace, held between 70 and 250 ms.
  M9KeyGuard g;
  assert(m9KeyGuardWindow(g) == kM9GuardDefaultMs);
  g.pace_ms = 100; assert(m9KeyGuardWindow(g) == 70);
  g.pace_ms = 300; assert(m9KeyGuardWindow(g) == 180);
  g.pace_ms = 900; assert(m9KeyGuardWindow(g) == 250);
  // A pause does not count as the pace.
  M9KeyGuard p;
  assert(m9KeyGuardAccept(p, 'a', 1000));
  assert(m9KeyGuardAccept(p, 'b', 5000));
  assert(p.pace_ms == 0);
  assert(m9KeyGuardAccept(p, 'c', 5300));
  assert(p.pace_ms == 300);
}

int main() {
  checkChatter();
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
