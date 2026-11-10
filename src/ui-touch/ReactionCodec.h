// SPDX-License-Identifier: GPL-3.0-or-later
// Emoji reactions from other MeshCore clients (#791).
//
// MeshCore has no reaction packet. Clients send a reaction as an ordinary text
// message in one of these conventions, and wadamesh used to show the raw payload
// ("r:6801:02") as a chat bubble:
//
//   MeshCore Open v3 (also Offband, meshcore-web)  r:<hash4>:<index2>
//     hash4  lower 16 bits, 4 lowercase hex, of the Dart VM String.hashCode
//            (Jenkins one-at-a-time over UTF-16 code units, finalized to 30 bits,
//            0 -> 1) of: decimal send timestamp (s) + target sender name (channels
//            only) + the first 5 UTF-16 code units of the target text.
//     index2 2 lowercase hex, an index into MeshCore Open's fixed emoji table.
//   MeshCore Open v1 (older)  r:<ms>_<n>_<n>:<emoji> (channel) / r:<ms>_<hex8>:<emoji> (DM)
//     Recognised so it never shows raw; it is not matched to a message.
//   MeshCore One  @[<target sender>]<emoji>\n<hash8> in channels (legacy order:
//     <emoji>@[<target sender>]\n<hash8>), <emoji>\n<hash8> in DMs.
//     hash8  8 lowercase Crockford Base32 chars of the first 5 bytes of
//            SHA-256(UTF-8 target text || send timestamp as uint32 LE).
//
// References: zjs81/meshcore-open lib/helpers/reaction_helper.dart and
// lib/widgets/emoji_picker.dart (MIT), Avi0n/MeshCoreOne docs/Reactions.md, and
// Yeraze/meshmonitor's codec (PR #5771), whose known-answer vectors
// test/test_reaction_codec.cpp reuses.
//
// Header-only and free of Arduino/LVGL so the host test can include it.
// Build the test: g++ -std=c++17 -I src -o /tmp/test_reaction_codec test/test_reaction_codec.cpp
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ReactionCodec {

// ---- MeshCore Open's emoji table -------------------------------------------
// quickEmojis, smileys, gestures, hearts, objects from emoji_picker.dart, in that
// order. The ORDER IS THE WIRE FORMAT: index 0x02 is whatever sits third here.
// Generated from the Dart lists with every code point escaped, so a variation
// selector or ZWJ cannot be lost in an editor.
static const char* const kMcoEmoji[] = {
  // quickEmojis (00..05)
  "\U0001F44D", "❤️", "\U0001F602", "\U0001F389", "\U0001F44F", "\U0001F525",
  // smileys (06..45)
  "\U0001F600", "\U0001F603", "\U0001F604", "\U0001F601", "\U0001F605", "\U0001F602",
  "\U0001F923", "\U0001F60A", "\U0001F607", "\U0001F642", "\U0001F643", "\U0001F609",
  "\U0001F60C", "\U0001F60D", "\U0001F970", "\U0001F618", "\U0001F617", "\U0001F619",
  "\U0001F61A", "\U0001F60B", "\U0001F61B", "\U0001F61D", "\U0001F61C", "\U0001F92A",
  "\U0001F928", "\U0001F9D0", "\U0001F913", "\U0001F60E", "\U0001F978", "\U0001F929",
  "\U0001F973", "\U0001F60F", "\U0001F612", "\U0001F61E", "\U0001F614", "\U0001F61F",
  "\U0001F615", "\U0001F641", "\U0001F623", "\U0001F616", "\U0001F62B", "\U0001F629",
  "\U0001F97A", "\U0001F622", "\U0001F62D", "\U0001F624", "\U0001F620", "\U0001F621",
  "\U0001F92C", "\U0001F92F", "\U0001F633", "\U0001F975", "\U0001F976", "\U0001F631",
  "\U0001F628", "\U0001F630", "\U0001F625", "\U0001F613", "\U0001F917", "\U0001F914",
  "\U0001F92D", "\U0001F92B", "\U0001F925", "\U0001F636",
  // gestures (46..66)
  "\U0001F44D", "\U0001F44E", "\U0001F44A", "✊", "\U0001F91B", "\U0001F91C", "\U0001F91E",
  "✌️", "\U0001F91F", "\U0001F918", "\U0001F44C", "\U0001F90C", "\U0001F90F",
  "\U0001F448", "\U0001F449", "\U0001F446", "\U0001F447", "☝️", "\U0001F44B",
  "\U0001F91A", "\U0001F590️", "✋", "\U0001F596", "\U0001F44F", "\U0001F64C",
  "\U0001F450", "\U0001F932", "\U0001F91D", "\U0001F64F", "✍️", "\U0001F485",
  "\U0001F933", "\U0001F4AA",
  // hearts (67..86)
  "❤️", "\U0001F9E1", "\U0001F49B", "\U0001F49A", "\U0001F499", "\U0001F49C",
  "\U0001F5A4", "\U0001F90D", "\U0001F90E", "\U0001F494", "❤️‍\U0001F525",
  "❤️‍\U0001FA79", "\U0001F495", "\U0001F49E", "\U0001F493", "\U0001F497",
  "\U0001F496", "\U0001F498", "\U0001F49D", "\U0001F49F", "\U0001F48C", "\U0001F4A2",
  "\U0001F4A5", "\U0001F4AB", "\U0001F4A6", "\U0001F4A8", "\U0001F573️", "\U0001F4AC",
  "\U0001F441️‍\U0001F5E8️", "\U0001F5E8️", "\U0001F5EF️",
  "\U0001F4AD",
  // objects (87..b7)
  "\U0001F389", "\U0001F38A", "\U0001F388", "\U0001F381", "\U0001F380", "\U0001FA85",
  "\U0001FA86", "\U0001F3C6", "\U0001F947", "\U0001F948", "\U0001F949", "⚽", "⚾",
  "\U0001F94E", "\U0001F3C0", "\U0001F3D0", "\U0001F3C8", "\U0001F3C9", "\U0001F3BE",
  "\U0001F94F", "\U0001F3B3", "\U0001F3CF", "\U0001F3D1", "\U0001F3D2", "\U0001F94D",
  "\U0001F3D3", "\U0001F3F8", "\U0001F94A", "\U0001F94B", "\U0001F945", "⛳", "\U0001F525",
  "⭐", "\U0001F31F", "✨", "⚡", "\U0001F4A1", "\U0001F526", "\U0001F3EE",
  "\U0001FA94", "\U0001F4F1", "\U0001F4BB", "⌚", "\U0001F4F7", "\U0001F4FA", "\U0001F4FB",
  "\U0001F3B5", "\U0001F3B6", "\U0001F680",
};
static constexpr int kMcoEmojiCount = (int)(sizeof(kMcoEmoji) / sizeof(kMcoEmoji[0]));

// ---- UTF-8 ------------------------------------------------------------------
// Decode one code point and advance. A malformed byte reads as U+FFFD, which is
// also what Dart's utf8 codec puts in its place, so a broken sequence hashes the
// same on both ends.
inline uint32_t nextCodePoint(const char*& p) {
  const uint8_t* s = (const uint8_t*)p;
  const uint8_t c = s[0];
  if (c < 0x80) { p += 1; return c; }
  int n = 0; uint32_t cp = 0, min = 0;
  if      ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; min = 0x80; }
  else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; min = 0x800; }
  else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; min = 0x10000; }
  else { p += 1; return 0xFFFD; }
  for (int i = 1; i <= n; ++i) {
    if ((s[i] & 0xC0) != 0x80) { p += i; return 0xFFFD; }   // stops at a NUL too
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  p += n + 1;
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
  return cp;
}

