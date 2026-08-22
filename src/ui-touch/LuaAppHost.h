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
void luaAppPress();                   // synthetic centre tap (down+up) -> on_input, for touchless boards' select key
bool luaAppKey(int key);              // hardware key -> on_input; true = app consumed it
bool luaAppHasOnInput();              // app declares on_input? (touchless boards: app keys vs native d-pad)
bool luaAppScroll(bool up);           // page-scroll the app body (display-only apps on touchless boards)
// kind: "channel" | "dm" | "room" — what arrived, so an app can tell them apart.
void luaAppMessage(const char* kind, const char* channel, const char* sender, const char* text);  // -> on_message
// Per-app permission bits, stored as a decimal mask in /apps/perms.kv.
// Per-app grants, stored as a bitmask in /apps/perms.kv. Channels and private
// conversations are deliberately separate grants: posting to #public is not the
// same act as reading somebody's direct messages, and an app that legitimately
// needs one usually has no business with the other.
#define LUA_PERM_SEND     1   // post to a channel (public or private) the user has
#define LUA_PERM_READ     2   // see incoming channel traffic
#define LUA_PERM_DM_SEND  4   // send a direct message, or post to a room server, AS the user
#define LUA_PERM_DM_READ  8   // see incoming direct messages and room posts
#define LUA_PERM_PROBE    16  // transmit discovery probes (wada.mesh.discover)
#else
inline bool luaAppIsOpen() { return false; }
inline void luaAppDismiss() {}
inline void luaAppSteer(int, int) {}
inline void luaAppPress() {}
inline bool luaAppKey(int) { return false; }
inline bool luaAppHasOnInput() { return false; }
inline bool luaAppScroll(bool) { return false; }
inline void luaAppMessage(const char*, const char*, const char*, const char*) {}
#endif
