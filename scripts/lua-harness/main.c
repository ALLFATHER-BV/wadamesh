// Host harness for wadamesh Lua apps: vendored Lua 5.4.7 (LUA_32BITS, same
// numeric model as the device) + the same library set the firmware opens, plus
// the debug lib for the harness's own instruction-budget hook.
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include <stdio.h>
int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: luah harness.lua app.lua [scenario]\n"); return 2; }
  lua_State* L = luaL_newstate();
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);        lua_pop(L, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);  lua_pop(L, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);  lua_pop(L, 1);
  luaL_requiref(L, LUA_DBLIBNAME, luaopen_debug, 1);   lua_pop(L, 1);
  luaL_requiref(L, LUA_IOLIBNAME, luaopen_io, 1);      lua_pop(L, 1);
  lua_pushstring(L, argv[2]); lua_setglobal(L, "APP_PATH");
  lua_pushstring(L, argc > 3 ? argv[3] : "all"); lua_setglobal(L, "SCENARIO");
  if (luaL_dofile(L, argv[1]) != LUA_OK) { fprintf(stderr, "harness: %s\n", lua_tostring(L, -1)); return 1; }
  lua_close(L);
  return 0;
}
