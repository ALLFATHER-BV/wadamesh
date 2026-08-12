#include "i18n.h"
#include <string.h>
#include "device_caps.h"
#if CAP_BUILTIN_LANGS
#include "i18n_builtin.h"   // generated: baked-in translations
#endif

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

// Advance *p to just past the next printf conversion and write its type
// signature (length modifier + conversion char) into out. "%%" is a literal
// percent, not a conversion. Returns false when there are no more.
static bool i18nNextSpec(const char** p, char* out, size_t cap) {
  const char* s = *p;
  for (;;) {
    while (*s && *s != '%') ++s;
    if (!*s) { *p = s; return false; }
    ++s;                                   // past '%'
    if (*s == '%') { ++s; continue; }       // literal '%%' — keep scanning
    if (!*s) { *p = s; return false; }      // trailing '%': no conversion
    size_t n = 0;
    while (*s == '-' || *s == '+' || *s == ' ' || *s == '#' || *s == '0') ++s;
    if (*s == '*') { ++s; if (n + 1 < cap) out[n++] = '*'; }   // width from a vararg
    else while (*s >= '0' && *s <= '9') ++s;
    if (*s == '.') {
      ++s;
      if (*s == '*') { ++s; if (n + 1 < cap) out[n++] = '*'; } // precision vararg
      else while (*s >= '0' && *s <= '9') ++s;
    }
    while (*s == 'h' || *s == 'l' || *s == 'j' || *s == 'z' || *s == 't' || *s == 'L') {
      if (n + 1 < cap) out[n++] = *s;
      ++s;
    }
    if (!*s) { *p = s; return false; }      // truncated spec: nothing to compare
    if (n + 1 < cap) out[n++] = *s;
    ++s;
    out[n < cap ? n : cap - 1] = '\0';
    *p = s;
    return true;
  }
}

// ---- Format-string safety (issue #258) -------------------------------------
// Callers pass TR()'s result straight to snprintf as the FORMAT string, so a
// translation is attacker-shaped input: it decides how many varargs are read
// and how each is typed. A Hungarian row reordered "…%s… %lu…" to "…%lu… %s…",
// which made snprintf read the caller's minute COUNT as a char* and dereference
// address 3 — an instant reboot whenever a Hungarian user logged into a
// repeater. Translations arrive from .lang files users download or hand-write,
// so no amount of fixing the shipped files makes this safe; the runtime has to
// refuse a translation whose conversions do not match the key's.
//
// Compares the ordered sequence of (length-modifier, conversion) pairs, which
// is exactly what determines each vararg's type. '*' width/precision counts as
// its own int argument. On any mismatch the caller gets the English key, which
// is always in step with the call site because the key IS the format string.
static bool i18nFmtSpecsMatch(const char* a, const char* b) {
  const char* pa = a; const char* pb = b;
  for (;;) {
    char sa[8], sb[8];
    const bool ha = i18nNextSpec(&pa, sa, sizeof sa);
    const bool hb = i18nNextSpec(&pb, sb, sizeof sb);
    if (ha != hb) return false;          // one ran out of conversions first
    if (!ha) return true;                // both exhausted, all pairs matched
    if (strcmp(sa, sb) != 0) return false;
  }
}

const char* TR(const char* en) {
  if (!en) return "";
#if CAP_BUILTIN_LANGS
  if (!s_file_n && s_ui_lang == LANG_EN) return en;
#else
  if (!s_file_n) return en;   // no language file loaded: the keys ARE the English UI
#endif
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
#if CAP_BUILTIN_LANGS
  if (!v && s_ui_lang != LANG_EN && kBuiltinLang[s_ui_lang]) {
    const I18nPair* tab = kBuiltinLang[s_ui_lang];
    int lo = 0, hi = kBuiltinLangCount[s_ui_lang] - 1;
    while (lo <= hi) {                     // sorted by the generator
      const int mid = (lo + hi) / 2;
      const int c = strcmp(base, tab[mid].key);
      if (c == 0) { v = tab[mid].val; break; }
      if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
  }
#endif
  if (!v) return en;                       // untranslated: original, prefix intact
  // A translated FORMAT string must read the same varargs as the key, or the
  // caller's snprintf misreads the stack (#258). Only pay for the scan when the
  // key actually has conversions — most UI strings have none.
  if (strchr(base, '%') && !i18nFmtSpecsMatch(base, v)) return en;
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







