// LilyGo T-Deck physical keyboard driver — see TDeckKeyboard.h.
#if defined(HAS_TDECK_KEYBOARD) && defined(ESP32)

#include "TDeckKeyboard.h"
#include "TDeckKeyboardState.h"
#include <Arduino.h>
#include <Wire.h>

#ifndef PIN_KB_ADDR
  #define PIN_KB_ADDR 0x55
#endif

// Core-0 produces and the UI task consumes. One short cross-core critical
// section publishes each byte together with its index and also protects the
// desired modifier-input mode below.
static portMUX_TYPE s_keyboard_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_ring[16];
static uint8_t s_head = 0;
static uint8_t s_tail = 0;
static bool             s_inited = false;
enum class KeyboardMode : uint8_t { Probe, Raw, Legacy };
// How long to keep asking the C3 for raw frames before settling for legacy mode.
// Generous on purpose: getting this wrong costs the user modifier latching for
// the entire session, and the only cost of waiting is that the first second of
// keystrokes goes through the legacy path, which still works.
static const uint32_t kProbeWindowMs = 1500;
static KeyboardMode       s_mode = KeyboardMode::Probe;
static TDeckKeyboardState s_raw_state;
static bool               s_modifier_input_allowed = true; // guarded by s_keyboard_mux
static uint32_t           s_modifier_mode_generation = 0;   // guarded by s_keyboard_mux
static uint32_t           s_modifier_generation_applied = 0; // core-0 owner only

// Backlight: the UI thread requests a level; the actual I2C write happens in the
// poll (core 0). The keyboard's C3 firmware sets the backlight on an I2C write.
static volatile uint8_t s_bl_desired = 0;
static volatile bool    s_bl_dirty   = false;

static void ringPushLocked(uint8_t key) {
  if (!key) return;
  const uint8_t next = (uint8_t)((s_head + 1) & 15);
  if (next != s_tail) {
    s_ring[s_head] = key;
    s_head = next;
  }
}

static void keyboardCommand(uint8_t command) {
  Wire.beginTransmission(PIN_KB_ADDR);
  Wire.write(command);
  Wire.endTransmission();
}

static int keyboardRead(uint8_t* out, size_t count) {
  Wire.requestFrom((int)PIN_KB_ADDR, (int)count);
  int read = 0;
  while (Wire.available() && read < (int)count) out[read++] = (uint8_t)Wire.read();
  while (Wire.available()) Wire.read();
  return read;
}

static void processRawFrame(const uint8_t frame[TDeckKeyboardState::COLS], uint32_t now_ms) {
  uint8_t keys[16];
  portENTER_CRITICAL(&s_keyboard_mux);
  const uint32_t generation = s_modifier_mode_generation;
  if (generation != s_modifier_generation_applied) {
    // Transition frame: establish current physical state, publish nothing.
    // The mode setter already cleared older queued bytes under this same lock.
    s_raw_state.baseline(frame);
    s_modifier_generation_applied = generation;
  } else {
    const size_t key_count = s_raw_state.update(frame, now_ms, keys, sizeof keys,
                                                s_modifier_input_allowed);
    for (size_t i = 0; i < key_count; ++i) ringPushLocked(keys[i]);
  }
  portEXIT_CRITICAL(&s_keyboard_mux);
}

// True when a five-byte answer can only be a legacy ASCII reply: the character
// in byte 0 with padding after it. A real matrix frame puts one bit per pressed
// key in that key's own column, so three or more bits in byte 0 with every other
// column empty is not something the hardware can report.
//
// Some older controllers do not pad: they repeat the character for every byte of
// the read, so a 'q' (0x71) arrives as 71 71 71 71 71. Read as columns that is
// rows 0, 4, 5 and 6 held in all five columns, and one press typed a burst like
// "QERUOXV$ ZNMFJ". Twenty keys held at once is not something a hand does, so
// columns equal to byte 0 count as the same reply.
static bool looksLikeAscii(const uint8_t frame[TDeckKeyboardState::COLS], bool* repeated = nullptr) {
  size_t reps = 0;
  for (size_t col = 1; col < TDeckKeyboardState::COLS; ++col) {
    if (!frame[col]) continue;
    if (frame[col] != frame[0]) return false;
    ++reps;
  }
  if (frame[0] < 0x20 || frame[0] > 0x7E) return false;   // must be a printable character
  int bits = 0;
  for (int b = 0; b < 8; ++b) if (frame[0] & (1u << b)) ++bits;
  // The same byte in all five columns is a repeat even for ' ', '0' or '!' (five or
  // ten keys at once); a partial repeat needs the three-bit test like a padded reply.
  if (bits <= 2 && reps < TDeckKeyboardState::COLS - 1) return false;
  if (repeated) *repeated = reps > 0;
  return true;
}

