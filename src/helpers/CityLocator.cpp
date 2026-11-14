// SPDX-License-Identifier: GPL-3.0-or-later
#include "CityLocator.h"

#include <stdlib.h>
#include <string.h>

namespace city {
namespace {

struct Place {
  int32_t min_lat, min_lon, max_lat, max_lon;
  uint32_t rings_off;   // offset of the first ring's point count
  uint16_t ring_count;
  uint8_t  kind;
  uint8_t  name_len;
  uint32_t name_off;
};

const uint8_t* s_data = nullptr;
size_t   s_len = 0;
Place*   s_places = nullptr;
int      s_count = 0;
char     s_name[64];

inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline int32_t  rd32(const uint8_t* p) {
  return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

// Even-odd crossing count of a horizontal ray from (lat, lon) towards +lon,
// over every ring of one place. Odd = inside.
bool insidePlace(const Place& pl, int32_t lat, int32_t lon) {
  const uint8_t* p = s_data + pl.rings_off;
  bool inside = false;
  for (uint16_t r = 0; r < pl.ring_count; ++r) {
    const uint16_t n = rd16(p); p += 2;
    if (n >= 2) {
      int32_t alat = rd32(p + (size_t)(n - 1) * 8), alon = rd32(p + (size_t)(n - 1) * 8 + 4);
      for (uint16_t i = 0; i < n; ++i) {
        const int32_t blat = rd32(p + (size_t)i * 8), blon = rd32(p + (size_t)i * 8 + 4);
        if ((alat > lat) != (blat > lat)) {
          const double x = (double)(blon - alon) * (double)(lat - alat) / (double)(blat - alat) + (double)alon;
          if ((double)lon < x) inside = !inside;
        }
        alat = blat; alon = blon;
      }
    }
    p += (size_t)n * 8;
  }
  return inside;
}

}  // namespace

void unload() {
  free(s_places);
  s_places = nullptr;
  s_count = 0;
  s_data = nullptr;
  s_len = 0;
}

bool loaded() { return s_places != nullptr; }
int  count()  { return s_count; }

bool load(const uint8_t* data, size_t len) {
  unload();
  if (!data || len < 8 || memcmp(data, "WCTY", 4) != 0 || data[4] != 1) return false;
  const int n = rd16(data + 6);
  if (n == 0) return false;
  Place* places = (Place*)malloc(sizeof(Place) * (size_t)n);
  if (!places) return false;
  size_t off = 8;
  for (int i = 0; i < n; ++i) {
    if (off + 2 > len) { free(places); return false; }
    Place& pl = places[i];
    pl.kind = data[off];
    pl.name_len = data[off + 1];
    off += 2;
    pl.name_off = (uint32_t)off;
    off += pl.name_len;
    if (off + 18 > len) { free(places); return false; }
    pl.min_lat = rd32(data + off);      pl.min_lon = rd32(data + off + 4);
    pl.max_lat = rd32(data + off + 8);  pl.max_lon = rd32(data + off + 12);
    pl.ring_count = rd16(data + off + 16);
    off += 18;
    pl.rings_off = (uint32_t)off;
    for (uint16_t r = 0; r < pl.ring_count; ++r) {   // walk (and bounds-check) every ring
      if (off + 2 > len) { free(places); return false; }
      const size_t pts = rd16(data + off);
      off += 2 + pts * 8;
      if (off > len) { free(places); return false; }
    }
  }
  s_data = data;
  s_len = len;
  s_places = places;
  s_count = n;
  return true;
}

const char* lookup(int32_t lat, int32_t lon, uint8_t* kind) {
  for (int i = 0; i < s_count; ++i) {
    const Place& pl = s_places[i];
    if (lat < pl.min_lat || lat > pl.max_lat || lon < pl.min_lon || lon > pl.max_lon) continue;
    if (!insidePlace(pl, lat, lon)) continue;
    const size_t nl = pl.name_len < sizeof(s_name) - 1 ? pl.name_len : sizeof(s_name) - 1;
    memcpy(s_name, s_data + pl.name_off, nl);
    s_name[nl] = '\0';
    if (kind) *kind = pl.kind;
    return s_name;
  }
  return nullptr;
}

}  // namespace city
