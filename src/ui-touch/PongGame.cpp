// SPDX-License-Identifier: GPL-3.0-or-later
#include "PongGame.h"
#include "AppPage.h"
#include "i18n.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <LvglPsramAlloc.h>

static constexpr uint32_t kTickMs   = 30;     // ~33 fps
static constexpr uint32_t kColBg    = 0x0A0C0E;
static constexpr uint32_t kColAi    = 0x828891;   // AI paddle (grey)
static constexpr uint32_t kColPlayer= 0x15B6A6;   // player paddle (brand teal)
static constexpr uint32_t kColBall  = 0xE6EAEE;
static constexpr uint32_t kColText  = 0xE6EAEE;
static constexpr uint32_t kColNet   = 0x1E2428;   // centre-line colour

PongGame* PongGame::s_active = nullptr;

bool PongGame::isOpen() { return s_active && s_active->root_; }

void PongGame::nudge(int dx, int dy) {
  if (!s_active || s_active->over_) return;
  if (!s_active->started_) s_active->started_ = true;
  // dominant axis → vertical paddle movement only
  const int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
  const int move = (ady >= adx) ? dy : 0;
  if (move == 0) return;
  const float step = 14.0f;
  const float half = s_active->kPadH / 2.0f;
  s_active->py_ += move * step;
  if (s_active->py_ < half) s_active->py_ = half;
  if (s_active->py_ > s_active->H_ - half) s_active->py_ = (float)(s_active->H_) - half;
}

void PongGame::launch() {
  if (s_active) return;
  PongGame* g = new PongGame();
  s_active = g;
  if (!g->open()) { s_active = nullptr; delete g; }
}

void PongGame::destroyAsync(void* p) { delete static_cast<PongGame*>(p); }

void PongGame::dismiss() {
  if (!s_active) return;
  PongGame* g = s_active;
  s_active = nullptr;
  g->close();
  lv_async_call(destroyAsync, g);
}

PongGame::~PongGame() {
  if (buf_) { lvglPsramFree(buf_); buf_ = nullptr; }
}

// ── helpers ───────────────────────────────────────────────────────────────────

static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : v > hi ? hi : v;
}

void PongGame::resetBall(int dir) {
  bx_ = W_ / 2.0f;
  by_ = H_ / 2.0f;
  // random vertical angle ±25°
  const float angle = ((float)(random(-25, 26))) * 3.14159f / 180.0f;
  const float spd = 3.8f;
  bvx_ =  dir * spd * cosf(angle);
  bvy_ =        spd * sinf(angle);
}

void PongGame::reset() {
  py_  = H_ / 2.0f;
  ay_  = H_ / 2.0f;
  ps_  = 0;
  as_  = 0;
  over_ = false;
  resetBall(1);
}

// ── render ────────────────────────────────────────────────────────────────────

void PongGame::render() {
  if (!canvas_) return;

  lv_canvas_fill_bg(canvas_, lv_color_hex(kColBg), LV_OPA_COVER);

  lv_draw_rect_dsc_t rd;
  lv_draw_rect_dsc_init(&rd);
  rd.radius = 0;

  // centre net (dashes)
  rd.bg_color = lv_color_hex(kColNet);
  rd.bg_opa   = LV_OPA_COVER;
  const int cx = W_ / 2;
  for (int y = 0; y < H_; y += 12)
    lv_canvas_draw_rect(canvas_, cx - 1, y, 2, 8, &rd);

  // AI paddle (left)
  rd.bg_color = lv_color_hex(kColAi);
  rd.radius   = 3;
  lv_canvas_draw_rect(canvas_, 6,
                      (int)(ay_ - kPadH / 2), kPadW, kPadH, &rd);

  // player paddle (right)
  rd.bg_color = lv_color_hex(kColPlayer);
  lv_canvas_draw_rect(canvas_, W_ - 6 - kPadW,
                      (int)(py_ - kPadH / 2), kPadW, kPadH, &rd);

  // ball
  rd.bg_color = lv_color_hex(kColBall);
  rd.radius   = 2;
  lv_canvas_draw_rect(canvas_, (int)(bx_ - kBallR), (int)(by_ - kBallR),
                      kBallR * 2, kBallR * 2, &rd);
}

