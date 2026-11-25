// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ChatContactTuple {

constexpr size_t PUBLIC_KEY_HEX_LENGTH = 64;
constexpr size_t CONTACT_NAME_CAPACITY = 32;

struct Contact {
  uint8_t public_key[32];
  uint8_t type;
  char public_key_hex[PUBLIC_KEY_HEX_LENGTH + 1];
  char name[CONTACT_NAME_CAPACITY];
  size_t end;
};

inline int hexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return -1;
}

inline bool parseAt(const char* text, size_t length, size_t start, Contact* out) {
  if (!text || !out || start >= length || text[start] != '<') return false;
  const size_t key_start = start + 1;
  if (length - key_start < PUBLIC_KEY_HEX_LENGTH + 5) return false;

  Contact parsed{};
  for (size_t i = 0; i < PUBLIC_KEY_HEX_LENGTH; ++i) {
    const int value = hexValue(text[key_start + i]);
    if (value < 0) return false;
    parsed.public_key_hex[i] = text[key_start + i];
    if ((i & 1u) == 0) parsed.public_key[i / 2] = static_cast<uint8_t>(value << 4);
    else parsed.public_key[i / 2] |= static_cast<uint8_t>(value);
  }
  parsed.public_key_hex[PUBLIC_KEY_HEX_LENGTH] = '\0';

  const size_t type_colon = key_start + PUBLIC_KEY_HEX_LENGTH;
  if (text[type_colon] != ':' || text[type_colon + 2] != ':') return false;
  const char type_char = text[type_colon + 1];
  if (type_char < '1' || type_char > '4') return false;
  parsed.type = static_cast<uint8_t>(type_char - '0');

  const size_t name_start = type_colon + 3;
  size_t name_end = name_start;
  while (name_end < length && text[name_end] != '>') {
    if (text[name_end] == '<' || name_end - name_start >= CONTACT_NAME_CAPACITY - 1)
      return false;
    ++name_end;
  }
  if (name_end == length || name_end == name_start) return false;
  const size_t name_length = name_end - name_start;
  memcpy(parsed.name, text + name_start, name_length);
  parsed.name[name_length] = '\0';
  parsed.end = name_end + 1;
  *out = parsed;
  return true;
}

inline bool span(const char* text, size_t length, size_t from, size_t* start,
                 Contact* out) {
  if (!text || !start || !out || from >= length) return false;
  for (size_t i = from; i < length; ++i) {
    if (text[i] != '<') continue;
    Contact parsed{};
    if (!parseAt(text, length, i, &parsed)) continue;
    *start = i;
    *out = parsed;
    return true;
  }
  return false;
}

inline size_t extractAndRemove(char* text, Contact* contacts = nullptr,
                               size_t contacts_capacity = 0) {
  if (!text) return 0;
  size_t length = strlen(text);
  size_t from = 0;
  size_t replaced = 0;
  size_t start = 0;
  Contact contact{};
  while (span(text, length, from, &start, &contact)) {
    if (contacts && replaced < contacts_capacity) contacts[replaced] = contact;
    size_t erase_start = start;
    size_t erase_end = contact.end;
    const auto is_space = [](char c) {
      return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    if (erase_start > 0 && is_space(text[erase_start - 1])) {
      --erase_start;
    } else if (erase_end < length && is_space(text[erase_end])) {
      ++erase_end;
    }
    memmove(text + erase_start, text + erase_end, length - erase_end + 1);
    length -= erase_end - erase_start;
    from = erase_start;
    ++replaced;
  }
  while (length > 0 && (text[0] == ' ' || text[0] == '\t' ||
                        text[0] == '\r' || text[0] == '\n')) {
    memmove(text, text + 1, length);
    --length;
  }
  while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                        text[length - 1] == '\r' || text[length - 1] == '\n')) {
    text[--length] = '\0';
  }
  return replaced;
}

}  // namespace ChatContactTuple