// Largest byte length <= max_bytes that ends on a code point boundary.
inline size_t utf8Prefix(const char* s, size_t max_bytes) {
  size_t n = strlen(s);
  if (n <= max_bytes) return n;
  n = max_bytes;
  while (n > 0 && (((uint8_t)s[n]) & 0xC0) == 0x80) --n;
  return n;
}

// ---- MeshCore Open hash (Dart String.hashCode) ------------------------------
struct DartHash {
  uint32_t h = 0;
  void unit(uint32_t u) { h += u; h += h << 10; h ^= h >> 6; }
  // Feed UTF-8 text as UTF-16 code units, at most max_units of them (-1 = all).
  // A limit that falls inside a surrogate pair feeds the high half alone, as
  // Dart's substring(0, 5) does.
  void utf8(const char* s, int max_units = -1) {
    int used = 0;
    const char* p = s;
    while (*p && (max_units < 0 || used < max_units)) {
      const uint32_t cp = nextCodePoint(p);
      if (cp >= 0x10000) {
        const uint32_t v = cp - 0x10000;
        unit(0xD800 + (v >> 10)); ++used;
        if (max_units >= 0 && used >= max_units) break;
        unit(0xDC00 + (v & 0x3FF)); ++used;
      } else {
        unit(cp); ++used;
      }
    }
  }
  uint32_t finish() const {
    uint32_t x = h;
    x += x << 3; x ^= x >> 11; x += x << 15;
    x &= 0x3FFFFFFF;
    return x ? x : 1;
  }
};

