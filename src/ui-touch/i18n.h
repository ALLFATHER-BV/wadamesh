#pragma once
#include <stdint.h>

// UI language support for the touch UI. Gettext-style: wrap an English literal
// in TR("...") and it returns the active language's translation (or the English
// itself when untranslated). The fonts (g_font_12/14/16 + their extras_* fallback)
// already carry Latin / Cyrillic / Greek / Arabic glyphs, so non-Latin renders.
//
// Keep this enum in sync with kUiLangNames + the table column order in i18n.cpp.
enum UiLang : uint8_t {
  LANG_EN = 0, LANG_NL, LANG_DE, LANG_FR, LANG_ES, LANG_IT,
  LANG_RU, LANG_UK, LANG_BG, LANG_SR, LANG_EL, LANG_COUNT
};

// Native names for the language picker (e.g. "Nederlands", "Русский", "Ελληνικά").
extern const char* const kUiLangNames[LANG_COUNT];

void    i18nSetLang(uint8_t lang);   // clamps an out-of-range value to English
uint8_t i18nGetLang();

// Translate an English UI string to the active language. Returns `en` itself when
// there is no translation (so untranslated strings show English, never blank).
// Keyed by the English source — just wrap literals: TR("Save").
const char* TR(const char* en);
