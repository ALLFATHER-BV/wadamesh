// SPDX-License-Identifier: GPL-3.0-or-later
#include "zero_term.h"
#include "zero_font.h"
#include <stdlib.h>
#include <algorithm>

int TermCanvas::cellW() { return kTermCW; }
int TermCanvas::cellH() { return kTermCH; }

void TermCanvas::fill(int x, int y, int fw, int fh, uint16_t c) {
  for (int j = std::max(0, y); j < std::min(h, y + fh); ++j)
    for (int i = std::max(0, x); i < std::min(w, x + fw); ++i) px[(size_t)j * w + i] = c;
}

void TermCanvas::blend(int x, int y, uint16_t c, int a) {
  if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
  uint16_t& d = px[(size_t)y * w + x];
  if (a >= 256) { d = c; return; }
  const int r = ((c >> 11) * a + (d >> 11) * (256 - a)) >> 8;
  const int g = (((c >> 5) & 63) * a + ((d >> 5) & 63) * (256 - a)) >> 8;
  const int b = ((c & 31) * a + (d & 31) * (256 - a)) >> 8;
  d = (uint16_t)((r << 11) | (g << 5) | b);
}

void TermCanvas::line(int x0, int y0, int x1, int y1, uint16_t c, int a) {
  const int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    blend(x0, y0, c, a);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

int TermCanvas::text(int x, int y, const char* s, uint16_t fg, int scale) {
  static const int kA[4] = {0, 96, 176, 256};
  for (; *s; ++s) {
    unsigned char ch = (unsigned char)*s;
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t* g = kTermFont[ch - 32];
    for (int j = 0; j < kTermCH; ++j)
      for (int i = 0; i < kTermCW; ++i) {
        const int a = kA[g[j * kTermCW + i]];
        if (!a) continue;
        for (int sy = 0; sy < scale; ++sy)
          for (int sx = 0; sx < scale; ++sx) blend(x + i * scale + sx, y + j * scale + sy, fg, a);
      }
    x += kTermCW * scale;
  }
  return x;
}

void TermCanvas::dimRows(int every, int keep) {
  for (int y = every - 1; y < h; y += every)
    for (int x = 0; x < w; ++x) {
      uint16_t& d = px[(size_t)y * w + x];
      const int r = ((d >> 11) * keep) >> 8, g = (((d >> 5) & 63) * keep) >> 8, b = ((d & 31) * keep) >> 8;
      d = (uint16_t)((r << 11) | (g << 5) | b);
    }
}