inline uint32_t dartStringHash(const char* s) { DartHash d; d.utf8(s); return d.finish(); }

// sender: the target message's sender name in a channel, nullptr in a DM.
inline uint16_t mcoHash(uint32_t ts, const char* sender, const char* text) {
  char num[12];
  int n = 0;
  char rev[12];
  uint32_t v = ts;
  do { rev[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  for (int i = 0; i < n; ++i) num[i] = rev[n - 1 - i];
  num[n] = '\0';
  DartHash d;
  d.utf8(num);
  if (sender) d.utf8(sender);
  d.utf8(text ? text : "", 5);
  return (uint16_t)(d.finish() & 0xFFFF);
}

// ---- SHA-256 (for the MeshCore One hash) ------------------------------------
// Small and portable: the same code on every board and in the host test, with no
// mbedTLS/IDF dependency. Inputs here are one chat message, so speed is moot.
struct Sha256 {
  uint32_t st[8];
  uint8_t  buf[64];
  uint64_t len = 0;
  size_t   fill = 0;
  Sha256() {
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(st, iv, sizeof st);
  }
  static uint32_t ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
  void block(const uint8_t* b) {
    static const uint32_t k[64] = {
      0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
      0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
      0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
      0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
      0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
      0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
      0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
      0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = (uint32_t)b[4*i] << 24 | (uint32_t)b[4*i+1] << 16 | (uint32_t)b[4*i+2] << 8 | b[4*i+3];
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = ror(w[i-15], 7) ^ ror(w[i-15], 18) ^ (w[i-15] >> 3);
      const uint32_t s1 = ror(w[i-2], 17) ^ ror(w[i-2], 19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a = st[0], b2 = st[1], c = st[2], d = st[3], e = st[4], f = st[5], g = st[6], h = st[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b2) ^ (a & c) ^ (b2 & c));
      h = g; g = f; f = e; e = d + t1; d = c; c = b2; b2 = a; a = t1 + t2;
    }
    st[0] += a; st[1] += b2; st[2] += c; st[3] += d; st[4] += e; st[5] += f; st[6] += g; st[7] += h;
  }
  void update(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    len += n;
    while (n) {
      const size_t take = (64 - fill) < n ? (64 - fill) : n;
      memcpy(buf + fill, p, take);
      fill += take; p += take; n -= take;
      if (fill == 64) { block(buf); fill = 0; }
    }
  }
  void final(uint8_t out[32]) {
    const uint64_t bits = len * 8;
    const uint8_t pad = 0x80, zero = 0;
    update(&pad, 1);
    while (fill != 56) update(&zero, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(lb, 8);
    for (int i = 0; i < 8; ++i) {
      out[4*i] = (uint8_t)(st[i] >> 24); out[4*i+1] = (uint8_t)(st[i] >> 16);
      out[4*i+2] = (uint8_t)(st[i] >> 8); out[4*i+3] = (uint8_t)st[i];
    }
  }
};

// ---- MeshCore One hash --------------------------------------------------------
static constexpr char kCrockford[] = "0123456789abcdefghjkmnpqrstvwxyz";

inline void mc1Hash(uint32_t ts, const char* text, char out[9]) {
  Sha256 s;
  if (text) s.update(text, strlen(text));
  const uint8_t le[4] = { (uint8_t)ts, (uint8_t)(ts >> 8), (uint8_t)(ts >> 16), (uint8_t)(ts >> 24) };
  s.update(le, 4);
  uint8_t d[32];
  s.final(d);
  uint64_t v = 0;
  for (int i = 0; i < 5; ++i) v = (v << 8) | d[i];   // 40 bits -> 8 x 5 bits
  for (int i = 0; i < 8; ++i) out[i] = kCrockford[(v >> (35 - 5 * i)) & 0x1F];
  out[8] = '\0';
}

// Crockford hash char as received -> canonical lowercase, folding the aliases
// Crockford decoding allows (O->0, I/L->1). 0 = not a hash character (U is excluded).
inline char foldCrockford(char c) {
  if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  if (c == 'o') return '0';
  if (c == 'i' || c == 'l') return '1';
  if (c == 'u') return 0;
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) return c;
  return 0;
}

// ---- Emoji sanity check ---------------------------------------------------------
// One user-perceived emoji: the first code point at U+2000 or above (or the run
// carries a VS16 / keycap mark, for 1️⃣ and ©️), and exactly one base character
// once ZWJ joins, variation selectors, skin tones, tags and flag pairs are folded.
// Keeps prose with a newline and 8 letters at the end from being read as a reaction.
inline bool looksLikeEmoji(const char* e, size_t n) {
  if (n == 0) return false;
  const char* p = e;
  const char* end = e + n;
  bool first = true, first_high = false, has_mark = false, after_zwj = false, ri_open = false;
  int bases = 0;
  while (p < end) {
    const uint32_t cp = nextCodePoint(p);
    if (p > end) return false;
    if (first) { first_high = cp >= 0x2000; first = false; }
    if (cp == 0xFE0F || cp == 0x20E3) has_mark = true;
    if (cp == 0x200D) { after_zwj = true; continue; }
    if ((cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0x20E3 ||
        (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F)) continue;
    if (after_zwj) { after_zwj = false; continue; }
    if (cp >= 0x1F1E6 && cp <= 0x1F1FF) {             // regional indicators pair into a flag
      if (ri_open) { ri_open = false; continue; }
      ri_open = true; ++bases; continue;
    }
    ri_open = false;
    if (cp < 0x2000 && !(cp == '#' || cp == '*' || (cp >= '0' && cp <= '9') || cp == 0xA9 || cp == 0xAE))
      return false;                                    // letters, spaces, punctuation: prose
    ++bases;
  }
  return bases == 1 && (first_high || has_mark);
}

// ---- Parse ------------------------------------------------------------------
enum Kind : uint8_t {
  NONE = 0,
  MCO_V3,   // r:<hash4>:<index2>, matchable
  MCO_V1,   // older MeshCore Open, shown but not matched
  MC1,      // MeshCore One, matchable
};

struct Reaction {
  Kind kind = NONE;
  char hash[9] = {0};           // MCO_V3: 4 hex; MC1: 8 canonical Crockford
  char emoji[48] = {0};         // UTF-8
  char target_sender[32] = {0}; // MC1 channel form: whose message it reacts to
};

namespace detail {
inline bool isLowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
inline int  hexVal(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }
inline bool allDigits(const char* s, size_t n) {
  if (n == 0) return false;
  for (size_t i = 0; i < n; ++i) if (s[i] < '0' || s[i] > '9') return false;
  return true;
}
inline bool copyEmoji(Reaction* r, const char* e, size_t n) {
  if (n == 0 || n >= sizeof(r->emoji) || !looksLikeEmoji(e, n)) return false;
  memcpy(r->emoji, e, n); r->emoji[n] = '\0';
  return true;
}
inline bool copySender(Reaction* r, const char* s, size_t n) {
  if (n == 0 || n >= sizeof(r->target_sender)) return false;
  memcpy(r->target_sender, s, n); r->target_sender[n] = '\0';
  return true;
}

inline bool parseMcoV3(const char* s, Reaction* r) {
  if (strlen(s) != 9 || s[0] != 'r' || s[1] != ':' || s[6] != ':') return false;
  for (int i = 2; i < 6; ++i) if (!isLowerHex(s[i])) return false;
  if (!isLowerHex(s[7]) || !isLowerHex(s[8])) return false;
  const int idx = hexVal(s[7]) * 16 + hexVal(s[8]);
  if (idx >= kMcoEmojiCount) return false;
  r->kind = MCO_V3;
  memcpy(r->hash, s + 2, 4); r->hash[4] = '\0';
  const size_t n = strlen(kMcoEmoji[idx]);
  memcpy(r->emoji, kMcoEmoji[idx], n + 1);
  return true;
}

// r:<1-15 digits>_<1-10 digits>_<1-10 digits>:<emoji>  or  r:<1-15 digits>_<8 hex>:<emoji>
inline bool parseMcoV1(const char* s, Reaction* r) {
  if (s[0] != 'r' || s[1] != ':') return false;
  const char* p = s + 2;
  const char* us1 = strchr(p, '_');
  if (!us1 || !allDigits(p, (size_t)(us1 - p)) || us1 - p > 15) return false;
  const char* colon = strchr(us1 + 1, ':');
  if (!colon) return false;
  const char* mid = us1 + 1;
  const char* us2 = (const char*)memchr(mid, '_', (size_t)(colon - mid));
  if (us2) {
    if (!allDigits(mid, (size_t)(us2 - mid)) || us2 - mid > 10) return false;
    if (!allDigits(us2 + 1, (size_t)(colon - us2 - 1)) || colon - us2 - 1 > 10) return false;
  } else {
    if (colon - mid != 8) return false;
    for (const char* q = mid; q < colon; ++q) if (!isLowerHex(*q)) return false;
  }
  if (!copyEmoji(r, colon + 1, strlen(colon + 1))) return false;
  r->kind = MCO_V1;
  return true;
}

// <line>\n<8 hash chars>, exactly one newline. Fills r->hash; returns the line length.
inline bool splitMc1(const char* s, size_t* line_len, Reaction* r) {
  const char* nl = strchr(s, '\n');
  if (!nl || strchr(nl + 1, '\n')) return false;
  if (strlen(nl + 1) != 8) return false;
  for (int i = 0; i < 8; ++i) {
    const char c = foldCrockford(nl[1 + i]);
    if (!c) return false;
    r->hash[i] = c;
  }
  r->hash[8] = '\0';
  *line_len = (size_t)(nl - s);
  return *line_len > 0;
}

inline bool parseMc1Channel(const char* s, Reaction* r) {
  size_t n = 0;
  if (!splitMc1(s, &n, r)) return false;
  // Current order: @[sender]emoji, the sender running to the LAST ']' that still
  // leaves an emoji after it.
  if (n > 3 && s[0] == '@' && s[1] == '[') {
    for (size_t j = n - 1; j > 2; --j) {
      if (s[j - 1] != ']') continue;
      if (copySender(r, s + 2, j - 3) && copyEmoji(r, s + j, n - j)) { r->kind = MC1; return true; }
      break;
    }
  }
  // Legacy order: emoji@[sender], the shortest emoji before an "@[" whose ']' ends the line.
  if (s[n - 1] == ']') {
    for (size_t k = 1; k + 3 <= n; ++k) {
      if (s[k] != '@' || s[k + 1] != '[') continue;
      if (copyEmoji(r, s, k) && copySender(r, s + k + 2, n - k - 3)) { r->kind = MC1; return true; }
      break;
    }
  }
  r->emoji[0] = r->target_sender[0] = '\0';
  return false;
}

inline bool parseMc1Dm(const char* s, Reaction* r) {
  size_t n = 0;
  if (!splitMc1(s, &n, r)) return false;
  if (!copyEmoji(r, s, n)) return false;
  r->kind = MC1;
  return true;
}
}  // namespace detail

// `body` is the message text without any "Sender: " prefix. `channel` picks the
// MeshCore One form that is valid there (with @[sender] in a channel, bare in a DM).
inline bool parse(const char* body, bool channel, Reaction* out) {
  if (!body || !out) return false;
  *out = Reaction();
  if (body[0] == 'r' && body[1] == ':') {
    if (detail::parseMcoV3(body, out)) return true;
    *out = Reaction();
    if (detail::parseMcoV1(body, out)) return true;
    *out = Reaction();
    return false;
  }
  const bool ok = channel ? detail::parseMc1Channel(body, out) : detail::parseMc1Dm(body, out);
  if (!ok) *out = Reaction();
  return ok;
}

// Does message (ts, text) match reaction r? `sender` is the target message's sender
// name in a channel and nullptr in a DM.
inline bool matches(const Reaction& r, uint32_t ts, const char* sender, const char* text) {
  if (r.kind == MCO_V3) {
    static const char hx[] = "0123456789abcdef";
    const uint16_t h = mcoHash(ts, sender, text);
    const char got[5] = { hx[(h >> 12) & 0xF], hx[(h >> 8) & 0xF], hx[(h >> 4) & 0xF], hx[h & 0xF], 0 };
    return memcmp(got, r.hash, 4) == 0;
  }
  if (r.kind == MC1) {
    // The hash covers text and time only; in a channel the named sender must match too.
    if (strcmp(r.target_sender, sender ? sender : "") != 0) return false;
    char got[9];
    mc1Hash(ts, text, got);
    return memcmp(got, r.hash, 8) == 0;
  }
  return false;
}

}  // namespace ReactionCodec
