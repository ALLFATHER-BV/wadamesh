// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for wada.sys.random's ranges (882a2046, LuaAppHost.cpp sysRandom), the
// range math in src/ui-touch/LuaRandomRange.h. The binding itself (random() raw,
// random(n) = 1..n, argument checks) runs in the web host build:
// web/scripts/lua-input.wada with web/scripts/apps/inputlen.lua.
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_lua_random_range.cpp \
//       -o /tmp/test_lua_random_range && /tmp/test_lua_random_range
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <initializer_list>

#include "ui-touch/LuaRandomRange.h"

// sysRandom's argument handling, as in LuaAppHost.cpp: random(n) = (1, n).
static int32_t randomN(int32_t n, uint32_t r) { return luaRandomRange(1, n, r); }

static uint32_t s = 0x9E3779B9u;
static uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

int main() {
  const uint32_t edge_r[] = { 0u, 1u, 2u, 5u, 6u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu };

  // random(n): 1..n, and every value turns up
  for (int32_t n : { 1, 2, 6, 10, 100, 1000 }) {
    bool seen[1001] = {};
    for (int i = 0; i < 200000; ++i) {
      const int32_t v = randomN(n, next());
      assert(v >= 1 && v <= n);
      seen[v] = true;
    }
    for (int32_t v = 1; v <= n; ++v) assert(seen[v]);
    for (uint32_t r : edge_r) { const int32_t v = randomN(n, r); assert(v >= 1 && v <= n); }
  }
  // random(1, 1) = 1, whatever the word (the shuffle's last step, #729)
  for (uint32_t r : edge_r) assert(luaRandomRange(1, 1, r) == 1);
  for (int i = 0; i < 10000; ++i) assert(luaRandomRange(1, 1, next()) == 1);
  // a reversed range is swapped: random(5, 3) in 3..5, all three seen
  {
    bool seen[6] = {};
    for (int i = 0; i < 10000; ++i) { const int32_t v = luaRandomRange(5, 3, next()); assert(v >= 3 && v <= 5); seen[v] = true; }
    assert(seen[3] && seen[4] && seen[5]);
  }
  // negative ranges
  for (int i = 0; i < 10000; ++i) { const int32_t v = luaRandomRange(-10, -5, next()); assert(v >= -10 && v <= -5); }
  // the ends of the 32-bit range: no overflow, no modulo by zero
  for (uint32_t r : edge_r) {
    // full range: one value per word (lo + r, wrapping), so every int32 is reachable
    assert(luaRandomRange(INT32_MIN, INT32_MAX, r) == (int32_t)(0x80000000u + r));
    assert(luaRandomRange(INT32_MAX, INT32_MIN, r) == (int32_t)(0x80000000u + r));
    const int32_t a = luaRandomRange(-1, INT32_MAX, r);  assert(a >= -1);
    const int32_t b = luaRandomRange(INT32_MIN, -1, r);  assert(b <= -1);
    const int32_t c = luaRandomRange(INT32_MIN, 0, r);   assert(c <= 0);
    const int32_t d = luaRandomRange(0, INT32_MAX, r);   assert(d >= 0);
    assert(luaRandomRange(INT32_MAX, INT32_MAX, r) == INT32_MAX);
    assert(luaRandomRange(INT32_MIN, INT32_MIN, r) == INT32_MIN);
  }
  // 882a2046's formula, lo + (esp_random() % (uint32_t)(hi - lo + 1)) in 32-bit
  // signed ints, overflowed for spans past 2^31 and divided by zero for the full
  // range: (hi - lo + 1) is 2^32 there. Shown in 64 bits rather than executed. The
  // ESP32-S3 compiler at -O2 turns it into sub / addi 1 / remu, and remu by zero is an
  // IntegerDivideByZero exception: wada.sys.random(math.mininteger, math.maxinteger)
  // from any app panicked the device.
  assert((uint32_t)((int64_t)INT32_MAX - (int64_t)INT32_MIN + 1) == 0u);

  printf("test_lua_random_range: ok\n");
  return 0;
}
