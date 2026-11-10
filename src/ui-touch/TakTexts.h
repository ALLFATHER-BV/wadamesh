// SPDX-License-Identifier: GPL-3.0-or-later
// From jade-on-mesh's notes for Wadamesh (hiding-tak-control-texts.md); only caption() is ours.
// What a MeshCore Open TAK / Wadamesh TAK text is, for hiding or labelling it.
// `text`: a DM's text, or a channel text with its "<name>: " already skipped.
#pragma once
#include <string.h>
#include <initializer_list>

namespace taktext {

enum Kind { kNone, kControl, kPhoto, kVoice, kTalk, kEncrypted };

inline bool b36(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'); }
inline bool hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
inline bool starts(const char* t, const char* p) { return strncmp(t, p, strlen(p)) == 0; }

// <prefix><b36>:<b36>:<b36>:<b36>, then the end or a space.
inline bool envelope(const char* t, const char* prefix) {
  if (!starts(t, prefix)) return false;
  const char* p = t + strlen(prefix);
  for (int f = 0; f < 4; f++) {
    const char* s = p;
    while (b36(*p)) p++;
    if (p == s || p - s > 7) return false;
    if (f < 3) { if (*p != ':') return false; p++; }
  }
  return *p == '\0' || *p == ' ';
}

inline Kind classify(const char* t, bool is_dm) {
  if (!t) return kNone;
  // Control texts: always hide.
  if (starts(t, "VR3:") || starts(t, "VT1:")) return kControl;
  if (starts(t, "z3") && (t[2] == 's' || t[2] == 'S') && b36(t[3])) return kControl;   // channel pad
  if (starts(t, "z3") && (t[2] == 'p' || t[2] == 'P') && (t[3] == 'A' || t[3] == 'B')) return kControl;   // DM pad
  if (!strcmp(t, "z4Cz") || !strcmp(t, "z4Dz") || !strcmp(t, "z4Gz")) return kControl;
  if (!is_dm && (starts(t, "#TEL:") || starts(t, "#T:") || starts(t, "#CAP:") || starts(t, "#WAY:") ||
                 starts(t, "#WRC:")))
    return kControl;
  // Content: hide, or show a label.
  if (envelope(t, "VI1:")) return kPhoto;
  if (envelope(t, "VM1:")) return kVoice;
  if (envelope(t, "VE3:")) {
    const char* sp = strchr(t, ' ');
    return (sp && starts(sp + 1, "PTT") && (sp[4] == '\0' || sp[4] == ' ')) ? kTalk : kVoice;
  }
  if (!is_dm && starts(t, "z2") && b36(t[2])) {
    const char* p = t + 2;
    while (b36(*p)) p++;
    if (*p == '|' && hex(p[1])) return kEncrypted;
  }
  if (is_dm) {   // an OTP DM: all lowercase hex, even, at least 20 characters
    size_t n = 0;
    while (hex(t[n])) n++;
    if (t[n] == '\0' && n >= 20 && n % 2 == 0) return kEncrypted;
  }
  return kNone;
}

// The caption of a photo or voice envelope, "" if there is none: what follows the
// fields and the plain word (IMAGE, VOICE or PTT).
inline const char* caption(const char* t) {
  const char* p = strchr(t, ' ');
  if (!p) return "";
  p++;
  for (const char* word : {"IMAGE", "VOICE", "PTT"}) {
    const size_t n = strlen(word);
    if (starts(p, word) && (p[n] == '\0' || p[n] == ' ')) { p += n; break; }
  }
  while (*p == ' ') p++;
  return p;
}

}  // namespace taktext
