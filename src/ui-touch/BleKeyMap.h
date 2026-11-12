// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// HID-usage -> Unicode maps for the external Bluetooth keyboard.
//
// A keyboard only reports key POSITIONS, so the same report means "q" on a
// US deck, "a" on an AZERTY deck and "я" on a Bulgarian phonetic deck. The
// tables here are the ones BleKeyboard.cpp decodes reports with; keeping them
// out of that translation unit makes them host-testable (test/test_ble_keymap).
//
// Bulgarian follows the traditional phonetic layout: q=я, w=в, e=е, ...,
// [ = ш, ] = щ, \ = ю, ` = ч. Shift gives the capital and Caps Lock flips it.

#include <stddef.h>
#include <stdint.h>

#include "KeyboardLayouts.h"   // KeyboardLayoutId

namespace BleKbd {

// The layouts an external keyboard can type today. Keep this order in step
// with the settings dropdown: US, UK, German, French, Belgian, Bulgarian.
enum class Layout : uint8_t { US = 0, UK, DE, FR, BE, BG, Count };

}  // namespace BleKbd

namespace BleKeyMap {

// mapKey() sets this on a dead-key result (German/French accents). The low
// bits are the accent to wait for.
constexpr uint32_t kDeadFlag = 0x80000000u;

// Code point for one HID usage (0x04..0x64), or kDeadFlag | accent for a dead
// key. 0 means the key types nothing in this layout.
uint32_t mapKey(BleKbd::Layout layout, uint8_t usage, bool shift, bool altgr, bool caps);

// Accent + key -> composed code point, or 0 when the pair has no letter.
uint32_t compose(uint32_t accent, uint32_t cp);

// The on-screen/shared layout a Bluetooth layout corresponds to. False for
// the two Bluetooth-only decks (British UK, Belgian) that have no matching
// KeyboardLayoutId.
bool toKeyboardLayout(BleKbd::Layout layout, KeyboardLayoutId* out);
bool fromKeyboardLayout(KeyboardLayoutId id, BleKbd::Layout* out);

// Two-letter code for the status bar and the composer badge: EN, BG, ...
const char* layoutCode(BleKbd::Layout layout);

}  // namespace BleKeyMap
