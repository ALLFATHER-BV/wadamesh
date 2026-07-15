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

/** True while Alt is physically held (raw modifier state, tracked by
 *  pagerKeyboardPoll() — not a ring event, since Alt alone drives the symbol
 *  layer and is never itself pushed as a key). Lets other drivers build
 *  Alt+<gesture> shortcuts (e.g. the rotary encoder's Alt+turn tab switch)
 *  without a second, separate modifier concept. */
bool pagerKeyboardAltHeld();

/** Mark the currently-held Alt as "used as a modifier" — call this when some
 *  other gesture (e.g. the rotary encoder's Alt+turn) consumes the hold, so
 *  releasing Alt afterward isn't also read as a solo tap by
 *  pagerKeyboardConsumeAltTap(). */
void pagerKeyboardMarkAltUsed();

/** One-shot: true exactly once if Alt was pressed and released without being
 *  used as a modifier for anything else in between (no key typed, no
 *  pagerKeyboardMarkAltUsed() call) — a "solo tap", distinct from a
 *  symbol-layer or Alt+turn hold. Consumes the pending flag on read. */
bool pagerKeyboardConsumeAltTap();

/** True while Backspace is physically held WITHOUT Alt (raw state, mirrors
 *  pagerKeyboardAltHeld()). A plain press still immediately ring-pushes '\b'
 *  as before; this is for callers that want to detect a long hold separately
 *  (e.g. UITask's press-and-hold "back" gesture). Alt+Backspace is a
 *  different gesture entirely (see pagerKeyboardConsumeAltBackspaceChord())
 *  and never sets this or ring-pushes '\b'. */
bool pagerKeyboardBackspaceHeld();

/** True while Space is physically held (raw state, mirrors
 *  pagerKeyboardBackspaceHeld()). A press still immediately ring-pushes ' '
 *  as before, so normal typing is unaffected; this is for callers that want
 *  to detect a long hold separately (e.g. UITask's press-and-hold
 *  "lock screen" gesture). */
bool pagerKeyboardSpaceHeld();

/** One-shot: true exactly once after Alt(Fn)+Shift is chorded (Shift pressed
 *  while Alt is held). The driver no longer decides what this chord DOES
 *  (that depends on UI state — is a text field being edited? — which this
 *  driver has no visibility into), it only reports that the chord happened;
 *  consumes the pending flag on read. See pagerKeyboardToggleCaps(). */
bool pagerKeyboardConsumeAltShiftChord();

/** Toggle persistent Caps Lock. Callers gate this on the Alt+Shift chord
 *  above only applying while a text field is actually being edited. */
void pagerKeyboardToggleCaps();

/** One-shot: true exactly once after Alt(Fn)+Backspace is chorded (Backspace
 *  pressed while Alt is held) — jumps Home, everywhere (editing a field or
 *  not), unlike the Alt+Shift chord above. Suppresses the normal Backspace
 *  press entirely: no '\b' ring-push, and pagerKeyboardBackspaceHeld() never
 *  reports held for this press, so it can't also fire the plain-Backspace
 *  hold-to-back/hold-to-unlock gestures. Consumes the pending flag on read. */
bool pagerKeyboardConsumeAltBackspaceChord();

#endif
