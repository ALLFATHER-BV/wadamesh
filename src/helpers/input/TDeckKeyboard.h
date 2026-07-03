#pragma once

// LilyGo T-Deck physical keyboard. It's a separate ESP32-C3 running the
// T-Keyboard firmware on the shared I2C bus (SDA 18 / SCL 8, addr 0x55): each
// 1-byte read returns the ASCII code of the last key press (0 = none). The C3
// resolves shift / sym layers itself, so we just receive final characters.
//
// CRITICAL: the keyboard shares the I2C bus with the GT911 touch controller,
// which is polled from a core-0 task. To avoid two cores hitting Wire at once,
// tdeckKeyboardPoll() must be called from THAT task; the UI thread only drains
// the lock-free ring via tdeckKeyboardReadKey().
#if defined(HAS_TDECK_KEYBOARD) && defined(ESP32)

#include <stdint.h>

/** Mark the keyboard available. The I2C bus is already brought up by the touch
 *  driver, so this just enables polling. */
void tdeckKeyboardBegin();

/** Read one key over I2C and push it into the ring. CORE-0 ONLY (touch task). */
void tdeckKeyboardPoll();

/** Pop the next buffered key (ASCII), or 0 if none. Safe from the UI thread. */
int tdeckKeyboardReadKey();

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
