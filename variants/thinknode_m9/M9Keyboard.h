#pragma once

// ThinkNode M9 keyboard controller. Unlike the T-Deck — where the keyboard
// shares an I2C bus with the touch controller and must be polled from a
// dedicated core-0 task — the M9 keyboard lives on its OWN bus (Wire1,
// SDA=20 / SCL=21, addr 0x6c) with no touch controller present at all. So
// this can be polled directly from the UI thread; no cross-core ring buffer
// is strictly required, but one is kept anyway for interface parity with
// TDeckKeyboard and so a future move to a dedicated poll task is a no-op.
//
// PROTOCOL — from Elecrow's own keyboard-controller firmware
// (ThinkNode-M9-KB-platformio): the keyboard is a separate ESP32-S2 running a
// matrix scanner as an I2C slave with ADDRESSED REGISTERS. A key read is
// write-reg(0x01) then read 1 byte (0x00 = no key; the controller latches a
// single pending key). The earlier "one raw byte per read" protocol note was
// WRONG — a bare read returns the last-addressed register (0x00 = HW version,
// constant 0x03), which is why bring-up builds 1-7 saw no keys. The
// controller resolves shift/symbol/alt layers itself and emits final ASCII
// for printable keys; non-ASCII bytes are the d-pad / dedicated function
// keys (see M9_KEY_* below), passed through unchanged, so
// m9KeyboardReadKey() returns either a printable ASCII code or a sentinel.
// Backlight: write-reg(0x02) + duty byte; the controller auto-lights on
// keypress and times out after 10 s on its own.
#if defined(HAS_M9_KEYBOARD) && defined(ESP32)

#include <stdint.h>

// Raw byte values confirmed on hardware (Specter bring-up, notes/m9_keycodes.md).
#define M9_KEY_LEFT          0xB4
#define M9_KEY_UP            0xB5
#define M9_KEY_DOWN          0xB6
#define M9_KEY_RIGHT         0xB7
#define M9_KEY_ENTER         0x0D   // shared with d-pad centre click — indistinguishable
#define M9_KEY_DEL            0x08
#define M9_KEY_MIC           0x88   // triangle/mic glyph key
#define M9_KEY_LEFT_MESSAGE  0x81
#define M9_KEY_HOME          0x82
#define M9_KEY_SUB_MESSAGE   0x83
#define M9_KEY_SUB_MAP       0x84
#define M9_KEY_MAP           0x85
#define M9_KEY_HW_BACK       0x86
#define M9_KEY_CTRL          0x90
// From the controller firmware's key tables (not yet bound in the UI):
// 0x84 long-press emits 0x87 (labeled "GPS toggle"), Enter long-press emits
// 0xA3, and matrix position col3/row4 emits 0x89 (unlabeled). Shift/Sym/Alt
// never reach the wire — the controller consumes them as layer modifiers.
#define M9_KEY_GPS_LONG      0x87
#define M9_KEY_ENTER_LONG    0xA3

/** Bring up Wire1 (SDA=PIN_KB_SDA, SCL=PIN_KB_SCL) and mark the keyboard
 *  available. Safe to call from the UI thread — this bus has no other
 *  device on it. */
void m9KeyboardBegin();

/** Read one key over I2C and push it into the ring. May be called from any
 *  single task (this board has no cross-core contention on Wire1). */
void m9KeyboardPoll();

/** Pop the next buffered raw byte (ASCII or M9_KEY_* sentinel), or 0 if none. */
int m9KeyboardReadKey();

/** Set the keyboard backlight PWM duty (0..255) on the controller. The
 *  controller lights up on keypress by itself and times out after 10 s;
 *  this just sets HOW bright. No-op until the controller has been found. */
void m9KeyboardSetBacklight(uint8_t duty);

#endif
