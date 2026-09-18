// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Windows malware carriers on a removable card.
//
// Written for the ThinkNode M9 SD-card worm (Elecrow security advisory,
// September 2026): cards shipped with a hidden autorun.inf and Windows programs
// disguised as shortcuts, dormant until the card goes into a Windows PC. A mesh
// radio never reads any of these files, so on this card a match is malware or,
// at best, something no device workflow put there.
//
// This is the ONLY policy wada.sd.remove() honours. A store app may ask for a
// file to be deleted, but the firmware decides whether it is a threat, so no app
// can use that call to delete map tiles, backups, chat history or an identity.
namespace SdThreat {

enum Kind : uint8_t {
  None = 0,
  Autorun,          // autorun.inf: tells Windows what to launch from the card
  Program,          // .exe, .scr, .com, ...: runs when double-clicked
  Script,           // .vbs, .js, .bat, ...: run by Windows' own script hosts
  Shortcut,         // .lnk, .url, .scf: how the worm disguises itself as a folder
  RenamedProgram,   // a Windows program under a harmless-looking name
};

inline const char* label(Kind k) {
  switch (k) {
    case Autorun:        return "autorun file";
    case Program:        return "Windows program";
    case Script:         return "Windows script";
    case Shortcut:       return "Windows shortcut";
    case RenamedProgram: return "renamed Windows program";
    default:             return nullptr;
  }
}

inline bool eqNoCase(const char* a, const char* b) {
  for (;; ++a, ++b) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
    if (!x) return true;
  }
}

// By name alone. `name` is the last path component. Only the final extension
// counts, which is also how Windows decides what a double-click runs, so the
// "photo.jpg.exe" and right-to-left-override tricks are caught by the ".exe".
inline Kind byName(const char* name) {
  if (!name || !name[0]) return None;
  if (eqNoCase(name, "autorun.inf")) return Autorun;
  const char* dot = strrchr(name, '.');
  if (!dot || !dot[1]) return None;
  const char* ext = dot + 1;
  static const char* const kProgram[] = {
    "exe", "scr", "com", "pif", "cpl", "msi", "msp", "dll", "jar",
  };
  static const char* const kScript[] = {
    "bat", "cmd", "vbs", "vbe", "js", "jse", "wsf", "wsh", "hta", "ps1", "reg", "msc",
  };
  static const char* const kShortcut[] = { "lnk", "url", "scf" };
  for (const char* e : kProgram)  if (eqNoCase(ext, e)) return Program;
  for (const char* e : kScript)   if (eqNoCase(ext, e)) return Script;
  for (const char* e : kShortcut) if (eqNoCase(ext, e)) return Shortcut;
  return None;
}

// By content: a Windows program is "MZ" at offset 0 plus a "PE\0\0" signature
// where the DOS header's e_lfanew (a 32-bit little-endian value at 0x3C) points.
// Two bytes of "MZ" alone are not enough: a random binary blob starts with them
// once in 65,536 files, and on this card that blob could be chat history.
constexpr size_t kHeadBytes = 64;

inline bool mzHeader(const uint8_t* head, size_t n) {
  return head && n >= kHeadBytes && head[0] == 'M' && head[1] == 'Z';
}

inline uint32_t peOffset(const uint8_t* head) {
  return (uint32_t)head[0x3C] | ((uint32_t)head[0x3D] << 8) |
         ((uint32_t)head[0x3E] << 16) | ((uint32_t)head[0x3F] << 24);
}

// Real linkers put the PE header within the first few hundred bytes; bound it so
// a corrupt or hostile e_lfanew cannot send the check seeking across the card.
inline bool peOffsetPlausible(uint32_t off, uint32_t file_size) {
  return off >= kHeadBytes && off <= 65536u && file_size >= 4 && off <= file_size - 4;
}

inline bool peSignature(const uint8_t* sig, size_t n) {
  return sig && n >= 4 && sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0;
}

}  // namespace SdThreat
