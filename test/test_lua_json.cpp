// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for wada.json (src/ui-touch/LuaJson.h), against the vendored Lua with
// the firmware's LUA_32BITS numeric model -- the reason big integers decode to text.
//
//   cc -c -O1 -w -I lib/lua/src lib/lua/src/*.c && \
//   c++ -std=c++17 -Wall -Wextra -I src -I lib/lua/src test/test_lua_json.cpp *.o \
//       -o /tmp/test_lua_json && /tmp/test_lua_json
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/LuaJson.h"

extern "C" {
#include "lualib.h"
}

static lua_State* L;

static void run(const char* code) {
  if (luaL_dostring(L, code) != LUA_OK) {
    fprintf(stderr, "FAIL: %s\n  %s\n", code, lua_tostring(L, -1));
    assert(false);
  }
}

int main() {
  L = luaL_newstate();
  luaL_openlibs(L);
  lua_newtable(L);
  lua_pushcfunction(L, luajson::decode); lua_setfield(L, -2, "decode");
  lua_pushcfunction(L, luajson::encode); lua_setfield(L, -2, "encode");
  lua_setglobal(L, "json");

  // shapes the cyberspace API returns
  run(R"(
    local v = json.decode('{"data":[{"id":"a1","timestamp":1730000000123,"n":42,"x":-7,'
      .. '"f":1.5,"ok":true,"no":false,"gone":null}],"cursor":"1730000000000"}')
    assert(type(v.data) == "table" and #v.data == 1)
    local m = v.data[1]
    assert(m.id == "a1")
    assert(m.timestamp == "1730000000123", "big int must stay exact text")
    assert(math.type(m.n) == "integer" and m.n == 42 and m.x == -7)
    assert(math.type(m.f) == "float" and m.f == 1.5)
    assert(m.ok == true and m.no == false and m.gone == nil)
    assert(v.cursor == "1730000000000")
  )");

  // int32 edges
  run(R"(
    assert(json.decode("2147483647") == 2147483647)
    assert(json.decode("-2147483648") == math.mininteger)
    assert(json.decode("2147483648") == "2147483648")
    assert(json.decode("-2147483649") == "-2147483649")
    assert(json.decode("123456789012345678901234567890") == "123456789012345678901234567890")
  )");

  // strings and escapes
  run(R"(
    assert(json.decode([["a\"b\\c\/d\n\t"]]) == 'a"b\\c/d\n\t')
    assert(json.decode([["é"]]) == "\xc3\xa9")
    assert(json.decode([["😀"]]) == "\xf0\x9f\x98\x80")
    assert(json.decode([["\ud83d x"]]) == "\xef\xbf\xbd x")
    assert(json.decode('"héllo"') == "héllo")
  )");

  // arrays with nulls keep their positions
  run(R"(
    local a = json.decode("[1,null,3]")
    assert(a[1] == 1 and a[2] == nil and a[3] == 3)
    assert(next(json.decode("[]")) == nil and next(json.decode("{}")) == nil)
  )");

  // errors come back as nil, message
  run(R"(
    for _, bad in ipairs({ "", "{", "[1,]", '{"a"}', '{"a":}', "tru", '"abc', "1 2",
                           '"\x01"', "[" .. string.rep("[", 40) .. string.rep("]", 41) }) do
      local v, err = json.decode(bad)
      assert(v == nil and type(err) == "string", "accepted: " .. bad)
    end
  )");

  // encode
  run(R"(
    assert(json.encode("a\"b\n\1") == [["a\"b\n\u0001"]])
    assert(json.encode(5) == "5" and json.encode(true) == "true" and json.encode(nil) == "null")
    assert(json.encode({1, 2, "x"}) == '[1,2,"x"]')
    assert(json.encode({}) == "{}")
    assert(json.encode({content = "hi"}) == '{"content":"hi"}')
    local obj = json.decode(json.encode({ a = { b = { 1, 2 } }, s = "é", f = 0.5 }))
    assert(obj.a.b[2] == 2 and obj.s == "é" and obj.f == 0.5)
    assert(not pcall(json.encode, { [1] = 1, [3] = 3 }))   -- sparse: neither array nor object
    assert(not pcall(json.encode, function() end))
    assert(not pcall(json.encode, 0/0))
    local t = {} t.self = t
    assert(not pcall(json.encode, t))
    -- long output forces the buffer to grow several times mid-walk
    local big = {}
    for i = 1, 500 do big[i] = { id = tostring(i), content = string.rep("x", 40) } end
    local back = json.decode(json.encode(big))
    assert(#back == 500 and back[500].id == "500" and #back[250].content == 40)
  )");

  lua_close(L);
  printf("test_lua_json: ok\n");
  return 0;
}
