// SPDX-License-Identifier: GPL-3.0-or-later
#include "TetrisGame.h"

#include <Arduino.h>
#include <LvglPsramAlloc.h>
#include <string.h>

static constexpr lv_coord_t kTopBar = 22;

static constexpr uint32_t kColBg      = 0x090B0F;
static constexpr uint32_t kColPanel   = 0x131922;
static constexpr uint32_t kColText    = 0xE6EAEE;
static constexpr uint32_t kColSub     = 0x9AA3AD;
static constexpr uint32_t kColGrid    = 0x24303E;
static constexpr uint32_t kColBorder  = 0x4E5D71;
static constexpr uint32_t kColGhost   = 0x141A22;
static constexpr uint32_t kColPieces[8] = {
  0x000000, 0x4AC7EA, 0x5C78FF, 0xF1A643, 0xF6D74A,
  0x58C96B, 0xC06BEA, 0xE65B5B
};

static constexpr uint8_t kShapes[7][4][4] = {
  { {0,0,0,0}, {1,1,1,1}, {0,0,0,0}, {0,0,0,0} }, // I
  { {1,0,0,0}, {1,1,1,0}, {0,0,0,0}, {0,0,0,0} }, // J
  { {0,0,1,0}, {1,1,1,0}, {0,0,0,0}, {0,0,0,0} }, // L
  { {1,1,0,0}, {1,1,0,0}, {0,0,0,0}, {0,0,0,0} }, // O
  { {0,1,1,0}, {1,1,0,0}, {0,0,0,0}, {0,0,0,0} }, // S
  { {0,1,0,0}, {1,1,1,0}, {0,0,0,0}, {0,0,0,0} }, // T
  { {1,1,0,0}, {0,1,1,0}, {0,0,0,0}, {0,0,0,0} }, // Z
};

TetrisGame* TetrisGame::s_active = nullptr;

bool TetrisGame::isOpen() { return s_active != nullptr && s_active->root_ != nullptr; }

void TetrisGame::launch() {
  if (s_active) return;
  TetrisGame* g = new TetrisGame();
  s_active = g;
  if (!g->open()) {
    s_active = nullptr;
    delete g;
  }
}

void TetrisGame::steer(int dx, int dy) {
  if (!s_active || !s_active->started_ || s_active->paused_ || s_active->over_) return;
  if (dx == 0 && dy == 0) return;
  const int adx = dx < 0 ? -dx : dx;
  const int ady = dy < 0 ? -dy : dy;
  if (adx >= ady) {
    s_active->movePiece(dx > 0 ? 1 : -1, 0);
    s_active->render();
  } else if (dy < 0) {
    s_active->rotatePiece();
    s_active->render();
  } else {
    s_active->softDrop();
  }
}

