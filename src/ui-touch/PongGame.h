// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <lvgl.h>

// Self-contained Pong mini-game launched from the Apps drawer.
//
// Single-player vs a simple AI. Canvas fills the screen below a score line.
// Player paddle (right) controlled by swipe/gesture, trackball, or hardware
// direction keys (UITask routes input here via isOpen()/nudge()).
// Tap "New game" to start; tap anywhere to restart after game over.
// One instance at a time — same lifecycle as SnakeGame.
class PongGame {
public:
  static void launch();                 // open (no-op if already open)
  static bool isOpen();
  static void nudge(int dx, int dy);    // UITask: trackball/key -> paddle move
  static void dismiss();               // THE single close path (also AppPage back hook)

private:
  ~PongGame();

  // Canvas dimensions are set at open() from the live screen size.
  lv_obj_t*   root_      = nullptr;
  lv_obj_t*   canvas_    = nullptr;
  lv_color_t* buf_       = nullptr;    // PSRAM canvas buffer
  lv_obj_t*   score_     = nullptr;
  lv_obj_t*   start_btn_ = nullptr;
  lv_timer_t* timer_     = nullptr;

  int  W_ = 0, H_ = 0;                // canvas size (px)
  bool started_ = false;
  bool over_    = false;

  // ball
  float bx_ = 0, by_ = 0;
  float bvx_ = 0, bvy_ = 0;

  // paddles (centre Y)
  float py_ = 0;    // player (right)
  float ay_ = 0;    // AI    (left)

  // scores
  int ps_ = 0, as_ = 0;
  static constexpr int kWin = 7;

  // geometry
  static constexpr int kPadW  = 8;
  static constexpr int kPadH  = 40;
  static constexpr int kBallR = 5;
  static constexpr float kAiSpd = 3.2f;

  void resetBall(int dir);   // dir: +1 toward player, -1 toward AI
  void reset();
  void render();
  void step();
  void updateScoreLabel();
  bool open();
  void close();

  static PongGame* s_active;
  static void timerCb(lv_timer_t* t);
  static void gestureCb(lv_event_t* e);
  static void tapCb(lv_event_t* e);
  static void startCb(lv_event_t* e);
  static void destroyAsync(void* p);
};
