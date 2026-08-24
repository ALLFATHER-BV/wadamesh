// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <lvgl.h>

// Keyboard-nav: a CLICKABLE container that must never be a focus target itself
// but whose children still are. Distinct from UITask.cpp's NAV_SKIP_FLAG, which
// takes the whole subtree out of nav. Needed by the Lua app host: its body is
// clickable so touch boards get press events, and on keypad-nav boards (M9,
// Pager, Tanmatsu, T-Deck trackball) navCollect otherwise harvests that body as
// a leaf target and navFocusCb's reverse-video fill paints the whole app white
// under the app's own widgets. Lives here because AppPage.h is the header both
// UITask.cpp and LuaAppHost.cpp already share.
#define NAV_PASSTHRU_FLAG LV_OBJ_FLAG_USER_4

// Shared chrome for full-screen "app pages" — Spectrum, RF Monitor, Discover, Airtime,
// Snake, VNC, Remote, Reader. Every one of them must look and behave identically:
//
//   * the GLOBAL status bar goes tall and paints "< <title>"; a tap on it closes the page
//     (on both CLICKED and PRESSED, so the cap-touch swipe detector dropping a lone
//     CLICKED can't trap the user on a touch-only board)
//   * the page root covers everything BELOW that bar, opaque and clickable, so the tab
//     bar and whatever was on screen underneath cannot be touched through the page
//   * the root is deleted ASYNCHRONOUSLY, never synchronously from inside an event
//     callback on one of its own children
//
// These exist because the self-contained app modules (SnakeGame, the Lua host) hand-rolled
// the chrome against a hardcoded `kTopBar = 22`. STATUSBAR_H is a runtime value — SC(22)
// once the UI scale is above 100%, and SB_TOP_PAD + SB_ROW*2 on the T-Display P4's
// two-row bar — so on those boards the pages were positioned too high, which put their
// own title and close button UNDERNEATH the real status bar with no reachable exit. Going
// through here instead means a page can never disagree with the bar about its own height.

/** y of the first usable content row: immediately under the (possibly tall) status bar. */
lv_coord_t appPageContentTop();

/** Usable content height below the status bar. */
lv_coord_t appPageContentH();

/**
 * Create a page root: full width, from just under the status bar to the bottom of the
 * screen, on lv_layer_top, opaque `bg_color`, non-scrollable and CLICKABLE so no press
 * reaches the UI underneath.
 */
lv_obj_t* appPageCreateRoot(uint32_t bg_color);

/** Take the status bar over: tall, "< title", tap anywhere on it calls close_fn. */
void appPageBegin(const char* title, void (*close_fn)());

/** Same contract, but the bar stays ONE line (no tall glass row) — for pages whose
 *  own chrome (e.g. the Lua Store's tab bar) starts right at the top. */
void appPageBeginSlim(const char* title, void (*close_fn)());

/**
 * Hand the status bar back — but only if `close_fn` is still the installed hook, so a
 * page closing after another one already opened cannot steal the new page's chrome.
 * Safe to call more than once (idempotent).
 */
void appPageEnd(void (*close_fn)());

/** Queue `root` for deletion after the current event finishes. Null-safe. */
void appPageDeleteRootAsync(lv_obj_t* root);
