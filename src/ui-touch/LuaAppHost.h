// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Lua app host (LUA_APPS.md Phase 1). Runs ONE sandboxed Lua app at a time as a
// full-screen AppPage overlay, exactly like SnakeGame: tall "< title"
// bar, status-bar-tap dismiss, async teardown. Apps are event-driven (the script
// returns {on_open,on_tick,on_input,on_close}) and every callback runs under an
// instruction budget + pcall — an app error or runaway loop becomes a toast and
// a clean close, never a watchdog reset. All app memory lives in PSRAM.
#include "device_caps.h"
#include <stddef.h>

#if CAP_LUA_APPS
// Launch an app from a Lua source buffer (id names the store file + bar title).
// Returns false if a host is already open or the state failed to allocate.
bool luaAppLaunch(const char* id, const char* title, const char* src, size_t len);
// Launch from /apps/<id>.lua on the app storage FS; falls back to `embedded`
// (when non-null) if the file is absent/unreadable. The file-first order makes
// seeded built-ins user-editable.
bool luaAppLaunchFile(const char* id, const char* title, const char* embedded, size_t embedded_len);
bool luaAppIsOpen();
void luaAppDismiss();                 // THE close path (AppPage back hook)
void luaAppSteer(int dx, int dy);     // trackball/keypad direction -> on_input
#else
inline bool luaAppIsOpen() { return false; }
inline void luaAppDismiss() {}
inline void luaAppSteer(int, int) {}
#endif
