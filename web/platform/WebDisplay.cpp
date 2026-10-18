// SPDX-License-Identifier: GPL-3.0-or-later
#include "WebDisplay.h"
#include <stdlib.h>
#include <string.h>

int g_wada_web_w = 800, g_wada_web_h = 480;
extern WebDisplay display;   // board.cpp
void wadaWebSetSize(int w, int h) {
  if (w < 240 || h < 240 || w > 2048 || h > 2048) return;
  g_wada_web_w = w;
  g_wada_web_h = h;
  display.resize(w, h);
}

WebDisplay::WebDisplay() : DisplayDriver(800, 480) {
  _fb = (uint16_t*)calloc((size_t)800 * 480, sizeof(uint16_t));
}
void WebDisplay::resize(int w, int h) {
  free(_fb);
  _fb = (uint16_t*)calloc((size_t)w * h, sizeof(uint16_t));
  setLogicalSize(w, h);
  resetBox();
  addBox(0, 0, w, h);
}
void WebDisplay::clear() {
  memset(_fb, 0, (size_t)WADA_WEB_W * WADA_WEB_H * sizeof(uint16_t));
  addBox(0, 0, WADA_WEB_W, WADA_WEB_H);
}
void WebDisplay::fillRect(int x, int y, int w, int h) {
  for (int r = y; r < y + h; ++r) {
    if (r < 0 || r >= WADA_WEB_H) continue;
    for (int c = x; c < x + w; ++c) if (c >= 0 && c < WADA_WEB_W) _fb[(size_t)r * WADA_WEB_W + c] = _color;
  }
  addBox(x, y, x + w, y + h);
}
void WebDisplay::drawRect(int x, int y, int w, int h) {
  fillRect(x, y, w, 1); fillRect(x, y + h - 1, w, 1);
  fillRect(x, y, 1, h); fillRect(x + w - 1, y, 1, h);
}
uint16_t WebDisplay::getTextWidth(const char* str) { return str ? (uint16_t)(strlen(str) * 6) : 0; }
void WebDisplay::writePixelsRGB565(int x, int y, int w, int h, const uint16_t* px) {
  if (!px || w <= 0 || h <= 0) return;
  for (int r = 0; r < h; ++r) {
    const int dy = y + r;
    if (dy < 0 || dy >= WADA_WEB_H) continue;
    int x0 = x, cw = w, sx = 0;
    if (x0 < 0) { sx = -x0; cw += x0; x0 = 0; }
    if (x0 + cw > WADA_WEB_W) cw = WADA_WEB_W - x0;
    if (cw > 0) memcpy(_fb + (size_t)dy * WADA_WEB_W + x0, px + (size_t)r * w + sx, (size_t)cw * sizeof(uint16_t));
  }
  addBox(x, y, x + w, y + h);
}
