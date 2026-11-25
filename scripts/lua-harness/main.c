// Host harness for wadamesh Lua apps: vendored Lua 5.4.7 (LUA_32BITS, same
// numeric model as the device) + the same library set the firmware opens, plus
// the debug lib for the harness's own instruction-budget hook.
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include <stdio.h>
#include <stdlib.h>

// The device caps each app's heap (kHeapCap in LuaAppHost.cpp, 256 KB) and Lua raises
// "not enough memory" past it. __memcap(n) applies such a cap to everything allocated
// from now on (n < 0 lifts it); __mempeak() returns the peak and live bytes since.
static size_t s_used, s_cap = (size_t)-1, s_peak, s_base;
static void* capAlloc(void* ud, void* p, size_t o, size_t n) {
  (void)ud;
  if (!p) o = 0;
  if (n == 0) { s_used -= o; free(p); return NULL; }
  if (s_used - o + n > s_cap) return NULL;
  void* q = realloc(p, n);
  if (!q) return NULL;
  s_used += n - o;
  if (s_used > s_peak) s_peak = s_used;
  return q;
}
static int memcap(lua_State* L) {
  const lua_Integer n = luaL_optinteger(L, 1, -1);
  s_base = s_peak = s_used;
  s_cap = n < 0 ? (size_t)-1 : s_used + (size_t)n;
  return 0;
}
static int mempeak(lua_State* L) {
  lua_pushinteger(L, (lua_Integer)(s_peak - s_base));
  lua_pushinteger(L, (lua_Integer)(s_used - s_base));
  s_peak = s_used;
  return 2;
}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: luah harness.lua app.lua [scenario]\n"); return 2; }
  lua_State* L = lua_newstate(capAlloc, NULL);
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);        lua_pop(L, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);  lua_pop(L, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);  lua_pop(L, 1);
  luaL_requiref(L, LUA_DBLIBNAME, luaopen_debug, 1);   lua_pop(L, 1);
  luaL_requiref(L, LUA_IOLIBNAME, luaopen_io, 1);      lua_pop(L, 1);
  lua_register(L, "__memcap", memcap);
  lua_register(L, "__mempeak", mempeak);
  lua_pushstring(L, argv[2]); lua_setglobal(L, "APP_PATH");
  lua_pushstring(L, argc > 3 ? argv[3] : "all"); lua_setglobal(L, "SCENARIO");
  if (luaL_dofile(L, argv[1]) != LUA_OK) { fprintf(stderr, "harness: %s\n", lua_tostring(L, -1)); return 1; }
  lua_close(L);
  return 0;
}