// ── physics ───────────────────────────────────────────────────────────────────

void PongGame::step() {
  if (!started_ || over_) return;

  bx_ += bvx_;
  by_ += bvy_;

  // top/bottom wall
  if (by_ - kBallR < 0)   { by_ = kBallR;        bvy_ = -bvy_; }
  if (by_ + kBallR > H_)  { by_ = H_ - kBallR;   bvy_ = -bvy_; }

  // AI tracking
  const float ai_diff = by_ - ay_;
  const float ai_move = clampf(ai_diff, -kAiSpd, kAiSpd);
  ay_ = clampf(ay_ + ai_move, kPadH / 2.0f, H_ - kPadH / 2.0f);

  // AI paddle hit (left)
  const float ai_right = 6.0f + kPadW;
  if (bvx_ < 0 && bx_ - kBallR < ai_right && bx_ - kBallR > ai_right - 10) {
    if (by_ >= ay_ - kPadH / 2 && by_ <= ay_ + kPadH / 2) {
      bx_  = ai_right + kBallR;
      bvx_ = -bvx_ * 1.05f;
      bvy_ = ((by_ - ay_) / (kPadH / 2.0f)) * fabsf(bvx_) * 0.8f;
    }
  }

  // player paddle hit (right)
  const float pl_left = (float)(W_ - 6 - kPadW);
  if (bvx_ > 0 && bx_ + kBallR > pl_left && bx_ + kBallR < pl_left + 10) {
    if (by_ >= py_ - kPadH / 2 && by_ <= py_ + kPadH / 2) {
      bx_  = pl_left - kBallR;
      bvx_ = -bvx_ * 1.05f;
      bvy_ = ((by_ - py_) / (kPadH / 2.0f)) * fabsf(bvx_) * 0.8f;
    }
  }

  // speed cap
  const float spd = sqrtf(bvx_ * bvx_ + bvy_ * bvy_);
  if (spd > 12.0f) { bvx_ = bvx_ / spd * 12.0f; bvy_ = bvy_ / spd * 12.0f; }

  // scoring
  if (bx_ + kBallR < 0) {
    ps_++;
    if (ps_ >= kWin) { over_ = true; } else { resetBall(1); }
    updateScoreLabel();
  } else if (bx_ - kBallR > W_) {
    as_++;
    if (as_ >= kWin) { over_ = true; } else { resetBall(-1); }
    updateScoreLabel();
  }

  render();
}

void PongGame::updateScoreLabel() {
  if (!score_) return;
  if (over_) {
    const bool win = ps_ >= kWin;
    lv_label_set_text_fmt(score_,
      TR(win ? "You win!  \xe2\x80\x94  %d : %d  (tap to restart)"
             : "AI wins  \xe2\x80\x94  %d : %d  (tap to restart)"),
      ps_, as_);
  } else if (started_) {
    lv_label_set_text_fmt(score_, TR("You %d  \xe2\x80\x94  AI %d"), ps_, as_);
  } else {
    lv_label_set_text(score_, TR("Pong  \xe2\x80\x94  swipe up/down or use arrow keys"));
  }
}

// ── open / close ──────────────────────────────────────────────────────────────

