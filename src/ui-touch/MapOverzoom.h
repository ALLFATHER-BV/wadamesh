#pragma once
// Map overzoom: the pure parts (no LVGL, no files), so test/test_map_overzoom.cpp can
// run them on the host.
//
// A tile that no offline source holds, and that no download can bring (Wi-Fi down), is
// drawn from its nearest ancestor instead: tile z/x/y covers a (256 >> k) px square of
// tile (z-k)/(x>>k)/(y>>k), at offset ((x mod 2^k), (y mod 2^k)) * (256 >> k). That
// square is cut out and every pixel of it repeated 2^k times each way, so the slot shows
// the right part of the map, only blockier. Nothing here allocates: the caller owns the
// 256x256 tile buffer and a crop buffer of cropPixels(k) pixels.
#include <stdint.h>
#include <string.h>

namespace MapOverzoom {

constexpr int kTile     = 256;   // tile side, px
constexpr int kMaxDepth = 3;     // levels up we look: 2x, 4x, 8x

struct Source {
  uint8_t z;        // ancestor zoom
  int32_t x, y;     // ancestor tile
  int     ox, oy;   // top-left of the covering square inside the ancestor, px
  int     side;     // its side, px (256 >> k)
};

// The ancestor k levels up that covers z/x/y, or false when there is none (k out of
// range, z too small, a negative tile index).
inline bool ancestor(uint8_t z, int32_t x, int32_t y, int k, Source& out) {
  if (k < 1 || k > kMaxDepth || k > (int)z || x < 0 || y < 0) return false;
  const int32_t mask = (1 << k) - 1;
  out.z    = (uint8_t)(z - k);
  out.x    = x >> k;
  out.y    = y >> k;
  out.side = kTile >> k;
  out.ox   = (int)(x & mask) * out.side;
  out.oy   = (int)(y & mask) * out.side;
  return true;
}

inline int cropPixels(int k) { return (kTile >> k) * (kTile >> k); }

// Copy the covering square out of a decoded 256x256 ancestor (P: the pixel type).
template <typename P>
inline void crop(const P* tile, const Source& s, P* out) {
  for (int r = 0; r < s.side; ++r)
    memcpy(out + (size_t)r * s.side, tile + (size_t)(s.oy + r) * kTile + s.ox, (size_t)s.side * sizeof(P));
}

// Nearest-neighbour blow-up of a side x side square to the full 256x256 tile.
// `out` may be the very buffer the ancestor was decoded into (the crop is a copy).
template <typename P>
inline void upscale(const P* sq, int side, P* out) {
  const int f = kTile / side;
  for (int r = 0; r < side; ++r) {
    P* row = out + (size_t)r * f * kTile;
    const P* src = sq + (size_t)r * side;
    for (int c = 0; c < side; ++c) {
      const P p = src[c];
      for (int i = 0; i < f; ++i) row[c * f + i] = p;
    }
    for (int i = 1; i < f; ++i) memcpy(row + (size_t)i * kTile, row, kTile * sizeof(P));
  }
}

// Ancestors already found missing, so a pan or a re-render does not ask the storage
// for the same absent tile again. A small ring, cleared whenever the tile source can
// have changed (new card, other archive, map style, a finished download).
template <int N>
struct MissRing {
  struct E { int32_t x, y; uint8_t z; bool used; };
  E   e[N] = {};
  int head = 0;
  void clear() { for (int i = 0; i < N; ++i) e[i].used = false; head = 0; }
  bool has(uint8_t z, int32_t x, int32_t y) const {
    for (int i = 0; i < N; ++i)
      if (e[i].used && e[i].z == z && e[i].x == x && e[i].y == y) return true;
    return false;
  }
  void add(uint8_t z, int32_t x, int32_t y) {
    if (has(z, x, y)) return;
    e[head] = { x, y, z, true };
    head = (head + 1) % N;
  }
};

}  // namespace MapOverzoom
