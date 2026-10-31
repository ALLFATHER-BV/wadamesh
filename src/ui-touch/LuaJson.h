// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// wada.json.decode / wada.json.encode (LuaAppHost.cpp; host test:
// test/test_lua_json.cpp). In C because a web API reply is up to 32 KB, and a
// Lua-side parser walking that byte by byte blows the 100k-instruction budget of
// a single callback several times over.
//
// Numbers: Lua here is LUA_32BITS (int32 integers, single floats). A JSON integer
// that fits int32 comes back as an integer; one that does not -- a millisecond
// timestamp, a 64-bit id -- comes back as its exact decimal TEXT, because a float
// would round 1730000000123 to the nearest 131072 and a paging cursor built from
// it would skip or repeat messages. Anything with a fraction or exponent is a
// float. encode() writes a string as a string, so such a value only round-trips
// as a number if the app converts it; as a URL parameter it needs nothing.
//
// null decodes to nil (an object key with a null value is simply absent).
// encode(): a table whose keys are exactly 1..n (n > 0) is an array, any other
// table an object with string keys; an empty table is {}. NaN/inf, functions and
// cycles (via the depth limit) are errors.
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace luajson {

constexpr int kMaxDepth = 32;

struct Dec {
  lua_State* L;
  const char* s;
  size_t n, i;
  char* scratch;          // decoded string bytes; never longer than the input
  const char* err;
};

inline void skipWs(Dec& d) {
  while (d.i < d.n && (d.s[d.i] == ' ' || d.s[d.i] == '\t' || d.s[d.i] == '\n' || d.s[d.i] == '\r')) d.i++;
}

inline int hex4(const char* p) {
  int v = 0;
  for (int k = 0; k < 4; k++) {
    const char c = p[k];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return -1;
  }
  return v;
}

inline size_t putUtf8(char* o, uint32_t cp) {
  if (cp < 0x80)    { o[0] = (char)cp; return 1; }
  if (cp < 0x800)   { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                      o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
  o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

// Pushes the string at d.i (on the opening quote). \u escapes are at most as long
// as the UTF-8 they turn into (6 bytes -> <= 3, a 12-byte surrogate pair -> 4),
// so the scratch buffer sized to the input always holds it.
inline bool decString(Dec& d) {
  d.i++;
  size_t o = 0;
  while (d.i < d.n) {
    const char c = d.s[d.i++];
    if (c == '"') { lua_pushlstring(d.L, d.scratch, o); return true; }
    if ((unsigned char)c < 0x20) { d.err = "control character in string"; return false; }
    if (c != '\\') { d.scratch[o++] = c; continue; }
    if (d.i >= d.n) break;
    const char e = d.s[d.i++];
    switch (e) {
      case '"': case '\\': case '/': d.scratch[o++] = e; break;
      case 'b': d.scratch[o++] = '\b'; break;
      case 'f': d.scratch[o++] = '\f'; break;
      case 'n': d.scratch[o++] = '\n'; break;
      case 'r': d.scratch[o++] = '\r'; break;
      case 't': d.scratch[o++] = '\t'; break;
      case 'u': {
        if (d.i + 4 > d.n) { d.err = "short \\u escape"; return false; }
        int cp = hex4(d.s + d.i);
        if (cp < 0) { d.err = "bad \\u escape"; return false; }
        d.i += 4;
        uint32_t u = (uint32_t)cp;
        if (u >= 0xD800 && u <= 0xDBFF && d.i + 6 <= d.n && d.s[d.i] == '\\' && d.s[d.i + 1] == 'u') {
          const int lo = hex4(d.s + d.i + 2);
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            u = 0x10000 + ((u - 0xD800) << 10) + ((uint32_t)lo - 0xDC00);
            d.i += 6;
          }
        }
        if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;   // a lone surrogate is not UTF-8
        o += putUtf8(d.scratch + o, u);
        break;
      }
      default: d.err = "bad escape"; return false;
    }
  }
  d.err = "unterminated string";
  return false;
}

inline bool decNumber(Dec& d) {
  const size_t start = d.i;
  bool frac = false;
  if (d.i < d.n && d.s[d.i] == '-') d.i++;
  const size_t digits = d.i;
  while (d.i < d.n && d.s[d.i] >= '0' && d.s[d.i] <= '9') d.i++;
  if (d.i == digits) { d.err = "bad number"; return false; }
  if (d.i < d.n && d.s[d.i] == '.') {
    frac = true; d.i++;
    while (d.i < d.n && d.s[d.i] >= '0' && d.s[d.i] <= '9') d.i++;
  }
  if (d.i < d.n && (d.s[d.i] == 'e' || d.s[d.i] == 'E')) {
    frac = true; d.i++;
    if (d.i < d.n && (d.s[d.i] == '+' || d.s[d.i] == '-')) d.i++;
    while (d.i < d.n && d.s[d.i] >= '0' && d.s[d.i] <= '9') d.i++;
  }
  const size_t len = d.i - start;
  char tmp[48];
  if (len >= sizeof tmp) {
    if (frac) { d.err = "number too long"; return false; }
    lua_pushlstring(d.L, d.s + start, len);   // an absurdly long integer: keep it exact
    return true;
  }
  memcpy(tmp, d.s + start, len);
  tmp[len] = 0;
  if (frac) { lua_pushnumber(d.L, (lua_Number)strtod(tmp, nullptr)); return true; }
  // Eleven digits or more cannot be an int32, and stay under strtoll's range
  // until 19, so only short ones are converted at all.
  const size_t digits_len = len - (tmp[0] == '-' ? 1 : 0);
  const long long v = digits_len <= 10 ? strtoll(tmp, nullptr, 10) : 0;
  if (digits_len <= 10 && v >= (long long)LUA_MININTEGER && v <= (long long)LUA_MAXINTEGER)
    lua_pushinteger(d.L, (lua_Integer)v);
  else
    lua_pushlstring(d.L, tmp, len);
  return true;
}

inline bool decValue(Dec& d, int depth);

inline bool lit(Dec& d, const char* word) {
  const size_t k = strlen(word);
  if (d.i + k > d.n || memcmp(d.s + d.i, word, k) != 0) { d.err = "unexpected token"; return false; }
  d.i += k;
  return true;
}

// Pushes exactly one value, or pushes nothing and sets d.err. A null pushes nil.
inline bool decValue(Dec& d, int depth) {
  if (depth > kMaxDepth) { d.err = "nested too deep"; return false; }
  if (!lua_checkstack(d.L, 4)) { d.err = "nested too deep"; return false; }
  skipWs(d);
  if (d.i >= d.n) { d.err = "unexpected end"; return false; }
  const char c = d.s[d.i];
  if (c == '"') return decString(d);
  if (c == '-' || (c >= '0' && c <= '9')) return decNumber(d);
  if (c == 't') { if (!lit(d, "true"))  return false; lua_pushboolean(d.L, 1); return true; }
  if (c == 'f') { if (!lit(d, "false")) return false; lua_pushboolean(d.L, 0); return true; }
  if (c == 'n') { if (!lit(d, "null"))  return false; lua_pushnil(d.L); return true; }
  if (c == '[') {
    d.i++;
    lua_newtable(d.L);
    lua_Integer k = 0;
    skipWs(d);
    if (d.i < d.n && d.s[d.i] == ']') { d.i++; return true; }
    for (;;) {
      if (!decValue(d, depth + 1)) { lua_pop(d.L, 1); return false; }
      lua_rawseti(d.L, -2, ++k);   // a null leaves a hole; the index still advances
      skipWs(d);
      if (d.i < d.n && d.s[d.i] == ',') { d.i++; continue; }
      if (d.i < d.n && d.s[d.i] == ']') { d.i++; return true; }
      d.err = "expected , or ]";
      lua_pop(d.L, 1);
      return false;
    }
  }
  if (c == '{') {
    d.i++;
    lua_newtable(d.L);
    skipWs(d);
    if (d.i < d.n && d.s[d.i] == '}') { d.i++; return true; }
    for (;;) {
      skipWs(d);
      if (d.i >= d.n || d.s[d.i] != '"') { d.err = "expected a key"; lua_pop(d.L, 1); return false; }
      if (!decString(d)) { lua_pop(d.L, 1); return false; }
      skipWs(d);
      if (d.i >= d.n || d.s[d.i] != ':') { d.err = "expected :"; lua_pop(d.L, 2); return false; }
      d.i++;
      if (!decValue(d, depth + 1)) { lua_pop(d.L, 2); return false; }
      lua_rawset(d.L, -3);         // t[key] = nil for a null: no entry
      skipWs(d);
      if (d.i < d.n && d.s[d.i] == ',') { d.i++; continue; }
      if (d.i < d.n && d.s[d.i] == '}') { d.i++; return true; }
      d.err = "expected , or }";
      lua_pop(d.L, 1);
      return false;
    }
  }
  d.err = "unexpected character";
  return false;
}

// wada.json.decode(s) -> value | nil, "message at byte N"
inline int decode(lua_State* L) {
  size_t n = 0;
  const char* s = luaL_checklstring(L, 1, &n);
  Dec d{L, s, n, 0, nullptr, nullptr};
  // A userdata rather than malloc: if a push below raises a memory error, the
  // collector frees it, where a malloc'd buffer would leak on the longjmp.
  d.scratch = (char*)lua_newuserdatauv(L, n + 1, 0);
  const int base = lua_gettop(L);
  bool ok = decValue(d, 0);
  if (ok) { skipWs(d); if (d.i != d.n) { ok = false; d.err = "trailing characters"; lua_pop(L, 1); } }
  if (!ok) {
    lua_settop(L, base);
    lua_pushnil(L);
    lua_pushfstring(L, "%s at byte %d", d.err ? d.err : "bad json", (int)d.i + 1);
    return 2;
  }
  return 1;
}

// The output grows in a userdata kept at one fixed stack slot (Enc::slot), not a
// luaL_Buffer: luaL_Buffer needs its box on top of the stack whenever it grows,
// and walking a table keeps keys and values above it. A raised error leaves the
// userdata to the collector, so nothing leaks on the longjmp.
struct Enc {
  lua_State* L;
  int slot;
  char* p;
  size_t n, cap;
};

inline void reserve(Enc& e, size_t more) {
  if (e.n + more <= e.cap) return;
  size_t cap = e.cap ? e.cap : 256;
  while (cap < e.n + more) cap *= 2;
  char* q = (char*)lua_newuserdatauv(e.L, cap, 0);
  if (e.n) memcpy(q, e.p, e.n);
  lua_replace(e.L, e.slot);
  e.p = q;
  e.cap = cap;
}
inline void put(Enc& e, const char* s, size_t k) { reserve(e, k); memcpy(e.p + e.n, s, k); e.n += k; }
inline void put(Enc& e, const char* s) { put(e, s, strlen(s)); }
inline void put(Enc& e, char c) { reserve(e, 1); e.p[e.n++] = c; }

inline void encString(Enc& e, const char* s, size_t n) {
  static const char kHex[] = "0123456789abcdef";
  put(e, '"');
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)s[i];
    switch (c) {
      case '"':  put(e, "\\\""); break;
      case '\\': put(e, "\\\\"); break;
      case '\n': put(e, "\\n"); break;
      case '\r': put(e, "\\r"); break;
      case '\t': put(e, "\\t"); break;
      default:
        if (c < 0x20) {
          const char u[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 15]};
          put(e, u, 6);
        } else {
          put(e, (char)c);
        }
    }
  }
  put(e, '"');
}

