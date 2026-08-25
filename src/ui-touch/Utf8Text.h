// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace Utf8Text {

inline bool continuation(uint8_t byte) {
  return (byte & 0xC0U) == 0x80U;
}

inline size_t sequenceLength(const uint8_t* text, size_t remaining) {
  if (!remaining) return 0;
  const uint8_t first = text[0];
  if (first <= 0x7F) return 1;
  if (first >= 0xC2 && first <= 0xDF) {
    return remaining >= 2 && continuation(text[1]) ? 2 : 0;
  }
  if (first == 0xE0) {
    return remaining >= 3 && text[1] >= 0xA0 && text[1] <= 0xBF && continuation(text[2]) ? 3 : 0;
  }
  if ((first >= 0xE1 && first <= 0xEC) || (first >= 0xEE && first <= 0xEF)) {
    return remaining >= 3 && continuation(text[1]) && continuation(text[2]) ? 3 : 0;
  }
  if (first == 0xED) {
    return remaining >= 3 && text[1] >= 0x80 && text[1] <= 0x9F && continuation(text[2]) ? 3 : 0;
  }
  if (first == 0xF0) {
    return remaining >= 4 && text[1] >= 0x90 && text[1] <= 0xBF &&
           continuation(text[2]) && continuation(text[3]) ? 4 : 0;
  }
  if (first >= 0xF1 && first <= 0xF3) {
    return remaining >= 4 && continuation(text[1]) && continuation(text[2]) && continuation(text[3]) ? 4 : 0;
  }
  if (first == 0xF4) {
    return remaining >= 4 && text[1] >= 0x80 && text[1] <= 0x8F &&
           continuation(text[2]) && continuation(text[3]) ? 4 : 0;
  }
  return 0;
}

inline bool valid(const char* text, size_t length) {
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
  size_t offset = 0;
  while (offset < length) {
    const size_t sequence = sequenceLength(bytes + offset, length - offset);
    if (!sequence) return false;
    offset += sequence;
  }
  return true;
}

// Returns the original pointer when it is already valid. Malformed runs are
// collapsed to one ASCII '?' in scratch so LVGL never receives a partial codepoint.
inline const char* sanitize(const char* text, size_t length, std::string& scratch) {
  scratch.clear();
  if (valid(text, length)) return text;

  scratch.reserve(length);
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(text);
  size_t offset = 0;
  while (offset < length) {
    const size_t sequence = sequenceLength(bytes + offset, length - offset);
    if (sequence) {
      scratch.append(text + offset, sequence);
      offset += sequence;
      continue;
    }
    scratch.push_back('?');
    ++offset;
    while (offset < length && continuation(bytes[offset])) ++offset;
  }
  return scratch.c_str();
}

}  // namespace Utf8Text