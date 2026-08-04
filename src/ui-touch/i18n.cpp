#include "i18n.h"
#include <string.h>

// Native language names shown in the picker. Index order == UiLang.
const char* const kUiLangNames[LANG_COUNT] = {
  "English", "Magyar","Nederlands", "Deutsch", "Français", "Español", "Italiano",
  "Русский", "Українська", "Български", "Српски", "Ελληνικά",
  "Português (BR)", "Română",
};

// File/catalog codes, index order == UiLang. Keep in sync with the enum AND with
// the deploy/apps/lang/*.lang filenames (the canonical translation source).
const char* const kUiLangCodes[LANG_COUNT] = {
  "en", "hu", "nl", "de", "fr", "es", "it",
  "ru", "uk", "bg", "sr", "el", "pt-br", "ro",
};

static uint8_t s_ui_lang = LANG_EN;
void    i18nSetLang(uint8_t l) { s_ui_lang = (l < LANG_COUNT) ? l : LANG_EN; }
uint8_t i18nGetLang() { return s_ui_lang; }

// File-language overlay: sorted key/translation pairs owned by the loader
// (UITask reads the .lang file into PSRAM at boot). Checked before the table.
static const I18nPair* s_file_pairs = nullptr;
static int             s_file_n     = 0;
void i18nSetFileOverlay(const I18nPair* pairs, int count) {
  s_file_pairs = (count > 0) ? pairs : nullptr;
  s_file_n     = (pairs && count > 0) ? count : 0;
}
bool i18nHasFileOverlay() { return s_file_n > 0; }

// The translation table used to live here, compiled into every image. It is now
// EXPORTED DATA: deploy/apps/lang/<code>.lang in the repo (canonical, translators
// PR those files) and served from firmware.wadamesh.com/apps/lang/. The device
// downloads the active language via the Lua Store's Languages tab and loads it at
// boot (uiLangFileBootLoad in UITask.cpp) — TR() below consults only that overlay.
// scripts/build/i18n-audit.py checks the files cover every TR() key in source.

const char* TR(const char* en) {
  if (!en) return "";
  if (!s_file_n) return en;   // no language file loaded: the keys ARE the English UI
  // Icon-prefixed labels ("<glyph>  Copy") carry the LVGL symbol's UTF-8 bytes
  // (3-byte private-use sequences, 0xEE/0xEF lead) in the lookup key, but the
  // table is keyed on the plain text — so those labels never matched and the
  // whole message menu / settings sheets silently stayed English. Split the
  // glyph+space prefix off, translate the text part, and rebuild the label in
  // a small static ring (lv_label_set_text and snprintf copy immediately, so
  // four slots cover any realistic single-label build).
  const char* txt = en;
  while (((uint8_t)txt[0] == 0xEE || (uint8_t)txt[0] == 0xEF) && txt[1] && txt[2])
    txt += 3;
  if (txt != en) { while (*txt == ' ') ++txt; }
  const size_t plen = (size_t)(txt - en);
  const char* base = txt[0] ? txt : en;   // an all-symbol string stays as-is
  const char* v = nullptr;
  {                                        // sorted pairs -> binary search
    int lo = 0, hi = s_file_n - 1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      const int c = strcmp(base, s_file_pairs[mid].key);
      if (c == 0) { v = s_file_pairs[mid].val; break; }
      if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
  }
  if (!v) return en;                       // untranslated: original, prefix intact
  if (plen == 0) return v;                 // plain key: the table cell directly
  static char ring[4][120];
  static uint8_t slot = 0;
  char* out = ring[slot];
  slot = (uint8_t)((slot + 1) & 3);
  const size_t pl = plen < 16 ? plen : 16; // prefix = one or two glyphs + spaces
  memcpy(out, en, pl);
  strncpy(out + pl, v, sizeof(ring[0]) - pl - 1);
  out[sizeof(ring[0]) - 1] = '\0';
  return out;
}