bool PongGame::open() {
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
  const lv_coord_t sh = lv_disp_get_ver_res(nullptr);

  root_ = appPageCreateRoot(kColBg);
  lv_obj_add_event_cb(root_, gestureCb, LV_EVENT_GESTURE, this);
  lv_obj_add_event_cb(root_, tapCb,     LV_EVENT_CLICKED, this);

  static char s_bar_title[16];
  snprintf(s_bar_title, sizeof s_bar_title, "%s", TR("Pong"));
  appPageBegin(s_bar_title, &PongGame::dismiss);

  // canvas below a 24 px score row
  W_ = (int)sw;
  H_ = (int)appPageContentH() - 24;
  buf_ = (lv_color_t*)lvglPsramAlloc((size_t)W_ * H_ * sizeof(lv_color_t));
  if (!buf_) return false;

  canvas_ = lv_canvas_create(root_);
  lv_canvas_set_buffer(canvas_, buf_, W_, H_, LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(canvas_, LV_ALIGN_BOTTOM_MID, 0, 0);

  score_ = lv_label_create(root_);
  lv_obj_set_style_text_color(score_, lv_color_hex(kColText), LV_PART_MAIN);
  lv_obj_align(score_, LV_ALIGN_TOP_LEFT, 6, 4);

  reset();
  updateScoreLabel();
  render();

  start_btn_ = lv_btn_create(root_);
  lv_obj_set_size(start_btn_, 150, 44);
  lv_obj_align(start_btn_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(start_btn_, startCb, LV_EVENT_CLICKED, this);
  lv_obj_t* sl = lv_label_create(start_btn_);
  lv_label_set_text(sl, TR(LV_SYMBOL_PLAY "  New game"));
  lv_obj_center(sl);

  return true;
}

void PongGame::close() {
  if (timer_) { lv_timer_del(timer_); timer_ = nullptr; }
  appPageEnd(&PongGame::dismiss);
  appPageDeleteRootAsync(root_);
  root_ = nullptr; canvas_ = nullptr; score_ = nullptr; start_btn_ = nullptr;
}

// ── LVGL callbacks ────────────────────────────────────────────────────────────

void PongGame::timerCb(lv_timer_t* t) {
  auto* self = static_cast<PongGame*>(t->user_data);
  if (self) self->step();
}

void PongGame::gestureCb(lv_event_t* e) {
  auto* self = static_cast<PongGame*>(lv_event_get_user_data(e));
  if (!self) return;
  if (!self->started_) {
    self->started_ = true;
    if (self->start_btn_) { lv_obj_del(self->start_btn_); self->start_btn_ = nullptr; }
    if (!self->timer_) self->timer_ = lv_timer_create(timerCb, kTickMs, self);
    self->updateScoreLabel();
  }
  const float half = self->kPadH / 2.0f;
  switch (lv_indev_get_gesture_dir(lv_indev_get_act())) {
    case LV_DIR_TOP:
      self->py_ = clampf(self->py_ - 30, half, self->H_ - half); break;
    case LV_DIR_BOTTOM:
      self->py_ = clampf(self->py_ + 30, half, self->H_ - half); break;
    default: break;
  }
}

void PongGame::tapCb(lv_event_t* e) {
  auto* self = static_cast<PongGame*>(lv_event_get_user_data(e));
  if (!self) return;
  if (self->over_) {
    self->reset();
    self->started_ = false;
    self->updateScoreLabel();
    self->render();
    // re-show start button
    self->start_btn_ = lv_btn_create(self->root_);
    lv_obj_set_size(self->start_btn_, 150, 44);
    lv_obj_align(self->start_btn_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(self->start_btn_, startCb, LV_EVENT_CLICKED, self);
    lv_obj_t* sl = lv_label_create(self->start_btn_);
    lv_label_set_text(sl, TR(LV_SYMBOL_PLAY "  New game"));
    lv_obj_center(sl);
    if (self->timer_) { lv_timer_del(self->timer_); self->timer_ = nullptr; }
  }
}

void PongGame::startCb(lv_event_t* e) {
  auto* self = static_cast<PongGame*>(lv_event_get_user_data(e));
  if (!self || self->started_) return;
  self->started_ = true;
  if (self->start_btn_) { lv_obj_del(self->start_btn_); self->start_btn_ = nullptr; }
  if (!self->timer_) self->timer_ = lv_timer_create(timerCb, kTickMs, self);
  self->updateScoreLabel();
}
