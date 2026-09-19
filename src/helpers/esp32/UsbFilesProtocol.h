#pragma once
// =============================================================================
// USB Files: the wire protocol between files.wadamesh.com (Web Serial) and the
// firmware's USB Files app. Pure C++ with no Arduino dependency, so the host
// tests (test/test_usb_files_protocol.cpp) build it as-is.
//
// Frame, both directions:
//
//   E7 5A | type u8 | seq u16 LE | len u16 LE | payload[len] | crc32 u32 LE
//
// crc32 is the IEEE one (zip, PNG) over type..payload. The device keeps
// printing log lines on the same port, so the parser hunts for the magic and
// resyncs after any damage. E7 5A never occurs in valid UTF-8 text: E7 is the
// lead byte of a three-byte sequence and 5A is not a continuation byte.
//
// The link is strictly one request at a time. Both Arduino USB serial drivers
// drop bytes once their receive queue is full (neither pushes back on the
// host), so the host waits for each reply before sending the next request and
// the device sizes its queue to hold one whole frame.
//
// Paths are virtual: "/" lists the roots, "/sd/...", "/internal/..." and
// "/tiles/..." address the SD card, internal flash and the map tile partition.
// =============================================================================

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "WebFileTransferProtocol.h"   // crc32Update

namespace UsbFiles {

constexpr uint8_t kMagic0 = 0xE7;
constexpr uint8_t kMagic1 = 0x5A;
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderBytes = 7;
constexpr size_t kTrailerBytes = 4;
constexpr size_t kMaxChunk = 4096;                 // file bytes per READ/WRITE_DATA
constexpr size_t kMaxPayload = kMaxChunk + 256;    // chunk + offset/status + a path
constexpr size_t kMaxFrame = kHeaderBytes + kMaxPayload + kTrailerBytes;
constexpr size_t kMaxPath = 200;                   // whole virtual path, bytes

enum Type : uint8_t {
  T_HELLO = 0x01,        // -> JSON: device, roots, chunk size
  T_LIST = 0x02,         // u32 start, u16 max, path -> JSON page of entries
  T_STAT = 0x03,         // path -> JSON {d,s}
  T_READ = 0x04,         // u32 offset, u16 len, path -> u32 offset + bytes
  T_WRITE_BEGIN = 0x05,  // u32 size, u8 flags, path
  T_WRITE_DATA = 0x06,   // u32 offset + bytes -> u32 next offset
  T_WRITE_END = 0x07,    // u32 crc32 of the whole file
  T_REMOVE = 0x08,       // path (file or empty folder)
  T_MKDIR = 0x09,        // path
  T_RENAME = 0x0A,       // u16 from_len, from, to
  T_SPACE = 0x0B,        // root path -> JSON {t,u}
  T_ABORT = 0x0C,        // drop an unfinished upload
  T_PING = 0x0D,         // keepalive while the page is idle
  T_BEACON = 0x40,       // device -> host, unsolicited: "USB Files is open"
  T_BYE = 0x41,          // device -> host, unsolicited: "USB Files closed"
  T_REPLY = 0x80,        // reply type = request type | T_REPLY, same seq
};

constexpr uint8_t kWriteOverwrite = 0x01;   // T_WRITE_BEGIN flags

// Every reply payload starts with one status byte. Errors may carry a short
// UTF-8 message after it.
enum Status : uint8_t {
  S_OK = 0,
  S_BAD_REQUEST = 1,
  S_NOT_FOUND = 2,
  S_EXISTS = 3,
  S_PROTECTED = 4,
  S_NO_SPACE = 5,
  S_IO = 6,
  S_BAD_PATH = 7,
  S_BUSY = 8,
  S_TOO_BIG = 9,
  S_BAD_CRC = 10,
  S_BAD_OFFSET = 11,
  S_UNSUPPORTED = 12,
  S_NO_MEDIA = 13,
  S_NOT_EMPTY = 14,
};

inline void putLe16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void putLe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
inline uint16_t getLe16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8));
}
inline uint32_t getLe32(const uint8_t* p) { return WebFileTransferProtocol::readLe32(p); }

