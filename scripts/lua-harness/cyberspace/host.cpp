// SPDX-License-Identifier: GPL-3.0-or-later
// Host runner for the Cyberspace app test: the vendored Lua (LUA_32BITS, as on
// the device) with the firmware's own wada.json codec (src/ui-touch/LuaJson.h)
// registered as the global JSON, then test.lua with its arguments.
#include "ui-touch/LuaJson.h"
extern "C" {
#include "lualib.h"
}
#include <stdio.h>

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: host test.lua app.lua [w h]\n"); return 2; }
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);
  lua_newtable(L);
  lua_pushcfunction(L, luajson::decode); lua_setfield(L, -2, "decode");
  lua_pushcfunction(L, luajson::encode); lua_setfield(L, -2, "encode");
  lua_setglobal(L, "JSON");
  lua_newtable(L);
  for (int i = 2; i < argc; i++) { lua_pushstring(L, argv[i]); lua_rawseti(L, -2, i - 1); }
  lua_setglobal(L, "arg");
  if (luaL_dofile(L, argv[1]) != LUA_OK) { fprintf(stderr, "%s\n", lua_tostring(L, -1)); return 1; }
  lua_close(L);
  return 0;
}
