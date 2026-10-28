// SPDX-License-Identifier: GPL-3.0-or-later
// Picture dimming / Wallpaper dimming steps (ui-touch/PictureDim.h).
// Build: g++ -std=c++17 -I src -o /tmp/test_picture_dim test/test_picture_dim.cpp
#include <stdio.h>

#include "ui-touch/PictureDim.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// bgImageFit's per-pixel step exactly as it was before the setting (UITask.cpp).
static void original(int c[3], bool day) {
  int luma;
  if (!day) {
    for (int k3 = 0; k3 < 3; ++k3) c[k3] = c[k3] * 107 >> 8;
    luma = (c[0] * 54 + c[1] * 183 + c[2] * 19) >> 8;
    if (luma > 64) for (int k3 = 0; k3 < 3; ++k3) c[k3] = c[k3] * 64 / luma;
  } else {
    for (int k3 = 0; k3 < 3; ++k3) c[k3] += (255 - c[k3]) * 153 >> 8;
    luma = (c[0] * 54 + c[1] * 183 + c[2] * 19) >> 8;
    if (luma < 200) {
      const int f = (200 - luma) * 256 / (255 - luma);
      for (int k3 = 0; k3 < 3; ++k3) c[k3] += (255 - c[k3]) * f >> 8;
    }
  }
}
static int luma(const int c[3]) { return (c[0] * 54 + c[1] * 183 + c[2] * 19) >> 8; }

int main() {
  int same = 0, total = 0;
  for (int r = 0; r < 256; r += 5)
    for (int g = 0; g < 256; g += 5)
      for (int b = 0; b < 256; b += 5)
        for (int day = 0; day < 2; ++day) {
          // Strong (the default, and what an absent key reads as) is the old look, pixel for pixel.
          int a[3] = {r, g, b}, o[3] = {r, g, b};
          PictureDim::bgPixel(a, day, PictureDim::kStrong);
          original(o, day);
          ++total;
          if (a[0] == o[0] && a[1] == o[1] && a[2] == o[2]) ++same;
          // A stored value out of range reads as Strong.
          int x[3] = {r, g, b};
          PictureDim::bgPixel(x, day, 9);
          CHECK(x[0] == o[0] && x[1] == o[1] && x[2] == o[2]);
          // Off leaves the picture as it is.
          int off[3] = {r, g, b};
          PictureDim::bgPixel(off, day, 0);
          CHECK(off[0] == r && off[1] == g && off[2] == b);
          // Every step stays in range, and each stronger step goes further from the
          // picture: never brighter by night, never darker by day.
          int prev = luma(off);
          for (uint8_t lv = 1; lv < PictureDim::kLevels; ++lv) {
            int c[3] = {r, g, b};
            PictureDim::bgPixel(c, day, lv);
            for (int k = 0; k < 3; ++k) CHECK(c[k] >= 0 && c[k] <= 255);
            const int l = luma(c);
            if (day) CHECK(l >= prev - 1); else CHECK(l <= prev + 1);   // 1 for integer rounding
            prev = l;
          }
          // Readability bounds per step: by night the brightest a pixel gets, by day the darkest.
          for (uint8_t lv = 1; lv < PictureDim::kLevels; ++lv) {
            int c[3] = {r, g, b};
            PictureDim::bgPixel(c, day, lv);
            if (!day) CHECK(luma(c) <= PictureDim::kNight[lv].cap);
            else      CHECK(luma(c) >= PictureDim::kDay[lv].floor - 2);
          }
        }
  CHECK(same == total);
  printf("strong == original on %d of %d samples\n", same, total);

  // Lock-screen veil: Strong is the old 105, Off is none, out of range reads as Strong.
  CHECK(PictureDim::lockVeil(3) == 105);
  CHECK(PictureDim::lockVeil(0) == 0);
  CHECK(PictureDim::lockVeil(200) == 105);
  CHECK(PictureDim::lockVeil(1) < PictureDim::lockVeil(2) && PictureDim::lockVeil(2) < PictureDim::lockVeil(3));

  if (failures) { printf("%d failure(s)\n", failures); return 1; }
  printf("test_picture_dim: all passed\n");
  return 0;
}
