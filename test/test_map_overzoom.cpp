// SPDX-License-Identifier: GPL-3.0-or-later
// Map overzoom: a missing tile drawn from its ancestor (ui-touch/MapOverzoom.h).
// Build: g++ -std=c++17 -I src -o /tmp/test_map_overzoom test/test_map_overzoom.cpp
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <vector>

#include "ui-touch/MapOverzoom.h"

using namespace MapOverzoom;

// A tile whose every pixel names its own position: (x << 8 | y) fits 16 bits.
static std::vector<uint16_t> coordTile() {
  std::vector<uint16_t> t(kTile * kTile);
  for (int y = 0; y < kTile; ++y)
    for (int x = 0; x < kTile; ++x) t[(size_t)y * kTile + x] = (uint16_t)(x << 8 | y);
  return t;
}

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void testAncestorGeometry() {
  Source s;
  // z13 tile (4198, 2734): its parent is z12 (2099, 1367), the left half (even x), top half (even y).
  CHECK(ancestor(13, 4198, 2734, 1, s));
  CHECK(s.z == 12 && s.x == 2099 && s.y == 1367 && s.side == 128 && s.ox == 0 && s.oy == 0);
  // Odd x and y: the bottom-right quadrant.
  CHECK(ancestor(13, 4199, 2735, 1, s));
  CHECK(s.x == 2099 && s.y == 1367 && s.ox == 128 && s.oy == 128);
  // Two levels: z14 (8397, 5470) is z12 (2099, 1367), column 1, row 2 of a 4x4 grid of 64 px.
  CHECK(ancestor(14, 8397, 5470, 2, s));
  CHECK(s.z == 12 && s.x == 2099 && s.y == 1367 && s.side == 64 && s.ox == 64 && s.oy == 128);
  // Three levels: 32 px squares, offsets up to 224.
  CHECK(ancestor(15, 16799, 10943, 3, s));
  CHECK(s.z == 12 && s.x == 2099 && s.y == 1367 && s.side == 32 && s.ox == 7 * 32 && s.oy == 7 * 32);
  // Out of range: depth 0 / 4, a level too shallow, a negative index.
  CHECK(!ancestor(13, 1, 1, 0, s));
  CHECK(!ancestor(13, 1, 1, 4, s));
  CHECK(!ancestor(1, 1, 1, 2, s));
  CHECK(!ancestor(13, -1, 4, 1, s));
  // Every child of a parent, at every depth, lands on that parent and tiles it exactly.
  for (int k = 1; k <= kMaxDepth; ++k) {
    const int n = 1 << k;
    std::vector<int> cover(kTile * kTile, 0);
    for (int dy = 0; dy < n; ++dy)
      for (int dx = 0; dx < n; ++dx) {
        CHECK(ancestor((uint8_t)(10 + k), 40 * n + dx, 70 * n + dy, k, s));
        CHECK(s.z == 10 && s.x == 40 && s.y == 70);
        for (int r = 0; r < s.side; ++r)
          for (int c = 0; c < s.side; ++c) cover[(size_t)(s.oy + r) * kTile + s.ox + c]++;
      }
    bool exact = true;
    for (int v : cover) exact &= (v == 1);
    CHECK(exact);
  }
}

// Cut and blow up: output pixel (X, Y) must be the ancestor's (ox + X / f, oy + Y / f).
static void testCropUpscale() {
  const std::vector<uint16_t> parent = coordTile();
  for (int k = 1; k <= kMaxDepth; ++k) {
    const int n = 1 << k, f = n;
    for (int q = 0; q < n * n; ++q) {
      Source s;
      CHECK(ancestor((uint8_t)(12 + k), (2099 << k) + q % n, (1367 << k) + q / n, k, s));
      std::vector<uint16_t> buf = parent;   // decoded "in place", as the firmware does
      std::vector<uint16_t> sq(cropPixels(k));
      crop(buf.data(), s, sq.data());
      upscale(sq.data(), s.side, buf.data());
      bool ok = true;
      for (int Y = 0; Y < kTile && ok; ++Y)
        for (int X = 0; X < kTile && ok; ++X) {
          const int px = s.ox + X / f, py = s.oy + Y / f;
          ok = buf[(size_t)Y * kTile + X] == (uint16_t)(px << 8 | py);
        }
      CHECK(ok);
    }
  }
  // Wider pixels (the host's 32-bit colour) take the same path.
  std::vector<uint32_t> wide(kTile * kTile);
  for (size_t i = 0; i < wide.size(); ++i) wide[i] = (uint32_t)i * 2654435761u;
  Source s;
  CHECK(ancestor(13, 1, 0, 1, s));
  std::vector<uint32_t> sq(cropPixels(1)), out(kTile * kTile);
  crop(wide.data(), s, sq.data());
  upscale(sq.data(), s.side, out.data());
  CHECK(out[0] == wide[128] && out[1] == wide[128] && out[2] == wide[129]);
  CHECK(out[(size_t)255 * kTile + 255] == wide[(size_t)127 * kTile + 255]);
}

static void testMissRing() {
  MissRing<4> r;
  CHECK(!r.has(12, 1, 2));
  r.add(12, 1, 2);
  r.add(12, 1, 2);   // no duplicate: it must not push a different entry out
  CHECK(r.has(12, 1, 2));
  CHECK(!r.has(13, 1, 2) && !r.has(12, 2, 1));
  r.add(11, 0, 0); r.add(10, 0, 0); r.add(9, 0, 0);
  CHECK(r.has(12, 1, 2));   // four entries, all still there
  r.add(8, 0, 0);            // the fifth replaces the oldest
  CHECK(!r.has(12, 1, 2) && r.has(8, 0, 0) && r.has(9, 0, 0));
  r.clear();
  CHECK(!r.has(8, 0, 0) && !r.has(9, 0, 0));
}

int main() {
  testAncestorGeometry();
  testCropUpscale();
  testMissRing();
  if (failures) { printf("%d failure(s)\n", failures); return 1; }
  printf("test_map_overzoom: all passed\n");
  return 0;
}
