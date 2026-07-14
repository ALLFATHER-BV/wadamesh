// SPDX-License-Identifier: GPL-3.0-or-later
#include "SnakeGame.h"
#include "i18n.h"

#include <Arduino.h>          // random()
#include <string.h>           // memmove
#include <LvglPsramAlloc.h>   // PSRAM-preferred buffer for the canvas

// Mirrors the app's status-bar height; the overlay starts below it so the bar
// stays visible. Kept local so this module needs nothing from UITask.
static constexpr lv_coord_t kTopBar = 22;
static constexpr uint32_t   kStepMs = 300;   // tick period (a touch slower than before)

// Palette (hex literals so the module is self-contained).
static constexpr uint32_t kColBg   = 0x0A0B0C;   // playfield background
static constexpr uint32_t kColFood = 0xE0584C;   // food
static constexpr uint32_t kColBody = 0x15B6A6;   // snake body (brand teal)
static constexpr uint32_t kColText = 0xE6EAEE;

SnakeGame* SnakeGame::s_active = nullptr;

bool SnakeGame::isOpen() { return s_active != nullptr && s_active->root_ != nullptr; }

void SnakeGame::steer(int dx, int dy) {
  if (!s_active || !s_active->started_ || s_active->over_) return;
  // Trackball deltas -> one cardinal direction (dominant axis).
  if (dx == 0 && dy == 0) return;
  const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  if (adx >= ady) s_active->setDir(dx > 0 ? 1 : -1, 0);
  else            s_active->setDir(0, dy > 0 ? 1 : -1);
}

void SnakeGame::launch() {
  if (s_active) return;
  SnakeGame* g = new SnakeGame();
  s_active = g;
  if (!g->open()) { s_active = nullptr; delete g; }
}

void SnakeGame::placeFood() {
  const int cells = cols_ * rows_;
  for (int tries = 0; tries < 4 * cells + 16; ++tries) {
    const uint8_t fx = (uint8_t)random(cols_);
    const uint8_t fy = (uint8_t)random(rows_);
    bool on = false;
    for (int i = 0; i < len_; ++i) if (bx_[i] == fx && by_[i] == fy) { on = true; break; }
    if (!on) { fx_ = fx; fy_ = fy; return; }
  }
}

void SnakeGame::reset() {
  len_ = 3;
  const uint8_t cx = (uint8_t)(cols_ / 2), cy = (uint8_t)(rows_ / 2);
  for (int i = 0; i < len_; ++i) { bx_[i] = (uint8_t)(cx - i); by_[i] = cy; }
  dx_ = 1; dy_ = 0; ndx_ = 1; ndy_ = 0;
  over_ = false; score_val_ = 0;
  placeFood();
}

void SnakeGame::render() {
  if (!canvas_) return;
  const int c = kCellPx;
  lv_canvas_fill_bg(canvas_, lv_color_hex(kColBg), LV_OPA_COVER);
  lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d); d.radius = 2;
  d.bg_color = lv_color_hex(kColFood);
  lv_canvas_draw_rect(canvas_, fx_ * c + 1, fy_ * c + 1, c - 2, c - 2, &d);
  for (int i = 0; i < len_; ++i) {
    d.bg_color = lv_color_hex(kColBody);
    d.bg_opa   = (i == 0) ? LV_OPA_COVER : LV_OPA_80;   // head reads brighter
    lv_canvas_draw_rect(canvas_, bx_[i] * c + 1, by_[i] * c + 1, c - 2, c - 2, &d);
  }
}

void SnakeGame::updateScoreLabel() {
  if (!score_) return;
  if (over_)         lv_label_set_text_fmt(score_, TR("Game over  \xe2\x80\x94  score %d   (tap to restart)"), score_val_);
  else if (paused_)  lv_label_set_text_fmt(score_, TR("Paused  \xe2\x80\x94  score %d"), score_val_);
  else if (started_) lv_label_set_text_fmt(score_, TR("score %d"), score_val_);
  else               lv_label_set_text(score_, TR("Snake  \xe2\x80\x94  swipe or roll the trackball"));
}

