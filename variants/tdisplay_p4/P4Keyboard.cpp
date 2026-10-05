// SPDX-License-Identifier: GPL-3.0-or-later
#include "P4Keyboard.h"

#if defined(HAS_TDISPLAY_P4_KEYBOARD)

#include <Arduino.h>
#include <Wire.h>
#include "P4I2c1.h"

// Pins and addresses: LilyGo t_display_p4_config.h, as camillia-mt uses them.
#define P4KB_TCA_ADDR       0x34
#define P4KB_EXP_ADDR       0x20   // the expansion's own XL9555
#define P4KB_EXP_RESET_BIT  6      // port 0: TCA8418 reset, active low
#define P4KB_EXP_LED_MASK   0x38   // port 0 bits 3..5: indicator LEDs, lit low
#define P4KB_INT_PIN        48     // TCA8418 INT, active low
#define P4KB_BL_PIN         47
#define P4KB_BL_HZ          20000

// XL9555 registers (port 0).
#define XL_OUT0  0x02
#define XL_CFG0  0x06

// TCA8418 registers.
#define TCA_INT_STAT         0x02
#define TCA_KEY_LCK_EC       0x03
#define TCA_KEY_EVENT_A      0x04
#define TCA_GPIO_INT_EN_1    0x1A
#define TCA_KP_GPIO_1        0x1D
#define TCA_GPI_EM_1         0x20
#define TCA_GPIO_DIR_1       0x23
#define TCA_GPIO_INT_LVL_1   0x26
#define TCA_DEBOUNCE_DIS_1   0x29

