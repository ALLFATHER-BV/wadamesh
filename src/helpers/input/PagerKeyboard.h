// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// T-LoRa Pager physical QWERTY keyboard: a TCA8418 I2C matrix controller (4
// rows x 10 cols, addr 0x34) on the shared I2C bus (SDA 3 / SCL 2). Unlike
// the T-Deck's keyboard (a second MCU that resolves ASCII itself before we
// ever see a byte), the TCA8418 only reports raw row/col matrix events — the
// keymap + shift/sym/alt state machine lives HERE, so pagerKeyboardReadKey()
// produces the same final ASCII/control-code stream handleHwKey() already
// expects from the T-Deck; no UI-side changes needed to consume it.
//
// Threading: pagerKeyboardPoll() does the I2C read + keymap translation and
// must be called from a single, consistent context each tick (whichever task
// ends up owning it — wired in a later milestone; this board has no
// pre-existing shared-bus task the way the T-Deck's touch poll does).
// pagerKeyboardReadKey() only pops from a lock-free ring and is safe to call
// from the UI thread regardless of which context polls.
#if defined(HAS_PAGER_KEYBOARD) && defined(ESP32)

#include <stdint.h>

/** Bring up the TCA8418 (I2C addr 0x34, 4x10 matrix). One-shot; safe to call
 *  even if the chip isn't present (poll()/readKey() just stay idle). */
void pagerKeyboardBegin();

/** Drain any pending TCA8418 key events, translate through the keymap/shift-
 *  sym-alt state machine, and push resulting characters into the ring.
 *  Call from a single consistent context each tick. */
void pagerKeyboardPoll();

/** Pop the next buffered key (ASCII/control code), or 0 if none. Safe from
 *  the UI thread. */
int pagerKeyboardReadKey();

/** Set the keyboard backlight (0 = off, 1-255 = brightness). Applied
 *  immediately via LEDC PWM on GPIO 46 — unlike the T-Deck's I2C-based
 *  backlight, this is a plain GPIO and needs no deferred flush-on-next-poll. */
void pagerKeyboardSetBacklight(uint8_t level);

#endif