void SnakeGame::step() {
  if (over_ || !started_ || paused_) return;
  if (!(ndx_ == -dx_ && ndy_ == -dy_)) { dx_ = ndx_; dy_ = ndy_; }
  const int nx = bx_[0] + dx_, ny = by_[0] + dy_;
  const bool eat = (nx == fx_ && ny == fy_);
  bool over = (nx < 0 || ny < 0 || nx >= cols_ || ny >= rows_);
  const int clen = eat ? len_ : len_ - 1;   // the tail vacates unless we grow
  for (int i = 0; i < clen && !over; ++i) if (bx_[i] == nx && by_[i] == ny) over = true;
  if (over) { over_ = true; updateScoreLabel(); return; }

  int newlen = len_ + (eat ? 1 : 0);
  if (newlen > cols_ * rows_) newlen = cols_ * rows_;
  memmove(&bx_[1], &bx_[0], (size_t)(newlen - 1));
  memmove(&by_[1], &by_[0], (size_t)(newlen - 1));
  bx_[0] = (uint8_t)nx; by_[0] = (uint8_t)ny;
  len_ = newlen;
  if (eat) { score_val_++; placeFood(); updateScoreLabel(); }
  render();
}

void SnakeGame::setDir(int dx, int dy) { ndx_ = dx; ndy_ = dy; }

void SnakeGame::startGame() {
  if (started_) return;
  started_ = true;
  paused_  = false;
  if (start_btn_) { lv_obj_del(start_btn_); start_btn_ = nullptr; }
  if (pause_btn_) {                              // reveal the pause toggle now that we're playing
    lv_obj_clear_flag(pause_btn_, LV_OBJ_FLAG_HIDDEN);
    if (pause_lbl_) lv_label_set_text(pause_lbl_, LV_SYMBOL_PAUSE);
  }
  reset();
  updateScoreLabel();
  render();
  if (!timer_) timer_ = lv_timer_create(timerCb, kStepMs, this);
}

