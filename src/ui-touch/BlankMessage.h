// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A draft made only of spaces (or tabs / newlines) is not a message. The composer
// used to refuse only an EMPTY draft, so a stray space from the keyboard went out
// as a real post, and every radio on the channel showed "Name:" over a blank
// bubble. Treat whitespace-only like empty: nothing is sent and the draft stays.
namespace BlankMessage {

inline bool isBlank(const char* text) {
  if (!text) return true;
  for (const char* p = text; *p; ++p) {
    if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') return false;
  }
  return true;
}

}  // namespace BlankMessage