bool TetrisGame::open() {
  const lv_coord_t sw = lv_disp_get_hor_res(nullptr);
  const lv_coord_t sh = lv_disp_get_ver_res(nullptr);
  const int overlayH = (int)(sh - kTopBar);
  const int topRowH = 28;
  const int sidePanelW = (sw >= 300) ? 84 : 74;
  int cell = (int)(sw - sidePanelW - 20) / kFieldW;
  const int byH = (overlayH - topRowH - 10) / kFieldH;
  if (byH < cell) cell = byH;
  if (cell > kMaxCellPx) cell = kMaxCellPx;
  if (cell < 8) cell = 8;
  cellPx_ = cell;
  boardWpx_ = kFieldW * cellPx_;
  boardHpx_ = kFieldH * cellPx_;
  boardX_ = 8;
  boardY_ = topRowH + 4;
  previewX_ = boardX_ + boardWpx_ + 10;
  previewY_ = boardY_ + 18;

  root_ = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(root_);
  lv_obj_set_size(root_, sw, overlayH);
  lv_obj_set_pos(root_, 0, kTopBar);
  lv_obj_set_style_bg_color(root_, lv_color_hex(0x0E1216), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(root_, tapCb, LV_EVENT_CLICKED, this);

  score_ = lv_label_create(root_);
  lv_obj_set_style_text_color(score_, lv_color_hex(kColText), LV_PART_MAIN);
  lv_obj_align(score_, LV_ALIGN_TOP_LEFT, 6, 4);

  lv_obj_t* x = lv_btn_create(root_);
  lv_obj_set_size(x, 30, 24);
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -4, 0);
  lv_obj_add_event_cb(x, closeCb, LV_EVENT_CLICKED, this);
  lv_obj_t* xl = lv_label_create(x);
  lv_label_set_text(xl, LV_SYMBOL_CLOSE);
  lv_obj_center(xl);

  pause_btn_ = lv_btn_create(root_);
  lv_obj_set_size(pause_btn_, 30, 24);
  lv_obj_align(pause_btn_, LV_ALIGN_TOP_RIGHT, -40, 0);
  lv_obj_add_event_cb(pause_btn_, pauseCb, LV_EVENT_CLICKED, this);
  pause_lbl_ = lv_label_create(pause_btn_);
  lv_label_set_text(pause_lbl_, LV_SYMBOL_PAUSE);
  lv_obj_center(pause_lbl_);
  lv_obj_add_flag(pause_btn_, LV_OBJ_FLAG_HIDDEN);

  boardBuf_ = (lv_color_t*)lvglPsramAlloc((size_t)boardWpx_ * boardHpx_ * sizeof(lv_color_t));
  prevBuf_ = (lv_color_t*)lvglPsramAlloc((size_t)(kPreviewW * cellPx_) * (kPreviewH * cellPx_) * sizeof(lv_color_t));
  if (!boardBuf_ || !prevBuf_) return false;

  board_ = lv_canvas_create(root_);
  lv_canvas_set_buffer(board_, boardBuf_, boardWpx_, boardHpx_, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(board_, boardX_, boardY_);

  preview_ = lv_canvas_create(root_);
  lv_canvas_set_buffer(preview_, prevBuf_, kPreviewW * cellPx_, kPreviewH * cellPx_, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(preview_, previewX_, previewY_);

  nextLbl_ = lv_label_create(root_);
  lv_label_set_text(nextLbl_, "NEXT");
  lv_obj_set_style_text_color(nextLbl_, lv_color_hex(kColSub), LV_PART_MAIN);
  lv_obj_set_style_text_font(nextLbl_, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_pos(nextLbl_, previewX_, boardY_);

  start_btn_ = lv_btn_create(root_);
  lv_obj_set_size(start_btn_, 150, 44);
  lv_obj_align(start_btn_, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_event_cb(start_btn_, startCb, LV_EVENT_CLICKED, this);
  lv_obj_t* sl = lv_label_create(start_btn_);
  lv_label_set_text(sl, LV_SYMBOL_PLAY "  New game");
  lv_obj_center(sl);

  beginRound();
  started_ = false;
  paused_ = false;
  over_ = false;
  updateScoreLabel();
  render();
  if (!timer_) timer_ = lv_timer_create(timerCb, kTickMs, this);
  return true;
}

void TetrisGame::close() {
  if (timer_) { lv_timer_del(timer_); timer_ = nullptr; }
  if (root_)  { lv_obj_del(root_); root_ = nullptr; }
  if (boardBuf_) { lvglPsramFree(boardBuf_); boardBuf_ = nullptr; }
  if (prevBuf_)  { lvglPsramFree(prevBuf_);  prevBuf_  = nullptr; }
  board_ = nullptr;
  preview_ = nullptr;
  score_ = nullptr;
  nextLbl_ = nullptr;
  start_btn_ = nullptr;
  pause_btn_ = nullptr;
  pause_lbl_ = nullptr;
}

void TetrisGame::beginRound() {
  memset(field_, 0, sizeof(field_));
  scoreVal_ = 0;
  linesCleared_ = 0;
  dropIntervalMs_ = 500;
  paused_ = false;
  over_ = false;
  nextType_ = (int)random(0, 7);
  spawnPiece();
  lastDropMs_ = millis();
}

void TetrisGame::startGame() {
  started_ = true;
  beginRound();
  if (start_btn_) {
    lv_obj_del(start_btn_);
    start_btn_ = nullptr;
  }
  if (pause_btn_) {
    lv_obj_clear_flag(pause_btn_, LV_OBJ_FLAG_HIDDEN);
    if (pause_lbl_) lv_label_set_text(pause_lbl_, LV_SYMBOL_PAUSE);
  }
  updateScoreLabel();
  render();
}

void TetrisGame::updateScoreLabel() {
  if (!score_) return;
  if (over_) {
    lv_label_set_text_fmt(score_, "Game over - score %d (tap to restart)", scoreVal_);
  } else if (paused_) {
    lv_label_set_text_fmt(score_, "Paused - score %d", scoreVal_);
  } else if (started_) {
    lv_label_set_text_fmt(score_, "score %d   lines %d", scoreVal_, linesCleared_);
  } else {
    lv_label_set_text(score_, "Tetris - swipe L/R, up rotate, down drop");
  }
}

int TetrisGame::getBlock(int type, int rot, int x, int y) const {
  switch (rot & 3) {
    case 0: return kShapes[type][y][x];
    case 1: return kShapes[type][3 - x][y];
    case 2: return kShapes[type][3 - y][3 - x];
    case 3: return kShapes[type][x][3 - y];
  }
  return 0;
}

bool TetrisGame::checkCollision(int x, int y, int rot) const {
  for (int py = 0; py < 4; ++py) {
    for (int px = 0; px < 4; ++px) {
      if (!getBlock(currentType_, rot, px, py)) continue;
      const int fx = x + px;
      const int fy = y + py;
      if (fx < 0 || fx >= kFieldW || fy >= kFieldH) return true;
      if (fy >= 0 && field_[fy][fx] != 0) return true;
    }
  }
  return false;
}

bool TetrisGame::movePiece(int dx, int dy) {
  if (checkCollision(currentX_ + dx, currentY_ + dy, currentRot_)) return false;
  currentX_ += dx;
  currentY_ += dy;
  return true;
}

void TetrisGame::rotatePiece() {
  const int nextRot = (currentRot_ + 1) & 3;
  if (!checkCollision(currentX_, currentY_, nextRot)) currentRot_ = nextRot;
}

void TetrisGame::spawnPiece() {
  currentType_ = nextType_;
  nextType_ = (int)random(0, 7);
  currentRot_ = 0;
  currentX_ = (kFieldW / 2) - 2;
  currentY_ = -1;
}

void TetrisGame::lockPiece() {
  for (int py = 0; py < 4; ++py) {
    for (int px = 0; px < 4; ++px) {
      if (!getBlock(currentType_, currentRot_, px, py)) continue;
      const int fx = currentX_ + px;
      const int fy = currentY_ + py;
      if (fy >= 0 && fy < kFieldH && fx >= 0 && fx < kFieldW)
        field_[fy][fx] = (uint8_t)(currentType_ + 1);
    }
  }
}

void TetrisGame::clearLines() {
  int cleared = 0;
  for (int y = kFieldH - 1; y >= 0; --y) {
    bool full = true;
    for (int x = 0; x < kFieldW; ++x) {
      if (field_[y][x] == 0) { full = false; break; }
    }
    if (!full) continue;
    for (int row = y; row > 0; --row) memcpy(field_[row], field_[row - 1], sizeof(field_[row]));
    memset(field_[0], 0, sizeof(field_[0]));
    ++cleared;
    ++y;
  }
  if (cleared > 0) {
    linesCleared_ += cleared;
    scoreVal_ += cleared * 100;
    const int speedStep = (scoreVal_ / 500) * 50;
    dropIntervalMs_ = 500 - speedStep;
    if (dropIntervalMs_ < 100) dropIntervalMs_ = 100;
  }
}

void TetrisGame::settlePiece() {
  lockPiece();
  clearLines();
  spawnPiece();
  if (checkCollision(currentX_, currentY_, currentRot_)) over_ = true;
  updateScoreLabel();
  render();
}

void TetrisGame::softDrop() {
  if (!started_ || paused_ || over_) return;
  if (!movePiece(0, 1)) settlePiece();
  else render();
  lastDropMs_ = millis();
}

void TetrisGame::step() {
  if (!started_ || paused_ || over_) return;
  const uint32_t now = millis();
  if ((uint32_t)(now - lastDropMs_) < (uint32_t)dropIntervalMs_) return;
  lastDropMs_ = now;
  if (!movePiece(0, 1)) settlePiece();
  else render();
}

void TetrisGame::render() {
  if (!board_ || !preview_) return;

  lv_canvas_fill_bg(board_, lv_color_hex(kColBg), LV_OPA_COVER);
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.radius = 1;

  for (int y = 0; y < kFieldH; ++y) {
    for (int x = 0; x < kFieldW; ++x) {
      const int px = x * cellPx_;
      const int py = y * cellPx_;
      d.bg_color = lv_color_hex(field_[y][x] ? kColPieces[field_[y][x]] : kColGhost);
      d.bg_opa = field_[y][x] ? LV_OPA_COVER : LV_OPA_50;
      d.border_width = 1;
      d.border_color = lv_color_hex(field_[y][x] ? 0xFFFFFF : kColGrid);
      d.border_opa = field_[y][x] ? LV_OPA_30 : LV_OPA_20;
      lv_canvas_draw_rect(board_, px, py, cellPx_ - 1, cellPx_ - 1, &d);
    }
  }

  if (!over_) {
    for (int py = 0; py < 4; ++py) {
      for (int px = 0; px < 4; ++px) {
        if (!getBlock(currentType_, currentRot_, px, py)) continue;
        const int fx = currentX_ + px;
        const int fy = currentY_ + py;
        if (fy < 0 || fx < 0 || fx >= kFieldW || fy >= kFieldH) continue;
        d.bg_color = lv_color_hex(kColPieces[currentType_ + 1]);
        d.bg_opa = LV_OPA_COVER;
        d.border_width = 1;
        d.border_color = lv_color_hex(0xFFFFFF);
        d.border_opa = LV_OPA_40;
        lv_canvas_draw_rect(board_, fx * cellPx_, fy * cellPx_, cellPx_ - 1, cellPx_ - 1, &d);
      }
    }
  }

  lv_canvas_fill_bg(preview_, lv_color_hex(kColPanel), LV_OPA_COVER);
  d.bg_opa = LV_OPA_COVER;
  d.border_width = 1;
  d.border_color = lv_color_hex(kColBorder);
  d.border_opa = LV_OPA_30;
  for (int py = 0; py < 4; ++py) {
    for (int px = 0; px < 4; ++px) {
      if (!kShapes[nextType_][py][px]) continue;
      d.bg_color = lv_color_hex(kColPieces[nextType_ + 1]);
      lv_canvas_draw_rect(preview_, px * cellPx_, py * cellPx_, cellPx_ - 1, cellPx_ - 1, &d);
    }
  }
}

void TetrisGame::timerCb(lv_timer_t* t) {
  auto* self = static_cast<TetrisGame*>(t->user_data);
  if (self) self->step();
}

void TetrisGame::tapCb(lv_event_t* e) {
  auto* self = static_cast<TetrisGame*>(lv_event_get_user_data(e));
  if (!self) return;
  if (self->over_) {
    self->beginRound();
    self->started_ = true;
    if (self->pause_btn_) {
      lv_obj_clear_flag(self->pause_btn_, LV_OBJ_FLAG_HIDDEN);
      if (self->pause_lbl_) lv_label_set_text(self->pause_lbl_, LV_SYMBOL_PAUSE);
    }
    self->updateScoreLabel();
    self->render();
  } else if (self->started_ && !self->paused_) {
    self->rotatePiece();
    self->render();
  }
}

void TetrisGame::startCb(lv_event_t* e) {
  auto* self = static_cast<TetrisGame*>(lv_event_get_user_data(e));
  if (self) self->startGame();
}

void TetrisGame::pauseCb(lv_event_t* e) {
  auto* self = static_cast<TetrisGame*>(lv_event_get_user_data(e));
  if (!self || !self->started_ || self->over_) return;
  self->paused_ = !self->paused_;
  if (self->pause_lbl_) lv_label_set_text(self->pause_lbl_, self->paused_ ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  self->updateScoreLabel();
}

void TetrisGame::closeCb(lv_event_t* e) {
  auto* self = static_cast<TetrisGame*>(lv_event_get_user_data(e));
  if (!self) return;
  self->close();
  if (s_active == self) s_active = nullptr;
  delete self;
}
