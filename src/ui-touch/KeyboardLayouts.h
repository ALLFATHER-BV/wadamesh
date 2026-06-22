/*******************************************************************************
 * KeyboardLayouts.h — extensible on-screen + physical keyboard layout registry
 *
 * Adding a new language later only requires editing KeyboardLayouts.cpp.
 ******************************************************************************/
#pragma once

#include <stdint.h>

/* Forward-declare LVGL types so we don't pull lvgl.h into every translation unit. */
struct _lv_obj_t;
typedef struct _lv_obj_t lv_obj_t;

enum class KeyboardLayoutId : uint8_t {
    EN = 0,
    BG = 1,   // Bulgarian (phonetic)
    RU = 2,   // Russian (ЙЦУКЕН on-screen, phonetic physical)
    UK = 3,   // Ukrainian (phonetic) — best-effort, validate with a native speaker
    SR = 4,   // Serbian (phonetic) — best-effort, validate with a native speaker
    EL = 5,   // Greek (ΕΛΟΤ-style phonetic)
    AR = 6,   // Arabic (RTL, Arabic-101 positions) — EXPERIMENTAL, needs on-device validation
    FR = 7,   // French (AZERTY)
    NL = 8,   // Dutch (QWERTY + accent popups)
    DE = 9,   // German (QWERTZ)
    ES = 10,  // Spanish (QWERTY + accent popups)
    IT = 11,  // Italian (QWERTY + accent popups)
    Count
};

constexpr int KEYBOARD_LAYOUT_COUNT = static_cast<int>(KeyboardLayoutId::Count);

/** Human-readable label for a layout (e.g. "EN", "BG"). */
const char* keyboardLayoutName(KeyboardLayoutId id);

/** Apply an on-screen keyboard layout to an LVGL keyboard widget.
 *  Replaces the TEXT_LOWER and TEXT_UPPER maps; keeps the default SPECIAL map.
 */
void keyboardLayoutsApply(lv_obj_t* keyboard, KeyboardLayoutId id);

/** Currently active layout (cached in RAM). */
KeyboardLayoutId keyboardLayoutsGetCurrent();

/** Cycle to the next ENABLED layout (the double-tap-space action). The cycle is
 *  English plus whichever layouts are switched on in the enabled mask, walked in
 *  KeyboardLayoutId order and wrapping back to English. Returns the new active
 *  layout. With nothing enabled it stays on English. */
KeyboardLayoutId keyboardLayoutsCycle(lv_obj_t* keyboard);

/** Enabled-layout set. Bit (1 << KeyboardLayoutId) marks a layout as part of the
 *  space-cycle; English is always implicitly in the cycle. Setting a mask that
 *  no longer contains the active layout snaps the active layout back to English.
 *  Cached in RAM — persistence is the caller's job (TouchPrefsStore). */
void     keyboardLayoutsSetEnabledMask(uint32_t mask);
uint32_t keyboardLayoutsGetEnabledMask();

/** True if any non-English layout is enabled (i.e. the space-cycle does
 *  something and the status indicator should show). */
bool keyboardLayoutsAnySecondary();

/** Map a physical T-Deck key to a UTF-8 string in the given layout.
 *  @param key    ASCII code from tdeckKeyboardReadKey() (e.g. 'a', 'A', '1', '!')
 *  @param shifted true if Shift was active (key >= 'A' && key <= 'Z', or symbol)
 *  @return UTF-8 string to insert, or nullptr to pass the key through unchanged.
 */
const char* keyboardLayoutMapHwKey(KeyboardLayoutId id, int key, bool shifted);
