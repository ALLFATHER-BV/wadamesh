// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

// wada.sys.random(lo, hi)'s range (LuaAppHost.cpp sysRandom; host test:
// test/test_lua_random_range.cpp). lo..hi inclusive, in either order, from one 32-bit
// random word. Lua integers are 32-bit here (LUA_32BITS), so the span is worked out in
// unsigned 32-bit arithmetic: hi - lo + 1 in signed ints overflows for spans past
// 2^31, and for the full range (math.mininteger, math.maxinteger) it is 2^32, which
// is 0 in 32 bits, so a modulo by it divides by zero (an exception on the ESP32).
// A span of 0 therefore means "every 32-bit value": the word itself.
inline int32_t luaRandomRange(int32_t lo, int32_t hi, uint32_t r) {
  if (hi < lo) { const int32_t t = lo; lo = hi; hi = t; }
  const uint32_t span = (uint32_t)hi - (uint32_t)lo + 1u;
  const uint32_t off = span ? r % span : r;
  return (int32_t)((uint32_t)lo + off);
}
