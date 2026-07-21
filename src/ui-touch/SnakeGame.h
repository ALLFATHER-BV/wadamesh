// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Self-contained Snake mini-game launched from the Apps drawer.
//
// Owns a full-screen overlay on lv_layer_top: a canvas playfield filling the
// unoccupied screen, a score line, and a close button. The game waits on a
// "New game" button (doesn't start moving on open). Steer by swipe OR trackball
// (UITask routes the trackball here via isOpen()/steer() and hides the cursor);
// tap to restart after a game over; the X closes it. One instance at a time.
//
// Decoupled from UITask internals — depends only on LVGL and lvglPsramAlloc.
class SnakeGame {
public:
  static void launch();                 // open (no-op if already open)
  static bool isOpen();                 // UITask: gate the trackball + tab bar
  static void steer(int dx, int dy);    // UITask: trackball motion -> direction
  static void dismiss();                // close from outside (status-bar tap) — guaranteed exit

private:
  // The playfield grid is sized to the screen at open() (cols_ x rows_), capped
  // by these maxima so the body arrays are fixed-size.
  static constexpr int kCellPx   = 14;  // px per cell
  static constexpr int kMaxCols  = 40;
  static constexpr int kMaxRows  = 30;
  static constexpr int kMaxCells = kMaxCols * kMaxRows;

  lv_obj_t*   root_      = nullptr;     // full-screen overlay (owns everything below)
  lv_obj_t*   canvas_    = nullptr;
  lv_color_t* buf_       = nullptr;     // canvas pixel buffer (PSRAM)
  lv_obj_t*   score_     = nullptr;
  lv_obj_t*   start_btn_ = nullptr;     // "New game" gate (deleted once tapped)
  lv_obj_t*   pause_btn_ = nullptr;     // pause/resume toggle (shown once playing)
  lv_obj_t*   pause_lbl_ = nullptr;     // its glyph (play <-> pause)
  lv_timer_t* timer_     = nullptr;

  int     cols_ = 14, rows_ = 14;       // grid dimensions (set at open)
  bool    started_ = false;             // false until "New game" is tapped
  bool    paused_  = false;             // pause toggle (timer keeps ticking; step() no-ops)
  uint8_t bx_[kMaxCells], by_[kMaxCells];   // snake body, [0] = head
  int     len_ = 0;
  int     dx_ = 1, dy_ = 0;             // active direction
  int     ndx_ = 1, ndy_ = 0;          // queued direction (applied next step)
  uint8_t fx_ = 0, fy_ = 0;            // food cell
  bool    over_ = false;
  int     score_val_ = 0;

  bool open();                          // build UI (paused); false on alloc failure
  void startGame();                     // begin the tick (from the New-game button)
  void togglePause();                   // pause/resume the running game
  void close();
  void reset();
  void placeFood();
  void render();
  void step();
  void setDir(int dx, int dy);
  void updateScoreLabel();

  static SnakeGame* s_active;           // the one live game (or nullptr)
  static void timerCb(lv_timer_t* t);
  static void gestureCb(lv_event_t* e);
  static void tapCb(lv_event_t* e);
  static void closeCb(lv_event_t* e);
  static void startCb(lv_event_t* e);
  static void pauseCb(lv_event_t* e);
};
