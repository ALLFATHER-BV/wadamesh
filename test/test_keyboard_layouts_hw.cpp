// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for #747 (256b6b79): the physical-keyboard maps of the Italian, Spanish and
// Romanian layouts keep the digits. Compiles the production KeyboardLayouts.cpp (LVGL
// names from test/kb_layouts_fakes) and the T-Deck matrix decoder, and reads the accent
// box's tables out of UITask.cpp to show every letter the digit rows used to give is
// still reachable from its base letter. Run from the repo root:
//
//   c++ -std=c++17 -Wall -Wextra -I src -I test/kb_layouts_fakes test/test_keyboard_layouts_hw.cpp \
//       -o /tmp/test_keyboard_layouts_hw && /tmp/test_keyboard_layouts_hw
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "../src/ui-touch/KeyboardLayouts.cpp"
#include "helpers/input/TDeckKeyboardState.h"

using L = KeyboardLayoutId;

// What the T-Deck types for Sym + one key (Sym tapped, then the key), through the real
// matrix decoder.
static char symPlus(int col, int row) {
  TDeckKeyboardState kb;
  uint8_t st[5] = {}, out[8];
  uint32_t t = 100;
  kb.update(st, t += 10, out, 8);
  st[0] |= 1u << 2; kb.update(st, t += 10, out, 8);         // Sym down
  st[0] &= ~(1u << 2); kb.update(st, t += 10, out, 8);      // Sym up (one-shot)
  st[col] |= (uint8_t)(1u << row);
  const size_t n = kb.update(st, t += 10, out, 8);
  assert(n == 1);
  return (char)out[0];
}

// The text handleHwKey() inserts for one key: the layout's mapping, else the key itself.
static std::string typed(L id, int key) {
  const bool shifted = key >= 'A' && key <= 'Z';
  const char* m = keyboardLayoutMapHwKey(id, key, shifted);
  return m ? std::string(m) : std::string(1, (char)key);
}

// kAccX[] = {"à","á",...} arrays and the kAccentSets[] rows, read from UITask.cpp.
static std::map<std::string, std::string> accentArrays() {
  std::ifstream f("src/ui-touch/UITask.cpp");
  assert(f && "run from the repo root");
  std::map<std::string, std::string> out;
  for (std::string l; std::getline(f, l);) {
    const size_t a = l.find("static const char* const kAcc");
    if (a == std::string::npos) continue;
    const size_t n0 = l.find("kAcc", a), n1 = l.find("[]", n0);
    out[l.substr(n0, n1 - n0)] = l.substr(l.find('{'));
  }
  return out;
}
static std::string accentSetsRow() {
  std::ifstream f("src/ui-touch/UITask.cpp");
  std::stringstream ss; ss << f.rdbuf();
  const std::string s = ss.str();
  const size_t a = s.find("static const AccentSet kAccentSets[] = {");
  assert(a != std::string::npos);
  return s.substr(a, s.find("};", a) - a);
}
// `letter` is offered by the accent box after typing `base`.
static bool reachable(const std::map<std::string, std::string>& arr, const std::string& sets,
                      char base, const char* letter) {
  for (const auto& kv : arr) {
    if (kv.second.find(std::string("\"") + letter + "\"") == std::string::npos) continue;
    char row[32];
    snprintf(row, sizeof row, "{'%c',%s,", base, kv.first.c_str());
    if (sets.find(row) != std::string::npos) return true;
  }
  return false;
}

int main() {
  // ---- the T-Deck's digits are the Sym layer's: Sym+W/E/R = 1 2 3 ----
  const char d1 = symPlus(0, 1), d2 = symPlus(1, 0), d3 = symPlus(2, 0);
  assert(d1 == '1' && d2 == '2' && d3 == '3');
  // ...and Sym+Q / Sym+B... give # and the shifted-symbol row the fallback used to catch
  const char hash = symPlus(0, 0);
  assert(hash == '#');

  // ---- Italian, Spanish, Romanian: the digits and ! @ # $ % type as themselves ----
  for (L id : {L::IT, L::ES, L::RO}) {
    for (char k : {d1, d2, d3}) assert(typed(id, k) == std::string(1, k));
    for (int k = '0'; k <= '9'; ++k) {
      assert(keyboardLayoutMapHwKey(id, k, false) == nullptr);
      assert(keyboardLayoutMapHwKey(id, k, true) == nullptr);
    }
    for (char k : std::string(")!@#$%^&*(")) assert(typed(id, k) == std::string(1, k));
    // letters are still plain Latin
    assert(typed(id, 'a') == "a" && typed(id, 'N') == "N");
  }

  // ---- the letters the digit rows used to give come from the accent box ----
  const auto arr = accentArrays();
  const std::string sets = accentSetsRow();
  assert(arr.size() >= 20);
  struct { char base; const char* letter; } need[] = {
    {'a', "à"}, {'e', "è"}, {'i', "ì"}, {'A', "À"}, {'E', "È"}, {'I', "Ì"},    // Italian
    {'n', "ñ"}, {'N', "Ñ"},                                                     // Spanish
    {'a', "ă"}, {'a', "â"}, {'i', "î"}, {'s', "ş"}, {'t', "ţ"},                 // Romanian
    {'A', "Ă"}, {'A', "Â"}, {'I', "Î"}, {'S', "Ş"}, {'T', "Ţ"},
    {'o', "ò"}, {'u', "ù"}, {'e', "é"},                                         // the rest of Italian
  };
  for (const auto& n : need) {
    if (!reachable(arr, sets, n.base, n.letter)) {
      fprintf(stderr, "'%s' is not offered after '%c'\n", n.letter, n.base);
      assert(false);
    }
  }
  // The accent box is on by default (s_accent_popups = true).
  {
    std::ifstream f("src/ui-touch/UITask.cpp");
    std::stringstream ss; ss << f.rdbuf();
    assert(ss.str().find("static bool      s_accent_popups     = true;") != std::string::npos);
  }

  // ---- Latvian and the non-Latin layouts keep their digit rows ----
  assert(typed(L::LV, '1') == "ā" && typed(L::LV, '2') == "ē" && typed(L::LV, '8') == "ū");
  assert(!strcmp(keyboardLayoutMapHwKey(L::LV, '1', true), "Ā"));
  assert(typed(L::LV, '!') == "Ā");   // the shifted-symbol fallback, as before
  assert(typed(L::LV, '0') == "0" && typed(L::LV, '9') == "9");
  assert(typed(L::RU, 'a') == "а");   // Cyrillic a
  assert(typed(L::BG, 'a') == "а");
  assert(typed(L::EN, '1') == "1" && typed(L::EN, 'a') == "a");

  printf("test_keyboard_layouts_hw: ok\n");
  return 0;
}
