// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the P4 keyboard remap (kbP4Remap in KeyboardLayouts.cpp).
// Compiles the real translation unit against a minimal LVGL stub and checks
// that every language's on-screen map comes out in the phone-style shape:
//   5 rows, utility row on top (1#/shift/lang/cursor/backspace), three letter
//   rows of 9-12 keys holding only letters/punctuation, bottom row
//   [Sym][space][new-line][OK], and the language cell follows the enabled mask.
//
// Build: g++ -std=c++17 -I test/lvgl_stub -I src/ui-touch -I src \
//        test/test_kb_p4_remap.cpp -o /tmp/test_kb_p4_remap && /tmp/test_kb_p4_remap
#include <cstdio>
#include <cstring>
// The remap under test exists only on the round-cornered P4.
#define HAS_TDISPLAY_P4 1
#include "lvgl.h"
#include "KeyboardLayouts.h"
#include "ui_icons.h"

// Capture the last maps handed to LVGL.
static const char* const* cap_map[2] = {nullptr, nullptr};
static const lv_btnmatrix_ctrl_t* cap_ctrl[2] = {nullptr, nullptr};
void lv_keyboard_set_map(lv_obj_t*, uint16_t mode,
                         const char* const map[], const lv_btnmatrix_ctrl_t ctrl[]) {
    if (mode == LV_KEYBOARD_MODE_TEXT_LOWER) { cap_map[0] = map; cap_ctrl[0] = ctrl; }
    if (mode == LV_KEYBOARD_MODE_TEXT_UPPER) { cap_map[1] = map; cap_ctrl[1] = ctrl; }
}

#include "../src/ui-touch/KeyboardLayouts.cpp"

static bool is_util(const char* k) {
    return !strcmp(k, "1#") || !strcmp(k, "ABC") || !strcmp(k, "abc") ||
           !strcmp(k, "LANG") || !strcmp(k, LV_SYMBOL_REFRESH) || !strcmp(k, LV_SYMBOL_PASTE) ||
           !strcmp(k, "\xF0\x9F\x98\x8A") || !strcmp(k, LV_SYMBOL_BACKSPACE) || !strcmp(k, LV_SYMBOL_NEW_LINE) ||
           !strcmp(k, LV_SYMBOL_KEYBOARD) || !strcmp(k, LV_SYMBOL_LEFT) || !strcmp(k, LV_SYMBOL_RIGHT) ||
           !strcmp(k, LV_SYMBOL_OK) || !strcmp(k, " ") || !strcmp(k, "\n");
}