// Verified against the real tables rather than assumed:
//   - all 26 lowercase letters set 3 or more bits, so ANY letter demotes on the
//     first press. Only '0', ' ' and '!' do not, and space happens to render
//     correctly anyway (column 0 bit 5 IS space).
//   - going the other way, a genuine frame reaching this needs three or more of
//     {q, w, a, space} held at once and nothing else. The printable-range test
//     above removes q+w+a (0x0B); what remains are combinations including space,
//     which is a fumble rather than typing, and costs a reboot to undo.

// Bits set across a frame. A raw frame where a whole hand's worth of keys went down
// in one poll is a controller answering in a format we do not read, not typing.
static int frameBits(const uint8_t frame[TDeckKeyboardState::COLS]) {
  int bits = 0;
  for (size_t col = 0; col < TDeckKeyboardState::COLS; ++col)
    for (int b = 0; b < 8; ++b) if (frame[col] & (1u << b)) ++bits;
  return bits;
}
static const int kBurstBits = 8;

// Set on core 0 when a controller showed the repeated-character reply, which only an
// older controller sends. The UI thread takes it once and saves the older protocol,
// so the next boot skips the probe (tdeckKeyboardTakeLegacyEvidence).
static volatile bool s_legacy_evidence = false;
bool tdeckKeyboardTakeLegacyEvidence() {
  if (!s_legacy_evidence) return false;
  s_legacy_evidence = false;
  return true;
}

static void processLegacyKey(uint8_t key) {
  portENTER_CRITICAL(&s_keyboard_mux);
  const uint32_t generation = s_modifier_mode_generation;
  if (generation != s_modifier_generation_applied) {
    // No raw modifier state exists, and the C3 exposes one latest-byte mailbox
    // (comdata/comdata_flag), not one response per host mode transition. Any
    // number of transitions before this read therefore coalesce deliberately:
    // drop the sole pending byte once so it cannot cross into the final mode.
    s_modifier_generation_applied = generation;
  } else {
    ringPushLocked(key);
  }
  portEXIT_CRITICAL(&s_keyboard_mux);
}

// Set from the UI at boot (Settings > Keyboard). When true the probe is skipped
// entirely and the older protocol is used, which is the escape hatch for a
// controller that detection gets wrong (#351).
static bool s_force_legacy = false;
void tdeckKeyboardForceLegacy(bool on) { s_force_legacy = on; }

// s_mode is written on core 0 (the poll task); a one-byte read from the UI thread
// is atomic, and a stale answer only lags the Settings line by one poll.
uint8_t tdeckKeyboardDetectedMode() {
  switch (s_mode) {
    case KeyboardMode::Raw:    return TDECK_KB_RAW;
    case KeyboardMode::Legacy: return TDECK_KB_LEGACY;
    default:                   return TDECK_KB_PROBING;
  }
}

void tdeckKeyboardBegin() {
  s_mode = KeyboardMode::Probe;
  s_raw_state = TDeckKeyboardState{};
  portENTER_CRITICAL(&s_keyboard_mux);
  s_head = s_tail = 0;
  // Keep any desired mode the UI published immediately after starting this
  // task. Setting applied one generation behind forces the first response to
  // baseline (or drain the legacy controller's one-byte mailbox).
  s_modifier_generation_applied = s_modifier_mode_generation - 1;
  portEXIT_CRITICAL(&s_keyboard_mux);
  s_inited = true;   // Wire was configured (18/8, 400k, 20ms timeout) by the touch driver
}

void tdeckKeyboardSetBacklight(uint8_t level) {
  // Force the FIRST write even when the requested level matches our cached default (0). A reflash
  // resets the ESP32 but NOT the keyboard's C3 — it keeps its previously-lit backlight — so without
  // this the boot "off" request (0 == cached 0) was never sent and the backlight stayed on despite
  // the setting reading "Off" (issue #33). After the first write, change-detection resumes.
  static bool forced = false;
  if (level != s_bl_desired || !forced) { s_bl_desired = level; s_bl_dirty = true; forced = true; }
}