void SnakeGame::togglePause() {
  if (!started_ || over_) return;
  paused_ = !paused_;
  if (pause_lbl_) lv_label_set_text(pause_lbl_, paused_ ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  updateScoreLabel();
}

bool SnakeGame::open() {
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
  const lv_coord_t sh = lv_disp_get_ver_res(nullptr);
  root_ = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(root_);
  lv_obj_set_size(root_, sw, sh - kTopBar);
  lv_obj_set_pos(root_, 0, kTopBar);
  lv_obj_set_style_bg_color(root_, lv_color_hex(0x0E1216), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(root_, gestureCb, LV_EVENT_GESTURE, this);
  lv_obj_add_event_cb(root_, tapCb,     LV_EVENT_CLICKED, this);

  // Fill the unoccupied screen below a 24-px score/close row. Whole cells only.
  const int avail_w = (int)sw;
  const int avail_h = (int)(sh - kTopBar) - 24;
  cols_ = avail_w / kCellPx; if (cols_ > kMaxCols) cols_ = kMaxCols; if (cols_ < 6) cols_ = 6;
  rows_ = avail_h / kCellPx; if (rows_ > kMaxRows) rows_ = kMaxRows; if (rows_ < 6) rows_ = 6;
  const int pw = cols_ * kCellPx, ph = rows_ * kCellPx;
  buf_ = (lv_color_t*)lvglPsramAlloc((size_t)pw * ph * sizeof(lv_color_t));
  if (!buf_) return false;
  canvas_ = lv_canvas_create(root_);
  lv_canvas_set_buffer(canvas_, buf_, pw, ph, LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(canvas_, LV_ALIGN_BOTTOM_MID, 0, 0);   // playfield fills the lower area

  score_ = lv_label_create(root_);
  lv_obj_set_style_text_color(score_, lv_color_hex(kColText), LV_PART_MAIN);
  lv_obj_align(score_, LV_ALIGN_TOP_LEFT, 6, 4);

  lv_obj_t* x = lv_btn_create(root_);
  lv_obj_set_size(x, 30, 24);
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -4, 0);
  lv_obj_add_event_cb(x, closeCb, LV_EVENT_CLICKED, this);
  lv_obj_t* xl = lv_label_create(x); lv_label_set_text(xl, LV_SYMBOL_CLOSE); lv_obj_center(xl);

  // Pause/resume toggle, left of the X. Hidden until the game starts.
  pause_btn_ = lv_btn_create(root_);
  lv_obj_set_size(pause_btn_, 30, 24);
  lv_obj_align(pause_btn_, LV_ALIGN_TOP_RIGHT, -40, 0);
  lv_obj_add_event_cb(pause_btn_, pauseCb, LV_EVENT_CLICKED, this);
  pause_lbl_ = lv_label_create(pause_btn_); lv_label_set_text(pause_lbl_, LV_SYMBOL_PAUSE); lv_obj_center(pause_lbl_);
  lv_obj_add_flag(pause_btn_, LV_OBJ_FLAG_HIDDEN);

  // Paused: draw the initial snake, then wait on a centred "New game" button.
  reset();
  updateScoreLabel();
  render();
  start_btn_ = lv_btn_create(root_);
  lv_obj_set_size(start_btn_, 150, 44);
  lv_obj_align(start_btn_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(start_btn_, startCb, LV_EVENT_CLICKED, this);
  lv_obj_t* sl = lv_label_create(start_btn_);
  lv_label_set_text(sl, LV_SYMBOL_PLAY "  New game");
  lv_obj_center(sl);
  return true;
}

void SnakeGame::close() {
  if (timer_) { lv_timer_del(timer_); timer_ = nullptr; }
  if (root_)  { lv_obj_del(root_);   root_  = nullptr; }   // deletes canvas + button children
  if (buf_)   { lvglPsramFree(buf_);  buf_  = nullptr; }   // free AFTER the canvas is gone
  canvas_ = nullptr; score_ = nullptr; start_btn_ = nullptr;
  pause_btn_ = nullptr; pause_lbl_ = nullptr;
}

// ---- LVGL C-callback trampolines (user_data = the instance) ----
void SnakeGame::timerCb(lv_timer_t* t) {
  auto* self = static_cast<SnakeGame*>(t->user_data);
  if (self) self->step();
}
void SnakeGame::gestureCb(lv_event_t* e) {
  auto* self = static_cast<SnakeGame*>(lv_event_get_user_data(e));
  if (!self || !self->started_) return;
  switch (lv_indev_get_gesture_dir(lv_indev_get_act())) {
    case LV_DIR_TOP:    self->setDir(0, -1); break;
    case LV_DIR_BOTTOM: self->setDir(0, 1);  break;
    case LV_DIR_LEFT:   self->setDir(-1, 0); break;
    case LV_DIR_RIGHT:  self->setDir(1, 0);  break;
    default: break;
  }
}
void SnakeGame::tapCb(lv_event_t* e) {
  auto* self = static_cast<SnakeGame*>(lv_event_get_user_data(e));
  if (self && self->started_ && self->over_) { self->reset(); self->updateScoreLabel(); self->render(); }
}
void SnakeGame::startCb(lv_event_t* e) {
  auto* self = static_cast<SnakeGame*>(lv_event_get_user_data(e));
  if (self) self->startGame();
}
void SnakeGame::pauseCb(lv_event_t* e) {
  auto* self = static_cast<SnakeGame*>(lv_event_get_user_data(e));
  if (self) self->togglePause();
}
void SnakeGame::closeCb(lv_event_t* e) {
  auto* self = static_cast<SnakeGame*>(lv_event_get_user_data(e));
  if (!self) return;
  self->close();
  if (s_active == self) s_active = nullptr;
  delete self;
}