static int failures = 0;
static void check(bool ok, const char* what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static void verify_map(const char* label, const char* const* m,
                       const lv_btnmatrix_ctrl_t* c, const char* lang_code) {
    printf("== %s\n", label);
    int row = 0, rows = 0, keys = 0, ctrl_i = 0;
    int letter_rows[3] = {0, 0, 0};
    bool lang_seen = false;
    for (const char* const* p = m; *p; ++p) {
        if (!strcmp(*p, "\n")) { ++row; continue; }
        const lv_btnmatrix_ctrl_t ctrl = c ? c[ctrl_i++] : 0;
        ++keys;
        if (row == 0) {
            // utility row; the language cell (index 2) must carry the active
            // layout's code when any secondary layout is enabled
            if (lang_code && !strcmp(*p, lang_code)) lang_seen = true;
            const char* u = *p;
            check(is_util(u) || (lang_code && !strcmp(u, lang_code)),
                  "row0 cell is a utility key");
            check((ctrl & 0x0F) >= 1 && (ctrl & 0x0F) <= 7, "utility width 1..7");
        } else if (row >= 1 && row <= 3) {
            // The language key rides at the end of the last letter row.
            const bool is_lang = lang_code && !strcmp(*p, lang_code);
            check(is_lang || !is_util(*p), "letter row holds only letters (plus the language key)");
            if (is_lang) lang_seen = true;
            if (!is_lang) ++letter_rows[row - 1];
        } else if (row == 4) {
            const bool is_lang = lang_code && !strcmp(*p, lang_code);
            if (is_lang) { check(true, "language key sits on the bottom row"); lang_seen = true; }
            else check(!strcmp(*p, LV_SYMBOL_KEYBOARD) || !strcmp(*p, " ") ||
                       !strcmp(*p, LV_SYMBOL_NEW_LINE) || !strcmp(*p, LV_SYMBOL_OK),
                       "row4 is [Sym][lang][space][NL][OK]");
        } else {
            check(false, "unexpected extra row");
        }
    }
    rows = row;
    check(rows == 4, "exactly 4 separators (5 rows)");
    check(lang_code ? lang_seen : true, "language cell shows the active layout code");
    char buf[96];
    snprintf(buf, sizeof buf, "letter rows 9-12/9-12/8-12 keys (got %d/%d/%d)",
             letter_rows[0], letter_rows[1], letter_rows[2]);
    check(letter_rows[0] >= 9 && letter_rows[0] <= 12 &&
          letter_rows[1] >= 9 && letter_rows[1] <= 12 &&
          letter_rows[2] >= 8 && letter_rows[2] <= 12, buf);
}

// First letter key of the given row (0-based among keys, separators skipped).
static const char* row_first_key(const char* const* m, int row) {
    int r = 0;
    for (const char* const* p = m; *p; ++p) {
        if (!strcmp(*p, "\n")) { ++r; continue; }
        if (r == row) return *p;
    }
    return "";
}

int main() {
    // A secondary layout enabled -> the language cell appears with its code.
    keyboardLayoutsSetEnabledMask(1u << static_cast<int>(KeyboardLayoutId::BG));
    for (int id = 0; id < KEYBOARD_LAYOUT_COUNT; ++id) {
        lv_obj_t kb{};
        const KeyboardLayoutId lid = static_cast<KeyboardLayoutId>(id);
        keyboardLayoutsApply(&kb, lid);
        verify_map(keyboardLayoutName(lid), cap_map[0], cap_ctrl[0], keyboardLayoutName(lid));
        verify_map("(upper)", cap_map[1], cap_ctrl[1], keyboardLayoutName(lid));
    }
    // Only-English mode -> no language cell anywhere in the deck.
    keyboardLayoutsSetEnabledMask(0);
    {
        lv_obj_t kb{};
        keyboardLayoutsApply(&kb, KeyboardLayoutId::EN);
        bool stray_lang = false;
        for (const char* const* p = cap_map[0]; *p; ++p)
            if (!strcmp(*p, "EN") || !strcmp(*p, "LANG")) stray_lang = true;
        for (const char* const* p = cap_map[1]; *p; ++p)
            if (!strcmp(*p, "EN") || !strcmp(*p, "LANG")) stray_lang = true;
        check(!stray_lang, "no language cell in only-English mode");
    }
    // QWERTY stagger: "a" opens row 2 and "z" opens row 3 (row 1 = utility row,
    // rows 2-4 = the letter deck's rows 1-3).
    {
        lv_obj_t kb{};
        keyboardLayoutsApply(&kb, KeyboardLayoutId::EN);
        check(!strcmp(row_first_key(cap_map[0], 1), "q"), "EN row 1 starts with q");
        check(!strcmp(row_first_key(cap_map[0], 2), "a"), "EN row 2 starts with a");
        check(!strcmp(row_first_key(cap_map[0], 3), "z"), "EN row 3 starts with z (_ - : dropped to the symbols page)");
        keyboardLayoutsApply(&kb, KeyboardLayoutId::BG);
        check(!strcmp(row_first_key(cap_map[0], 1), "\u044F"), "BG row 1 starts with я");
        check(!strcmp(row_first_key(cap_map[0], 2), "\u0430"), "BG row 2 starts with а");
        check(!strcmp(row_first_key(cap_map[0], 3), "\u0437"), "BG row 3 starts with з");
    }
    printf("\n%s (failures=%d)\n", failures ? "FAILED" : "ALL CHECKS PASSED", failures);
    return failures ? 1 : 0;
}