namespace {

constexpr uint32_t kHotplugMs      = 1500;   // how often the expansion is looked for
static bool s_scanned = false;   // one-shot bus scan (see p4KeyboardPoll)
constexpr uint32_t kHotplugIdleMs  = 15000;  // probe rate once the expansion looks absent
constexpr uint8_t  kMissesBeforeBackoff = 4;
static uint8_t     s_misses = 0;             // consecutive probes that found nothing
constexpr uint32_t kIdlePollMs     = 120;    // backstop drain when INT says nothing is waiting
constexpr uint32_t kModTimeoutMs   = 1500;   // a one-shot modifier lapses after this
constexpr uint32_t kRepeatDelayMs  = 450;
constexpr uint32_t kRepeatEveryMs  = 70;
constexpr uint32_t kBusWaitMs      = 5;

// Key numbers are the TCA8418's, 1-based: row * 10 + column + 1.
constexpr uint8_t kKeyCaps   = 31;
constexpr uint8_t kKeyAlt    = 41;
constexpr uint8_t kKeySym1   = 51;
constexpr uint8_t kKeyShift  = 53;
constexpr uint8_t kKeySym2   = 58;
constexpr uint8_t kKeyBksp   = 63;
constexpr uint8_t kKeyLight  = 61;   // F11
constexpr uint8_t kKeyCount  = 68;

constexpr uint8_t kModShift = 0x01;
constexpr uint8_t kModSym   = 0x02;
constexpr uint8_t kModAlt   = 0x04;

constexpr int K_ESC = 0x1B, K_TAB = 0x09, K_BS = 0x08, K_ENT = 0x0D;
constexpr int FKEY(int n) { return P4KB_F1 + n - 1; }

// base, shift, symbol. 0 in shift/symbol falls back to base; 0 in base is a
// modifier (handled by number in translate()) or a key with no use yet.
struct KeyDef { int16_t base, shift, sym; };
const KeyDef kMap[kKeyCount] = {
  {FKEY(1), 0, 0}, {FKEY(2), 0, 0}, {FKEY(3), 0, 0}, {FKEY(4), 0, 0}, {FKEY(5), 0, 0},   //  1-5
  {FKEY(6), 0, 0}, {FKEY(7), 0, 0}, {FKEY(8), 0, 0}, {FKEY(9), 0, 0}, {FKEY(10), 0, 0},  //  6-10
  {K_ESC, 0, 0}, {K_ESC, 0, 0},                                                // 11-12
  {'1', '!', '!'}, {'2', '@', '@'}, {'3', '#', '#'}, {'4', '$', '$'},     // 13-16
  {'5', '%', '%'}, {'6', '^', '^'}, {'7', '&', '&'}, {'8', '*', '*'},     // 17-20
  {'q', 'Q', '\''}, {'w', 'W', '_'}, {'e', 'E', '-'}, {'r', 'R', '+'},    // 21-24
  {'t', 'T', '='}, {'y', 'Y', '\\'}, {'u', 'U', '|'}, {'i', 'I', ';'},    // 25-28
  {'o', 'O', ':'}, {'p', 'P', '"'},                                        // 29-30
  {0, 0, 0},                                                               // 31 Caps Lock
  {'a', 'A', '~'}, {'s', 'S', '['}, {'d', 'D', ']'}, {'f', 'F', '{'},     // 32-35
  {'g', 'G', '}'}, {'h', 'H', ','}, {'j', 'J', '`'}, {'k', 'K', '/'},     // 36-39
  {'l', 'L', '?'},                                                         // 40
  {0, 0, 0},                                                               // 41 Alt
  {'z', 'Z', 0}, {'x', 'X', 0}, {'c', 'C', 0}, {'v', 'V', 0},             // 42-45
  {'b', 'B', '.'}, {'n', 'N', '<'}, {'m', 'M', '>'},                      // 46-48
  {0, 0, 0},                                                               // 49
  {P4KB_UP, 0, 0},                                                         // 50
  {0, 0, 0},                                                               // 51 Sym
  {0, 0, 0},                                                               // 52
  {0, 0, 0},                                                               // 53 Shift
  {K_TAB, 0, 0},                                                             // 54
  {' ', 0, 0}, {' ', 0, 0}, {' ', 0, 0},                                   // 55-57 space bar
  {0, 0, 0},                                                               // 58 Sym
  {P4KB_LEFT, 0, 0}, {P4KB_DOWN, 0, 0},                                    // 59-60
  {FKEY(11), 0, 0},                                                           // 61
  {'9', '(', '('},                                                         // 62
  {K_BS, 0, 0}, {K_ENT, 0, 0}, {P4KB_EMOJI, 0, 0}, {K_ENT, 0, 0},                // 63-66
  {'0', ')', ')'},                                                         // 67
  {P4KB_RIGHT, 0, 0},                                                      // 68
};

bool     s_present = false;
uint32_t s_next_probe_ms = 0;
uint32_t s_last_drain_ms = 0;

uint8_t  s_mod = 0;
uint32_t s_mod_ms = 0;
bool     s_caps = false;

// The key being held for repeat (Backspace, the arrows), 0 when none.
uint8_t  s_held_num = 0;
int      s_held_code = 0;
uint32_t s_held_next_ms = 0;

// Backlight steps F11 walks through, off first. PWM duty is far from linear to
// the eye, so the low steps sit close together.
constexpr uint8_t kLightLevels[P4KB_LIGHT_STEPS] = { 0, 24, 90, 255 };

bool     s_bl_attached = false;
uint8_t  s_light_step = P4KB_LIGHT_STEPS - 1;   // full, until the UI restores the saved step
bool     s_light_changed = false;               // F11 moved it; the UI saves it
bool     s_screen_on = true;
int      s_bl_level = -1;      // what the pin was last set to

constexpr uint8_t kRingSize = 32;
int      s_ring[kRingSize];
uint8_t  s_head = 0;
uint8_t  s_tail = 0;

void ringPush(int key) {
  const uint8_t next = (uint8_t)((s_head + 1) % kRingSize);
  if (next == s_tail) return;   // full: drop the newest, keep what was typed in order
  s_ring[s_head] = key;
  s_head = next;
}

bool regWrite(TwoWire* w, uint8_t addr, uint8_t reg, uint8_t val) {
  w->beginTransmission(addr);
  w->write(reg);
  w->write(val);
  return w->endTransmission() == 0;
}

bool regRead(TwoWire* w, uint8_t addr, uint8_t reg, uint8_t& val) {
  w->beginTransmission(addr);
  w->write(reg);
  if (w->endTransmission(false) != 0) return false;
  if (w->requestFrom(addr, (uint8_t)1) != 1) return false;
  val = (uint8_t)w->read();
  return true;
}

void applyBacklight() {
  if (!s_bl_attached) return;
  const int level = (s_present && s_screen_on) ? kLightLevels[s_light_step] : 0;
  if (level == s_bl_level) return;
  ledcWrite(P4KB_BL_PIN, (uint32_t)level);
  s_bl_level = level;
}

// Caps Lock lights all three, as LilyGo's own keyboard example does. The rest
// of port 0 is read back and left as it was.
void setLeds(TwoWire* w, bool on) {
  uint8_t out = 0xFF, cfg = 0xFF;
  if (!regRead(w, P4KB_EXP_ADDR, XL_OUT0, out) || !regRead(w, P4KB_EXP_ADDR, XL_CFG0, cfg)) return;
  out = on ? (uint8_t)(out & ~P4KB_EXP_LED_MASK) : (uint8_t)(out | P4KB_EXP_LED_MASK);
  cfg &= (uint8_t)~P4KB_EXP_LED_MASK;
  regWrite(w, P4KB_EXP_ADDR, XL_OUT0, out);
  regWrite(w, P4KB_EXP_ADDR, XL_CFG0, cfg);
}

// Pulse the TCA8418's reset through the expansion's XL9555, then see it answer.
bool resetController(TwoWire* w) {
  uint8_t out = 0xFF, cfg = 0xFF;
  if (!regRead(w, P4KB_EXP_ADDR, XL_OUT0, out) || !regRead(w, P4KB_EXP_ADDR, XL_CFG0, cfg)) return false;
  const uint8_t bit = (uint8_t)(1u << P4KB_EXP_RESET_BIT);
  cfg &= (uint8_t)~bit;
  out &= (uint8_t)~bit;
  if (!regWrite(w, P4KB_EXP_ADDR, XL_OUT0, out) || !regWrite(w, P4KB_EXP_ADDR, XL_CFG0, cfg)) return false;
  delay(10);
  out |= bit;
  if (!regWrite(w, P4KB_EXP_ADDR, XL_OUT0, out)) return false;
  delay(10);
  w->beginTransmission(P4KB_TCA_ADDR);
  return w->endTransmission() == 0;
}

// The matrix setup camillia-mt uses (after Meshtastic's tlora-pager profile),
// with seven rows instead of the Pager's four.
void setupMatrix(TwoWire* w) {
  for (uint8_t i = 0; i < 3; ++i) {
    regWrite(w, P4KB_TCA_ADDR, TCA_GPIO_DIR_1 + i, 0x00);
    regWrite(w, P4KB_TCA_ADDR, TCA_GPI_EM_1 + i, 0xFF);
    regWrite(w, P4KB_TCA_ADDR, TCA_GPIO_INT_LVL_1 + i, 0x00);
    regWrite(w, P4KB_TCA_ADDR, TCA_GPIO_INT_EN_1 + i, 0xFF);
    regWrite(w, P4KB_TCA_ADDR, TCA_DEBOUNCE_DIS_1 + i, 0x00);
  }
  regWrite(w, P4KB_TCA_ADDR, TCA_KP_GPIO_1,     0x7F);   // rows 0-6
  regWrite(w, P4KB_TCA_ADDR, TCA_KP_GPIO_1 + 1, 0xFF);   // columns 0-7
  regWrite(w, P4KB_TCA_ADDR, TCA_KP_GPIO_1 + 2, 0x03);   // columns 8-9
  // Throw away whatever the FIFO held from before, bounded in case it misbehaves.
  uint8_t ev = 0;
  for (int i = 0; i < 16 && regRead(w, P4KB_TCA_ADDR, TCA_KEY_EVENT_A, ev) && ev; ++i) {}
  regWrite(w, P4KB_TCA_ADDR, TCA_INT_STAT, 0x03);
}

void clearState() {
  s_mod = 0;
  s_caps = false;
  s_held_num = 0;
  s_held_code = 0;
  s_head = s_tail = 0;
}

bool attach(TwoWire* w) {
  if (!resetController(w)) return false;
  // Pulled down here, up on the expansion: low while it is away, and while the
  // TCA8418 has events waiting.
  pinMode(P4KB_INT_PIN, INPUT_PULLDOWN);
  if (!s_bl_attached) {
    s_bl_attached = ledcAttach(P4KB_BL_PIN, P4KB_BL_HZ, 8);
    if (!s_bl_attached) printf("[P4KB] backlight PWM attach failed\n");
  }
  setupMatrix(w);
  clearState();
  setLeds(w, false);
  s_present = true;
  s_bl_level = -1;
  applyBacklight();
  printf("[P4KB] keyboard expansion attached\n");
  return true;
}

void detach() {
  s_present = false;
  clearState();
  applyBacklight();   // nothing is driven into an empty connector
  printf("[P4KB] keyboard expansion removed\n");
}

bool repeats(int code) {
  return code == K_BS || code == P4KB_UP || code == P4KB_DOWN || code == P4KB_LEFT || code == P4KB_RIGHT;
}

// One press to a key code, or 0 for a modifier / unused key.
int translate(TwoWire* w, uint8_t k, uint32_t now) {
  if (s_mod && (now - s_mod_ms) > kModTimeoutMs) s_mod = 0;
  switch (k) {
    case kKeyShift: s_mod ^= kModShift; s_mod_ms = now; return 0;
    case kKeySym1:
    case kKeySym2:  s_mod ^= kModSym;   s_mod_ms = now; return 0;
    case kKeyAlt:   s_mod ^= kModAlt;   s_mod_ms = now; return 0;
    case kKeyCaps:
      s_caps = !s_caps;
      setLeds(w, s_caps);
      return 0;
    case kKeyLight:
      s_light_step = (uint8_t)((s_light_step + 1) % P4KB_LIGHT_STEPS);
      s_light_changed = true;
      applyBacklight();
      return 0;
    default: break;
  }
  if (k < 1 || k > kKeyCount) return 0;
  const KeyDef& d = kMap[k - 1];
  // Alt+Backspace backs out, the chord the Pager and T-Deck Pro keyboards share.
  if ((s_mod & kModAlt) && k == kKeyBksp) { s_mod = 0; return K_ESC; }
  int code = (s_mod & kModSym) ? d.sym : (s_mod & kModShift) ? d.shift : d.base;
  if (!code) code = d.base;
  if (s_caps && d.base >= 'a' && d.base <= 'z')
    code = (s_mod & kModShift) ? d.base : d.base - 'a' + 'A';
  s_mod = 0;   // one-shot: consumed by the key it modified
  return code;
}

void drain(TwoWire* w, uint32_t now) {
  // Bounded: four sweeps is forty events, far more than one poll can find.
  for (int sweep = 0; sweep < 4; ++sweep) {
    uint8_t count = 0;
    if (!regRead(w, P4KB_TCA_ADDR, TCA_KEY_LCK_EC, count)) return;
    count &= 0x0F;
    // Acknowledged before the events are popped, so a key struck mid-drain
    // raises INT again rather than being acknowledged unseen.
    regWrite(w, P4KB_TCA_ADDR, TCA_INT_STAT, 0x03);
    if (!count) return;
    for (uint8_t i = 0; i < count; ++i) {
      uint8_t ev = 0;
      if (!regRead(w, P4KB_TCA_ADDR, TCA_KEY_EVENT_A, ev) || !ev) return;
      const bool pressed = (ev & 0x80) != 0;
      const uint8_t k = ev & 0x7F;
      if (!pressed) {
        // Releases never reach translate(): they would toggle the modifiers back.
        if (k == s_held_num) s_held_num = 0;
        continue;
      }
      const int code = translate(w, k, now);
      if (!code) continue;
      ringPush(code);
      if (repeats(code)) {
        s_held_num = k;
        s_held_code = code;
        s_held_next_ms = now + kRepeatDelayMs;
      }
    }
  }
}

}  // namespace

