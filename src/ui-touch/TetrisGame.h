// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Self-contained Tetris mini-game launched from the Apps drawer.
//
// Mirrors SnakeGame's integration style: one full-screen overlay on lv_layer_top,
// its own score / controls row, start gate, pause button, and close button.
// UITask routes swipe + trackball input here via isOpen()/steer() so the game
// stays decoupled from the rest of the touch UI internals.
class TetrisGame {
public:
  static void launch();                 // open (no-op if already open)
  static bool isOpen();                 // UITask: gate background gestures / tab bar
  static void steer(int dx, int dy);    // UITask: trackball or swipe -> game control

private:
  static constexpr int kFieldW = 10;
  static constexpr int kFieldH = 20;
  static constexpr int kPreviewW = 4;
  static constexpr int kPreviewH = 4;
  static constexpr int kMaxCellPx = 14;
  static constexpr uint32_t kTickMs = 40;

  lv_obj_t*   root_      = nullptr;
  lv_obj_t*   board_     = nullptr;
  lv_obj_t*   preview_   = nullptr;
  lv_color_t* boardBuf_  = nullptr;
  lv_color_t* prevBuf_   = nullptr;
  lv_obj_t*   score_     = nullptr;
  lv_obj_t*   nextLbl_   = nullptr;
  lv_obj_t*   start_btn_ = nullptr;
  lv_obj_t*   pause_btn_ = nullptr;
  lv_obj_t*   pause_lbl_ = nullptr;
  lv_timer_t* timer_     = nullptr;

  int cellPx_      = 10;
  int boardWpx_    = 0;
  int boardHpx_    = 0;
  int boardX_      = 0;
  int boardY_      = 0;
  int previewX_    = 0;
  int previewY_    = 0;

  uint8_t field_[kFieldH][kFieldW] = {};
  int currentX_ = 0, currentY_ = 0;
  int currentType_ = 0, currentRot_ = 0;
  int nextType_ = 0;
  int scoreVal_ = 0;
  int linesCleared_ = 0;
  int dropIntervalMs_ = 500;
  uint32_t lastDropMs_ = 0;
  bool started_ = false;
  bool paused_  = false;
  bool over_    = false;

  bool open();
  void close();
  void startGame();
  void beginRound();
  void updateScoreLabel();
  void render();
  void step();
  void settlePiece();
  bool movePiece(int dx, int dy);
  void rotatePiece();
  void softDrop();
  void spawnPiece();
  void lockPiece();
  void clearLines();
  bool checkCollision(int x, int y, int rot) const;
  int  getBlock(int type, int rot, int x, int y) const;

  static TetrisGame* s_active;
  static void timerCb(lv_timer_t* t);
  static void tapCb(lv_event_t* e);
  static void closeCb(lv_event_t* e);
  static void startCb(lv_event_t* e);
  static void pauseCb(lv_event_t* e);
};