void tdeckKeyboardFlushBacklight() {
  if (!s_inited || !s_bl_dirty) return;
  {
    s_bl_dirty = false;
    // LilyGo T-Keyboard backlight: 2-byte command [0x01, brightness] (0 = off).
    Wire.beginTransmission(PIN_KB_ADDR);
    Wire.write(0x01);            // LILYGO_KB_BRIGHTNESS_CMD
    Wire.write(s_bl_desired);    // 0 = off, 1-255 = brightness
    Wire.endTransmission();
  }
}

void tdeckKeyboardPoll() {
  if (!s_inited) return;
  tdeckKeyboardFlushBacklight();
  if (s_force_legacy && s_mode != KeyboardMode::Legacy) {
    keyboardCommand(0x04);
    s_mode = KeyboardMode::Legacy;
    Serial.println("[keyboard] T-Deck legacy mode: forced in settings");
  }
  if (s_mode == KeyboardMode::Probe) {
    // Deciding whether this controller speaks the raw column protocol.
    //
    // The old test was "five bytes came back and no high bit is set", which a
    // LEGACY controller passes trivially: it answers a five-byte read with the
    // pressed character followed by padding, and every lowercase letter has its
    // high bit clear. Read as a column bitmap, 'a' (0x61) emits "q " and 'm'
    // (0x6d) emits "qa ". That is the garbage Vybo reported on an older T-Deck
    // (#341), and the retry window added in beta_72 made it MORE likely to
    // latch, not less.
    //
    // The positive test: a real matrix frame from a key press has ONE bit set,
    // in whichever column that key lives. A legacy reply only ever has byte 0
    // non-zero, and an ASCII letter sets three or four bits at once. So:
    //
    //   any non-zero byte in columns 1..4  -> genuinely raw (padding is never
    //                                         non-zero there)
    //   byte 0 with >2 bits set, rest zero -> an ASCII character, so legacy
    //
    // An all-zero frame is idle and proves nothing either way, so keep probing.
    // If the window expires having seen only column-0 activity, fall back to
    // LEGACY. That is the safe default: the cost of being wrong is losing
    // modifier latching, never losing the keyboard.
    static uint32_t s_probe_start_ms = 0;
    const uint32_t now = millis();
    if (!s_probe_start_ms) s_probe_start_ms = now;

    keyboardCommand(0x03);
    uint8_t frame[TDeckKeyboardState::COLS] = {};
    const int count = keyboardRead(frame, sizeof frame);

    bool shape_ok = (count == (int)sizeof frame);
    for (size_t col = 0; col < sizeof frame && shape_ok; ++col)
      if (frame[col] & 0x80) shape_ok = false;

    bool repeated = false;
    if (shape_ok && looksLikeAscii(frame, &repeated) && repeated) {
      keyboardCommand(0x04);
      s_mode = KeyboardMode::Legacy;
      s_legacy_evidence = true;
      Serial.printf("[keyboard] T-Deck legacy mode: controller repeated a character "
                    "(0x%02X) across the frame\n", frame[0]);
      processLegacyKey(frame[0]);
      return;
    }
    if (shape_ok && frameBits(frame) >= kBurstBits) {   // see the raw branch below
      keyboardCommand(0x04);
      s_mode = KeyboardMode::Legacy;
      Serial.println("[keyboard] T-Deck legacy mode: probe frame held too many keys at once");
      return;
    }
    if (shape_ok) {
      bool cols1plus = false;
      for (size_t col = 1; col < sizeof frame; ++col) if (frame[col]) cols1plus = true;
      int b0bits = 0;
      for (int b = 0; b < 8; ++b) if (frame[0] & (1u << b)) ++b0bits;

      if (cols1plus) {                       // only a real matrix reaches here
        s_mode = KeyboardMode::Raw;
        Serial.println("[keyboard] T-Deck raw mode: modifier latching enabled");
        processRawFrame(frame, now);
        return;
      }
      if (frame[0] && b0bits > 2) {          // an ASCII byte, not a column
        keyboardCommand(0x04);
        s_mode = KeyboardMode::Legacy;
        Serial.printf("[keyboard] T-Deck legacy mode: controller answered with a character "
                      "(0x%02X), not a column frame\n", frame[0]);
        processLegacyKey(frame[0]);
        return;
      }
      if ((uint32_t)(now - s_probe_start_ms) < kProbeWindowMs) return;   // idle: keep looking
      // Window expired with a well-formed, idle frame every time. That is what a
      // raw controller looks like when nobody is typing, so take it -- a legacy
      // one would have shown us a character by now if it had one, and if it has
      // not, looksLikeAscii() below still demotes the moment it does.
      s_mode = KeyboardMode::Raw;
      Serial.println("[keyboard] T-Deck raw mode: modifier latching enabled");
      return;
    } else if ((uint32_t)(now - s_probe_start_ms) < kProbeWindowMs) {
      if (count == 1) { keyboardCommand(0x04); processLegacyKey(frame[0]); }
      return;                                 // short/garbled read: the C3 may still be waking
    }

    keyboardCommand(0x04);
    s_mode = KeyboardMode::Legacy;
    Serial.printf("[keyboard] T-Deck legacy mode: no raw column frame seen in %lu ms "
                  "(probe read %d bytes)\n", (unsigned long)kProbeWindowMs, count);
    if (count == 1) processLegacyKey(frame[0]);
    return;
  }

  if (s_mode == KeyboardMode::Raw) {
    uint8_t frame[TDeckKeyboardState::COLS] = {};
    if (keyboardRead(frame, sizeof frame) != (int)sizeof frame) return;
    for (size_t col = 0; col < sizeof frame; ++col) if (frame[col] & 0x80) return;
    // Raw mode has to be able to admit it guessed wrong. An idle controller of
    // either kind answers with zeros, so the probe cannot tell them apart until
    // someone types -- and on a legacy controller that first character arrives
    // in byte 0 with three or four bits set, which no single key press can
    // produce. Demote on the spot and deliver the character, so the worst case
    // is one odd keystroke rather than a keyboard that types nonsense forever.
    bool repeated = false;
    if (looksLikeAscii(frame, &repeated)) {
      keyboardCommand(0x04);
      s_mode = KeyboardMode::Legacy;
      if (repeated) s_legacy_evidence = true;
      Serial.printf("[keyboard] T-Deck legacy mode: raw guess withdrawn, controller sent a "
                    "character (0x%02X)%s\n", frame[0], repeated ? ", repeated" : "");
      processLegacyKey(frame[0]);
      return;
    }
    // Any other reply that reads as eight or more keys at once: drop it and use the
    // older protocol for this session. Losing modifier latching is the cost of being
    // wrong; typing garbage is the cost of not doing it. Not saved, as the shape is
    // not one we have seen from a real controller.
    if (frameBits(frame) >= kBurstBits) {
      keyboardCommand(0x04);
      s_mode = KeyboardMode::Legacy;
      Serial.println("[keyboard] T-Deck legacy mode: raw frame held too many keys at once");
      return;
    }
    processRawFrame(frame, millis());
    return;
  }
  uint8_t key = 0;
  if (keyboardRead(&key, 1) == 1) processLegacyKey(key);
}

int tdeckKeyboardReadKey() {
  portENTER_CRITICAL(&s_keyboard_mux);
  if (s_tail == s_head) {
    portEXIT_CRITICAL(&s_keyboard_mux);
    return 0;
  }
  const uint8_t key = s_ring[s_tail];
  s_tail = (uint8_t)((s_tail + 1) & 15);
  portEXIT_CRITICAL(&s_keyboard_mux);
  return key;
}

void tdeckKeyboardDiscardModifiers() {
  portENTER_CRITICAL(&s_keyboard_mux);
  if (s_modifier_input_allowed) {
    s_modifier_input_allowed = false;
    ++s_modifier_mode_generation;
    s_tail = s_head;
  }
  portEXIT_CRITICAL(&s_keyboard_mux);
}

void tdeckKeyboardAllowModifiers() {
  portENTER_CRITICAL(&s_keyboard_mux);
  if (!s_modifier_input_allowed) {
    s_modifier_input_allowed = true;
    ++s_modifier_mode_generation;
    s_tail = s_head;
  }
  portEXIT_CRITICAL(&s_keyboard_mux);
}

#endif
