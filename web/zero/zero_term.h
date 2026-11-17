// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// A tiny RGB565 canvas with a 7x13 antialiased terminal font (zero_font.h), for the
// Cardputer Zero's own screens drawn outside the UI: the boot log, the system HUD, the
// key help and the Sac Valley easter egg.
#include <stdint.h>
#include <string>
#include <vector>

constexpr uint16_t rgb565(uint32_t rgb) {
  return (uint16_t)((((rgb >> 16) & 0xFF) >> 3) << 11 | (((rgb >> 8) & 0xFF) >> 2) << 5 | ((rgb & 0xFF) >> 3));
}

struct TermCanvas {
  int w = 320, h = 170;
  std::vector<uint16_t> px;
  void resize(int W, int H) { w = W; h = H; px.assign((size_t)w * h, 0); }
  void clear(uint16_t c) { std::fill(px.begin(), px.end(), c); }
  void fill(int x, int y, int fw, int fh, uint16_t c);
  void pixel(int x, int y, uint16_t c) { if (x >= 0 && y >= 0 && x < w && y < h) px[(size_t)y * w + x] = c; }
  void blend(int x, int y, uint16_t c, int a256);   // a256: 0..256 coverage
  void line(int x0, int y0, int x1, int y1, uint16_t c, int a256 = 256);
  // Text at a pixel position; returns the x after it. scale 1 or 2 (blocky 2x for banners).
  int text(int x, int y, const char* s, uint16_t fg, int scale = 1);
  static int cellW() ;
  static int cellH() ;
  void dimRows(int every, int keep256);   // scanlines: every Nth row at keep256/256
};