inline uint32_t crc32(const uint8_t* data, size_t len) {
  return WebFileTransferProtocol::crc32Update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

// Encode one frame. Returns the byte count, or 0 if it does not fit or the
// payload is too long for the protocol.
inline size_t encodeFrame(uint8_t* out, size_t cap, uint8_t type, uint16_t seq,
                          const uint8_t* payload, size_t len) {
  if (!out || len > kMaxPayload || (len && !payload)) return 0;
  const size_t total = kHeaderBytes + len + kTrailerBytes;
  if (cap < total) return 0;
  out[0] = kMagic0;
  out[1] = kMagic1;
  out[2] = type;
  putLe16(out + 3, seq);
  putLe16(out + 5, static_cast<uint16_t>(len));
  if (len) memmove(out + kHeaderBytes, payload, len);
  putLe32(out + kHeaderBytes + len, crc32(out + 2, 5 + len));
  return total;
}

// Streaming frame extractor over a caller-owned buffer. push() appends bytes
// read from the port; next() returns true once a complete frame with a valid
// CRC sits at the front, exposed through type()/seq()/payload()/length() until
// the following next(). Noise before a frame is skipped. A frame that fails its
// CRC, or claims an impossible length, costs one byte and the scan restarts
// there, so a real frame that begins inside a damaged one is still found.
//
// Give it at least 2 * kMaxFrame so a retransmitted frame always fits behind a
// damaged one that claimed the full length.
class FrameParser {
 public:
  FrameParser(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

  void reset() { n_ = 0; consumed_ = 0; len_ = 0; }
  size_t space() const { return cap_ - n_; }

  size_t push(const uint8_t* data, size_t len) {
    compact();
    if (len > cap_ - n_) len = cap_ - n_;
    if (len) memcpy(buf_ + n_, data, len);
    n_ += len;
    return len;
  }

  bool next() {
    compact();
    for (;;) {
      size_t i = 0;
      while (i + 1 < n_ && !(buf_[i] == kMagic0 && buf_[i + 1] == kMagic1)) ++i;
      if (i + 1 >= n_) {
        // No complete magic yet. Keep a trailing E7: it may be half of one.
        const size_t keep = (n_ > 0 && buf_[n_ - 1] == kMagic0) ? 1 : 0;
        dropped_ += n_ - keep;
        if (keep) buf_[0] = kMagic0;
        n_ = keep;
        return false;
      }
      if (i) drop(i);
      if (n_ < kHeaderBytes) return false;
      const size_t len = getLe16(buf_ + 5);
      if (len > kMaxPayload) { drop(1); ++bad_; continue; }
      const size_t total = kHeaderBytes + len + kTrailerBytes;
      if (total > cap_) { drop(1); ++bad_; continue; }
      if (n_ < total) return false;
      if (crc32(buf_ + 2, 5 + len) != getLe32(buf_ + kHeaderBytes + len)) {
        drop(1);
        ++bad_;
        continue;
      }
      len_ = len;
      consumed_ = total;
      return true;
    }
  }

  uint8_t type() const { return buf_[2]; }
  uint16_t seq() const { return getLe16(buf_ + 3); }
  const uint8_t* payload() const { return buf_ + kHeaderBytes; }
  size_t length() const { return len_; }
  uint32_t droppedBytes() const { return dropped_; }
  uint32_t badFrames() const { return bad_; }

 private:
  void drop(size_t k) {
    memmove(buf_, buf_ + k, n_ - k);
    n_ -= k;
    dropped_ += static_cast<uint32_t>(k);
  }
  void compact() {
    if (!consumed_) return;
    memmove(buf_, buf_ + consumed_, n_ - consumed_);
    n_ -= consumed_;
    consumed_ = 0;
    len_ = 0;
  }

  uint8_t* buf_;
  size_t cap_;
  size_t n_ = 0;
  size_t consumed_ = 0;
  size_t len_ = 0;
  uint32_t dropped_ = 0;
  uint32_t bad_ = 0;
};

// ---- Paths ------------------------------------------------------------------

enum Root : uint8_t { ROOT_NONE = 0, ROOT_SD = 1, ROOT_INTERNAL = 2, ROOT_TILES = 3 };

inline const char* rootName(Root r) {
  switch (r) {
    case ROOT_SD: return "sd";
    case ROOT_INTERNAL: return "internal";
    case ROOT_TILES: return "tiles";
    default: return "";
  }
}

// Syntax check on a virtual path: absolute, no empty/"."/".." components, no
// trailing slash (except "/" itself), and none of the characters FAT refuses or
// that a shell or URL would misread.
inline bool pathValid(const char* p, size_t len) {
  if (!p || len == 0 || len > kMaxPath || p[0] != '/') return false;
  if (len == 1) return true;
  if (p[len - 1] == '/') return false;
  size_t comp_start = 1;
  for (size_t i = 1; i <= len; ++i) {
    if (i == len || p[i] == '/') {
      const size_t clen = i - comp_start;
      if (clen == 0) return false;
      if (clen == 1 && p[comp_start] == '.') return false;
      if (clen == 2 && p[comp_start] == '.' && p[comp_start + 1] == '.') return false;
      comp_start = i + 1;
      continue;
    }
    const unsigned char c = static_cast<unsigned char>(p[i]);
    if (c < 0x20 || c == 0x7F) return false;
    if (c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
        c == '>' || c == '|')
      return false;
  }
  return true;
}

// Split a valid virtual path into its root and the path inside that root
// ("/sd" -> ROOT_SD + "/", "/sd/a/b" -> ROOT_SD + "/a/b"). "/" -> ROOT_NONE + "/".
inline bool splitPath(const char* p, size_t len, Root* root, char* rel, size_t rel_cap) {
  if (!pathValid(p, len) || !root || !rel || rel_cap < 2) return false;
  if (len == 1) {
    *root = ROOT_NONE;
    rel[0] = '/';
    rel[1] = '\0';
    return true;
  }
  size_t end = 1;
  while (end < len && p[end] != '/') ++end;
  const size_t nlen = end - 1;
  Root r = ROOT_NONE;
  if (nlen == 2 && memcmp(p + 1, "sd", 2) == 0) r = ROOT_SD;
  else if (nlen == 8 && memcmp(p + 1, "internal", 8) == 0) r = ROOT_INTERNAL;
  else if (nlen == 5 && memcmp(p + 1, "tiles", 5) == 0) r = ROOT_TILES;
  if (r == ROOT_NONE) return false;
  const size_t rlen = len - end;   // includes the leading '/', 0 for the root itself
  if (rlen + 2 > rel_cap) return false;
  if (rlen == 0) {
    rel[0] = '/';
    rel[1] = '\0';
  } else {
    memcpy(rel, p + end, rlen);
    rel[rlen] = '\0';
  }
  *root = r;
  return true;
}

// ---- Access policy ------------------------------------------------------------
//
// Everything is readable. Writing, deleting and renaming are limited so the page
// can never touch what the running firmware keeps in memory and rewrites on its
// own (identity, contacts, channels, settings, chat history): an uploaded copy
// would be overwritten a few seconds later, or clash with it. Those files are
// download-only; restoring them needs a deliberate restore-and-reboot flow.
//
//   SD card     everything except /meshcomod, the firmware's data folder
//   internal    only /transfer, /screenshots, /lock (wallpapers), settings
//               backups (meshcore-*.json) and crash reports in the top folder
//   map tiles   everything (a re-downloadable cache)
//
// On FAT (case_insensitive) names compare without case, and any '~' is refused:
// FAT answers to 8.3 aliases such as MESHCO~1, which would otherwise be a way
// around the /meshcomod rule.

enum Access : uint8_t { A_NONE = 0, A_READ = 1, A_WRITE = 2 };

inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// rel starts with "/<name>" followed by '/' or the end of the string.
inline bool relUnder(const char* rel, const char* name, bool ci) {
  if (!rel || rel[0] != '/') return false;
  size_t i = 0;
  for (; name[i]; ++i) {
    const char a = ci ? lowerAscii(rel[1 + i]) : rel[1 + i];
    if (a != name[i]) return false;
  }
  return rel[1 + i] == '\0' || rel[1 + i] == '/';
}

inline bool topLevelFile(const char* rel) {
  return rel && rel[0] == '/' && rel[1] != '\0' && strchr(rel + 1, '/') == nullptr;
}

inline bool nameIsBackupOrCrash(const char* name) {
  if (!name) return false;
  if (strcmp(name, "wadamesh-crash.elf") == 0 || strcmp(name, "wadamesh-crash.elf.txt") == 0)
    return true;
  const size_t n = strlen(name);
  return n > 14 && strncmp(name, "meshcore-", 9) == 0 && strcmp(name + n - 5, ".json") == 0;
}

inline Access access(Root root, const char* rel, bool case_insensitive) {
  if (root == ROOT_NONE || !rel || rel[0] != '/') return A_NONE;
  if (rel[1] == '\0') return A_READ;   // a root itself: list it, never delete or rename it
  switch (root) {
    case ROOT_SD:
      if (case_insensitive && strchr(rel, '~')) return A_READ;
      if (relUnder(rel, "meshcomod", case_insensitive)) return A_READ;
      return A_WRITE;
    case ROOT_INTERNAL:
      if (case_insensitive && strchr(rel, '~')) return A_READ;
      if (relUnder(rel, "transfer", case_insensitive) ||
          relUnder(rel, "screenshots", case_insensitive) ||
          relUnder(rel, "lock", case_insensitive))
        return A_WRITE;
      if (topLevelFile(rel) && nameIsBackupOrCrash(rel + 1)) return A_WRITE;
      return A_READ;
    case ROOT_TILES:
      return A_WRITE;
    default:
      return A_NONE;
  }
}

// ---- JSON -------------------------------------------------------------------

// Append s to out as a JSON string body (no quotes). UTF-8 passes through;
// quotes, backslashes and control bytes are escaped. Returns the new length, or
// cap if it did not fit (the caller treats that as "full").
inline size_t jsonAppendEscaped(char* out, size_t pos, size_t cap, const char* s) {
  static const char hex[] = "0123456789abcdef";
  for (; s && *s; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    char esc[7];
    size_t k = 0;
    if (c == '"' || c == '\\') { esc[k++] = '\\'; esc[k++] = static_cast<char>(c); }
    else if (c < 0x20) {
      esc[k++] = '\\'; esc[k++] = 'u'; esc[k++] = '0'; esc[k++] = '0';
      esc[k++] = hex[c >> 4]; esc[k++] = hex[c & 15];
    } else esc[k++] = static_cast<char>(c);
    if (pos + k >= cap) return cap;
    memcpy(out + pos, esc, k);
    pos += k;
  }
  if (pos < cap) out[pos] = '\0';
  return pos;
}

}  // namespace UsbFiles