// Encodes the value at absolute index idx.
inline void encValue(Enc& e, int idx, int depth) {
  lua_State* L = e.L;
  if (depth > kMaxDepth) luaL_error(L, "json.encode: nested too deep (a cycle?)");
  switch (lua_type(L, idx)) {
    case LUA_TNIL:     put(e, "null"); return;
    case LUA_TBOOLEAN: put(e, lua_toboolean(L, idx) ? "true" : "false"); return;
    case LUA_TNUMBER: {
      char tmp[32];
      if (lua_isinteger(L, idx)) {
        snprintf(tmp, sizeof tmp, LUA_INTEGER_FMT, (LUAI_UACINT)lua_tointeger(L, idx));
      } else {
        const double v = (double)lua_tonumber(L, idx);
        if (isnan(v) || isinf(v)) luaL_error(L, "json.encode: NaN or infinity");
        snprintf(tmp, sizeof tmp, "%.9g", v);
      }
      put(e, tmp);
      return;
    }
    case LUA_TSTRING: {
      size_t n = 0;
      const char* s = lua_tolstring(L, idx, &n);
      encString(e, s, n);
      return;
    }
    case LUA_TTABLE: break;
    default: luaL_error(L, "json.encode: cannot encode a %s", luaL_typename(L, idx)); return;
  }
  luaL_checkstack(L, 4, "json.encode");
  const lua_Integer len = (lua_Integer)lua_rawlen(L, idx);
  lua_Integer keys = 0;
  bool all_int = true;
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    keys++;
    if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_tointeger(L, -2) > len) all_int = false;
    lua_pop(L, 1);
  }
  if (len > 0 && all_int && keys == len) {
    put(e, '[');
    for (lua_Integer k = 1; k <= len; k++) {
      if (k > 1) put(e, ',');
      lua_rawgeti(L, idx, k);
      encValue(e, lua_gettop(L), depth + 1);
      lua_pop(L, 1);
    }
    put(e, ']');
    return;
  }
  put(e, '{');
  bool first = true;
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "json.encode: object keys must be strings");
    if (!first) put(e, ',');
    first = false;
    size_t kn = 0;
    const char* ks = lua_tolstring(L, -2, &kn);
    encString(e, ks, kn);
    put(e, ':');
    encValue(e, lua_gettop(L), depth + 1);
    lua_pop(L, 1);
  }
  put(e, '}');
}

// wada.json.encode(v) -> string (raises on a value JSON cannot hold)
inline int encode(lua_State* L) {
  luaL_checkany(L, 1);
  lua_settop(L, 1);
  lua_pushnil(L);                 // slot 2: the output userdata
  Enc e{L, 2, nullptr, 0, 0};
  encValue(e, 1, 0);
  lua_pushlstring(L, e.p ? e.p : "", e.n);
  return 1;
}

}  // namespace luajson
