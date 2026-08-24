#pragma once
#include <stdint.h>

// UI language support for the touch UI. Gettext-style: wrap an English literal
// in TR("...") and it returns the active language's translation (or the English
// itself when untranslated). The fonts (g_font_12/14/16 + their extras_* fallback)
// already carry Latin / Cyrillic / Greek / Arabic glyphs, so non-Latin renders.
//
// Keep this enum in sync with kUiLangNames + the table column order in i18n.cpp.
// ⚠️ APPEND new languages before LANG_COUNT only — the index is persisted as
// ui_lang, so inserting mid-enum shifts every stored choice (the v45 migration
// papers over the one HU insert; do not add more remaps).
enum UiLang : uint8_t {
  LANG_EN = 0, LANG_HU, LANG_NL, LANG_DE, LANG_FR, LANG_ES, LANG_IT,
  LANG_RU, LANG_UK, LANG_BG, LANG_SR, LANG_EL, LANG_PT_BR, LANG_RO, LANG_COUNT
};

// Native names for the language picker (e.g. "Nederlands", "Русский", "Ελληνικά").
extern const char* const kUiLangNames[LANG_COUNT];
// Short file/catalog codes, same order ("en","hu","nl",... "pt-br","ro"). Used as
// the .lang filename and the `# base:` fallback column in language files.
extern const char* const kUiLangCodes[LANG_COUNT];

// ---- File-language overlay (Lua Store -> Languages) ----
// A .lang file (one `EN-key<TAB>translation` per line, \n/\t/\\ escaped) can be
// loaded at boot and consulted BEFORE the built-in table; ui_lang stays active as
// the fallback column, so a partial file degrades to the built-in translation,
// then to English. The caller owns the pair array + backing strings (PSRAM);
// pairs MUST be sorted by strcmp(key) — TR() binary-searches them.
struct I18nPair { const char* key; const char* val; };
void i18nSetFileOverlay(const I18nPair* pairs, int count);
bool i18nHasFileOverlay();

void    i18nSetLang(uint8_t lang);   // clamps an out-of-range value to English
uint8_t i18nGetLang();

// Translate an English UI string to the active language. Returns `en` itself when
// there is no translation (so untranslated strings show English, never blank).
// Keyed by the English source — just wrap literals: TR("Save").
const char* TR(const char* en);
