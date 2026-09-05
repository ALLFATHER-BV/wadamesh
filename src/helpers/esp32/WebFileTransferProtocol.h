#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace WebFileTransferProtocol {

inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

inline uint32_t readLe32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

inline bool parseHex32(const char* text, uint32_t* value) {
  if (!text || !value || strlen(text) != 8) return false;
  uint32_t out = 0;
  for (int i = 0; i < 8; ++i) {
    const char c = text[i];
    uint8_t nibble;
    if (c >= '0' && c <= '9') nibble = static_cast<uint8_t>(c - '0');
    else if (c >= 'a' && c <= 'f') nibble = static_cast<uint8_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') nibble = static_cast<uint8_t>(c - 'A' + 10);
    else return false;
    out = (out << 4) | nibble;
  }
  *value = out;
  return true;
}

inline bool fileNameValid(const char* name) {
  if (!name || !name[0] || name[0] == '.' || strlen(name) > 64) return false;
  const size_t len = strlen(name);
  if (len >= 5 && strcmp(name + len - 5, ".part") == 0) return false;
  for (size_t i = 0; i < len; ++i) {
    const char c = name[i];
    const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!valid) return false;
  }
  return true;
}

inline bool readablePath(const char* path) {
  if (!path) return false;
  const char* leaf = nullptr;
  if (strncmp(path, "/screenshots/", 13) == 0) leaf = path + 13;
  else if (strncmp(path, "/transfer/", 10) == 0) leaf = path + 10;
  else return false;
  return fileNameValid(leaf) && strchr(leaf, '/') == nullptr;
}

}  // namespace WebFileTransferProtocol