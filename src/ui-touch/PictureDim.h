#pragma once
// How a picture of your own is toned down behind the text (Settings > Theme > Picture
// dimming, Settings > Lock screen > Wallpaper dimming). Pure, so the host test
// (test/test_picture_dim.cpp) checks it, above all that the default step is exactly
// the look every picture had before the setting existed.
//
// Levels: 0 off, 1 light, 2 medium, 3 strong (the default, TOUCH_DIM_STRONG).
#include <stdint.h>

namespace PictureDim {

constexpr uint8_t kLevels = 4;
constexpr uint8_t kStrong = 3;

// Background picture, by night: every channel scaled by mul/256, then the brightest
// parts held down to a luma of cap. By day: lifted toward white by lift/256, then the
// darkest parts held up to a luma of floor. Strong is the original 107 / 64, 153 / 200.
struct Night { int mul, cap; };
struct Day   { int lift, floor; };
constexpr Night kNight[kLevels] = { {256, 255}, {200, 160}, {150, 110}, {107, 64} };
constexpr Day   kDay[kLevels]   = { {0, 0},     {60, 120},  {110, 165}, {153, 200} };

inline uint8_t clampLevel(uint8_t level) { return level < kLevels ? level : kStrong; }

// One pixel of the fitted picture, channels 0..255 in c[0..2] (r, g, b), in place.
inline void bgPixel(int c[3], bool day, uint8_t level) {
  level = clampLevel(level);
  int luma;
  if (!day) {   // dimmed, and the brightest parts held down
    const Night n = kNight[level];
    for (int k = 0; k < 3; ++k) c[k] = c[k] * n.mul >> 8;
    luma = (c[0] * 54 + c[1] * 183 + c[2] * 19) >> 8;
    if (luma > n.cap) for (int k = 0; k < 3; ++k) c[k] = c[k] * n.cap / luma;
  } else {      // lifted toward white, the darkest parts held up
    const Day d = kDay[level];
    for (int k = 0; k < 3; ++k) c[k] += (255 - c[k]) * d.lift >> 8;
    luma = (c[0] * 54 + c[1] * 183 + c[2] * 19) >> 8;
    if (luma < d.floor) {
      const int f = (d.floor - luma) * 256 / (255 - luma);
      for (int k = 0; k < 3; ++k) c[k] += (255 - c[k]) * f >> 8;
    }
  }
}

// The lock-screen wallpaper's black veil, as an LVGL opacity (0..255). Strong is the
// original 105.
constexpr uint8_t kLockVeil[kLevels] = { 0, 40, 72, 105 };
inline uint8_t lockVeil(uint8_t level) { return kLockVeil[clampLevel(level)]; }

}  // namespace PictureDim
