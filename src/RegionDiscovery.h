#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

class RegionDiscoveryResults {
public:
  static constexpr uint8_t MAX_RESULTS = 14;
  static constexpr size_t MAX_PUBLIC_NAME_BYTES = 29;

  void clear() {
    _count = 0;
    memset(_names, 0, sizeof(_names));
  }

  uint8_t count() const { return _count; }
  const char* name(uint8_t index) const {
    return index < _count ? _names[index] : nullptr;
  }

  static bool validPublicScope(const char* text, bool allow_empty = true) {
    if (!text) return false;
    size_t begin = 0, end = strlen(text);
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' ||
                           text[begin] == '\r' || text[begin] == '\n')) ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' ||
                           text[end - 1] == '\r' || text[end - 1] == '\n')) --end;
    if (begin == end) return allow_empty;
    if (text[begin] == '#') ++begin;
    if (begin == end || text[begin] == '$' || text[begin] == '*' ||
        end - begin > MAX_PUBLIC_NAME_BYTES) return false;
    for (size_t i = begin; i < end; ++i) {
      const char c = text[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
  }

  bool add(const uint8_t* data, size_t length) {
    if (!data || length == 0) return false;
    while (length && (*data == ' ' || *data == '\t' || *data == '\r' || *data == '\n')) {
      ++data;
      --length;
    }
    while (length && (data[length - 1] == ' ' || data[length - 1] == '\t' ||
                      data[length - 1] == '\r' || data[length - 1] == '\n')) {
      --length;
    }
    if (length && data[0] == '#') {
      ++data;
      --length;
    }
    if (length == 0 || length > MAX_PUBLIC_NAME_BYTES || data[0] == '*' || data[0] == '$')
      return false;
    for (size_t i = 0; i < length; ++i) {
      const uint8_t c = data[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }

    char canonical[MAX_PUBLIC_NAME_BYTES + 2];
    canonical[0] = '#';
    memcpy(&canonical[1], data, length);
    canonical[length + 1] = '\0';
    for (uint8_t i = 0; i < _count; ++i)
      if (strcmp(_names[i], canonical) == 0) return false;
    if (_count >= MAX_RESULTS) return false;
    memcpy(_names[_count++], canonical, length + 2);
    return true;
  }

  uint8_t addCsv(const uint8_t* data, size_t length) {
    if (!data || length == 0) return 0;
    const uint8_t before = _count;
    size_t start = 0;
    for (size_t i = 0; i <= length; ++i) {
      if (i == length || data[i] == ',') {
        add(data + start, i - start);
        start = i + 1;
      }
    }
    return (uint8_t)(_count - before);
  }

  /** MeshCore REGIONS response: reflected request tag (4), repeater clock (4),
   *  then comma-separated names without a required trailing NUL. */
  uint8_t addResponse(const uint8_t* data, size_t length) {
    return data && length > 8 ? addCsv(data + 8, length - 8) : 0;
  }

private:
  char _names[MAX_RESULTS][MAX_PUBLIC_NAME_BYTES + 2] = {};
  uint8_t _count = 0;
};