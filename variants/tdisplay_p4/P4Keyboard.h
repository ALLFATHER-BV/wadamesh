// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// LilyGo T-Display P4 keyboard expansion: a clip-on QWERTY board with a TCA8418
// matrix controller (@0x34, 7 rows x 10 columns) and its own XL9555 expander
// (@0x20: TCA8418 reset, three indicator LEDs), on Wire1's keyboard route
// (P4I2c1.h). Backlight on GPIO47 (PWM), TCA8418 INT on GPIO48.
//
// The expansion clips on and off, so it is looked for on a timer, not once at
// boot: while it is away the UI keeps the on-screen keyboard, and when it
// arrives the UI treats it as an external keyboard, the way it treats a CardKB
// or a Bluetooth keyboard on the other touchscreen boards.
//
// Ported from camillia-mt (src/keyboard.cpp, the DEVICE_TDISPLAY_P4 paths),
// which has it working on hardware.
#if defined(HAS_TDISPLAY_P4_KEYBOARD)

#include <stdint.h>

// Key codes from p4KeyboardReadKey(): printable ASCII, Backspace 0x08, Tab 0x09,
// Enter 0x0D, Esc 0x1B, and these.
constexpr int P4KB_UP    = 0x100;
constexpr int P4KB_DOWN  = 0x101;
constexpr int P4KB_LEFT  = 0x102;
constexpr int P4KB_RIGHT = 0x103;
constexpr int P4KB_EMOJI = 0x104;
constexpr int P4KB_F1    = 0x110;   // F1..F11 are P4KB_F1 + 0..10 (F11 is taken here: the light)

/** UI loop, every pass: looks for the expansion (every 1.5 s), drains the
 *  TCA8418 into the key ring, and repeats a held Backspace or arrow. */
void p4KeyboardPoll();

/** Next key, or 0 when none. */
int  p4KeyboardReadKey();

/** True while the expansion is clipped on and answering. */
bool p4KeyboardPresent();

/** The backlight follows the screen: lit at the current step while it is on,
 *  dark while it is off. */
void p4KeyboardSetScreenOn(bool on);

/** Backlight brightness: F11 steps Off, Low, Medium, High, then back to Off.
 *  Step 0 is off, P4KB_LIGHT_STEPS - 1 is full. The driver does not persist it:
 *  the UI restores the saved step at boot with p4KeyboardSetLightStep() and
 *  saves it again when p4KeyboardTakeLightChanged() reports an F11 press. */
constexpr uint8_t P4KB_LIGHT_STEPS = 4;
uint8_t p4KeyboardLightStep();
void    p4KeyboardSetLightStep(uint8_t step);
bool    p4KeyboardTakeLightChanged();

#endif
