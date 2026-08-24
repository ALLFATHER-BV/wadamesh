// Lua app host — Phase 0 spike (LUA_APPS.md). Everything here is measurement:
// prove Lua 5.4 fits the budgets (flash <= 250 KB, state <= 40 KB PSRAM, ticks
// bounded by an instruction hook) before the wada.* API is built on top.
#include "LuaHost.h"

#if defined(WADA_LUA_SPIKE)
#include <Arduino.h>
#include <esp_heap_caps.h>

extern "C" {
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
}

// ---- PSRAM-only allocator with a hard cap + accounting -----------------------
// Apps must never touch internal DRAM: the V4 has ~2 MB PSRAM but a starved
// internal heap (#192), so every Lua byte lives in SPIRAM and the cap turns a
// runaway app into a clean Lua "not enough memory" error instead of an OOM.
struct LuaHeap {
  size_t used = 0, peak = 0, cap = 0;
};

static void* luaPsAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  LuaHeap* h = (LuaHeap*)ud;
  if (!ptr) osize = 0;                      // Lua passes the OLD type in osize for fresh blocks
  if (nsize == 0) {
    if (ptr) { h->used -= osize; heap_caps_free(ptr); }
    return nullptr;
  }
  if (h->used - osize + nsize > h->cap) return nullptr;   // hard per-app cap
  void* np = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!np) return nullptr;
  h->used += nsize - osize;
  if (h->used > h->peak) h->peak = h->used;
  return np;
}

// ---- instruction budget hook -------------------------------------------------
// LUA_MASKCOUNT fires every N VM instructions; raising an error unwinds to the
// enclosing pcall. This is what makes a while-true app a toast, not a watchdog.
static void budgetHook(lua_State* L, lua_Debug*) {
  luaL_error(L, "app exceeded its instruction budget");
}

// ---- sandbox: base + math + string + table only ------------------------------
// No io/os/package/load-of-new-chunks. The spike opens the same set the real
// host will, so the flash measurement includes exactly the shipped libs.
static void openSandboxLibs(lua_State* L) {
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);       lua_pop(L, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); lua_pop(L, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);lua_pop(L, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
  // strip the escape hatches base leaves behind
  static const char* const kill[] = { "dofile", "loadfile", "load", "require", "collectgarbage" };
  for (const char* k : kill) { lua_pushnil(L); lua_setglobal(L, k); }
}

static const char* kBench =
    "local function fib(n) if n < 2 then return n end return fib(n-1) + fib(n-2) end\n"
    "local t = {}\n"
    "for i = 1, 400 do t[i] = ('cell %d'):format(i) end\n"
    "return fib(24), #t\n";

void luaSpikeRun() {
  Serial.println("[LUA] ---- Phase 0 spike ----");
  LuaHeap heap; heap.cap = 256 * 1024;

  uint32_t t0 = millis();
  lua_State* L = lua_newstate(luaPsAlloc, &heap);
  if (!L) { Serial.println("[LUA] state alloc FAILED"); return; }
  openSandboxLibs(L);
  Serial.printf("[LUA] state+libs: %lums, heap used=%u peak=%u\n",
                (unsigned long)(millis() - t0), (unsigned)heap.used, (unsigned)heap.peak);

  // bench WITHOUT hook
  t0 = millis();
  if (luaL_dostring(L, kBench) != LUA_OK) {
    Serial.printf("[LUA] bench error: %s\n", lua_tostring(L, -1));
    lua_close(L);
    return;
  }
  uint32_t plain_ms = millis() - t0;
  int fib = (int)lua_tointeger(L, -2), cells = (int)lua_tointeger(L, -1);
  lua_pop(L, 2);

  // bench WITH the per-tick hook cadence the host will use (every 10k instr,
  // no error raised here — measuring pure hook overhead)
  lua_sethook(L, [](lua_State*, lua_Debug*) {}, LUA_MASKCOUNT, 10000);
  t0 = millis();
  luaL_dostring(L, kBench);
  lua_pop(L, 2);
  uint32_t hooked_ms = millis() - t0;
  lua_sethook(L, nullptr, 0, 0);

  // prove the budget actually contains a hostile loop
  lua_sethook(L, budgetHook, LUA_MASKCOUNT, 100000);
  t0 = millis();
  int rc = luaL_dostring(L, "while true do end");
  uint32_t contained_ms = millis() - t0;
  lua_sethook(L, nullptr, 0, 0);

  Serial.printf("[LUA] fib(24)=%d cells=%d plain=%lums hooked=%lums (overhead %+ld%%)\n",
                fib, cells, (unsigned long)plain_ms, (unsigned long)hooked_ms,
                plain_ms ? (long)((hooked_ms - plain_ms) * 100 / plain_ms) : 0);
  Serial.printf("[LUA] hostile while-true contained: rc=%d in %lums (%s)\n",
                rc, (unsigned long)contained_ms,
                rc != LUA_OK ? lua_tostring(L, -1) : "NOT CONTAINED?!");
  Serial.printf("[LUA] script heap: used=%u peak=%u cap=%u (all PSRAM)\n",
                (unsigned)heap.used, (unsigned)heap.peak, (unsigned)heap.cap);
  lua_close(L);
  Serial.printf("[LUA] after close: leaked=%u (must be 0)\n", (unsigned)heap.used);
  Serial.println("[LUA] ---- spike done ----");
}
#endif  // WADA_LUA_SPIKE
