#pragma once

// LilyGo T-Deck physical keyboard. It's a separate ESP32-C3 running the
// T-Keyboard firmware on the shared I2C bus (SDA 18 / SCL 8, addr 0x55).
// Current controller firmware supports a five-byte raw matrix mode, which lets
// this driver preserve held modifiers and add tap/double-tap latching. Older
// firmware returns one resolved ASCII byte; startup detects that and restores
// the original controller-side key mode automatically.
//
// CRITICAL: the keyboard shares the I2C bus with the GT911 touch controller,
// which is polled from a core-0 task. To avoid two cores hitting Wire at once,
// tdeckKeyboardPoll() must be called from THAT task; the UI thread only drains
// the critical-section-protected ring via tdeckKeyboardReadKey().
#if defined(HAS_TDECK_KEYBOARD) && defined(ESP32)

#include <stdint.h>

/** Mark the keyboard available. The I2C bus is already brought up by the touch
 *  driver, so this just enables polling. */
void tdeckKeyboardBegin();

/** Force the older keyboard protocol, skipping raw detection. Escape hatch for a
 *  controller the probe gets wrong; call before/at begin and on the setting. */
void tdeckKeyboardForceLegacy(bool on);

/** Read one key over I2C and push it into the ring. CORE-0 ONLY (touch task). */
void tdeckKeyboardPoll();

/** Pop the next buffered key (ASCII), or 0 if none. Safe from the UI thread. */
int tdeckKeyboardReadKey();

/** Cancel one-shot/locked modifiers and suppress a later release from a
 * currently held modifier. Call when queued input is intentionally discarded. */
void tdeckKeyboardDiscardModifiers();

/** Stop continuously discarding raw modifier state after the UI returns to a
 * mode where keyboard input is meaningful. */
void tdeckKeyboardAllowModifiers();

/** Request a keyboard-backlight level (0 = off, 0xFF = on). Safe from the UI
 *  thread — the I2C write happens inside tdeckKeyboardPoll() (core 0), which
 *  owns the bus. Only re-sent when the value changes. */
void tdeckKeyboardSetBacklight(uint8_t level);

/** Flush a pending backlight request over I2C NOW. CORE-0 ONLY (touch task).
 *  Cheap when nothing is pending (one flag check); called every poll tick so a
 *  brightness change lands within ~one touch-poll period (~8 ms) instead of
 *  waiting for the next full keyboard scan (~32 ms). */
void tdeckKeyboardFlushBacklight();

#endif