void p4KeyboardPoll() {
  const uint32_t now = millis();
  const bool probe = (int32_t)(now - s_next_probe_ms) >= 0;
  const bool int_active = s_present && digitalRead(P4KB_INT_PIN) == LOW;
  const bool drain_due = s_present && (int_active || (now - s_last_drain_ms) >= kIdlePollMs);

  if (probe || drain_due) {
    if (TwoWire* w = p4I2c1Acquire(P4_I2C1_KEYBOARD, kBusWaitMs)) {
      if (probe) {
        // Back off while nothing is there. The probe is a real I2C transaction,
        // and on an empty bus Arduino's Wire layer logs TWO [E] lines for every
        // attempt -- at 1.5 s that is a permanent drip down the same USB-CDC the
        // companion app talks over, for a board most people have never clipped
        // on. After a few misses, look every kHotplugIdleMs instead; a hit puts
        // it straight back to the responsive rate, so hot-plug still works, it
        // just takes up to that long to notice.
        s_next_probe_ms = now + (s_misses >= kMissesBeforeBackoff ? kHotplugIdleMs
                                                                  : kHotplugMs);
        uint8_t cfg = 0;
        const bool there = regRead(w, P4KB_EXP_ADDR, XL_CFG0, cfg);
        if (there) s_misses = 0;
        else if (s_misses < kMissesBeforeBackoff) ++s_misses;
        // One-shot bus scan on the first probe. "It does not work" on this
        // expansion has three very different causes and they are impossible to
        // tell apart from the outside: the board is not clipped on, it is
        // clipped on but unpowered (it carries its own cells, and the firmware
        // has no enable for it), or it answers and the bring-up fails. An
        // unpowered board does NOT give a clean NACK either: with its pull-ups
        // dead the lines sit low through its ESD diodes and every transaction
        // comes back ESP_ERR_INVALID_STATE, which reads like a driver fault.
        // So print what is actually on the bus, once, and let the log say which
        // of the three it is. Same one-shot scan the touch bring-up does.
        if (!s_scanned) {
          s_scanned = true;
          char sc[96]; int n = 0;
          n += snprintf(sc, sizeof sc, "[P4KB] i2c46/45 scan:");
          for (uint8_t a = 0x08; a < 0x78 && n < (int)sizeof sc - 6; a++) {
            w->beginTransmission(a);
            if (w->endTransmission() == 0) n += snprintf(sc + n, sizeof sc - n, " %02X", a);
          }
          if (n < (int)sizeof sc - 24 && !there)
            snprintf(sc + n, sizeof sc - n, "  (expansion absent)");
          printf("%s\n", sc);
        }
        if (there && !s_present) {
          if (!attach(w)) printf("[P4KB] keyboard expansion found, bring-up failed\n");
        } else if (!there && s_present) {
          detach();
        }
      }
      if (s_present && drain_due) {
        s_last_drain_ms = now;
        drain(w, now);
      }
      p4I2c1Release();
    }
  }

  if (s_present && s_held_num && (int32_t)(now - s_held_next_ms) >= 0) {
    ringPush(s_held_code);
    s_held_next_ms = now + kRepeatEveryMs;
  }
}

int p4KeyboardReadKey() {
  if (s_tail == s_head) return 0;
  const int key = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) % kRingSize);
  return key;
}

bool p4KeyboardPresent() { return s_present; }

uint8_t p4KeyboardLightStep() { return s_light_step; }

void p4KeyboardSetLightStep(uint8_t step) {
  if (step >= P4KB_LIGHT_STEPS) step = P4KB_LIGHT_STEPS - 1;
  s_light_step = step;
  applyBacklight();
}

bool p4KeyboardTakeLightChanged() {
  const bool changed = s_light_changed;
  s_light_changed = false;
  return changed;
}

void p4KeyboardSetScreenOn(bool on) {
  if (s_screen_on == on) return;
  s_screen_on = on;
  applyBacklight();
}

#endif
