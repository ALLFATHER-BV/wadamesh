// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the city locator's file parser and lookup (src/helpers/CityLocator.cpp):
// a place found inside its rings, holes (a county island inside a city), a place in two
// separate pieces, the point outside every place, cities listed before CDPs winning an
// overlap, the 63-byte name cap, and every truncated or wrong-header file refused.
// Build + run from the repo root (scripts/run-host-tests.sh does this):
//   c++ -std=c++17 -Wall -Wextra -I src test/test_city_locator.cpp -o /tmp/t && /tmp/t
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "helpers/CityLocator.h"
#include "helpers/CityLocator.cpp"   // compiled in, so the default build line needs no extra source

namespace {

struct Pt { double lat, lon; };
using Ring = std::vector<Pt>;
struct Place { std::string name; uint8_t kind; std::vector<Ring> rings; };

void put16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
void put32(std::vector<uint8_t>& b, int32_t v) {
  const uint32_t u = (uint32_t)v;
  for (int i = 0; i < 4; ++i) b.push_back((u >> (8 * i)) & 0xFF);
}
int32_t e6(double v) { return (int32_t)(v * 1e6 + (v < 0 ? -0.5 : 0.5)); }

// Same layout scripts/build/make_cities.py writes (format v1).
std::vector<uint8_t> build(const std::vector<Place>& places) {
  std::vector<uint8_t> b = {'W', 'C', 'T', 'Y', 1, 0};
  put16(b, (uint16_t)places.size());
  for (const Place& p : places) {
    double la0 = 90, lo0 = 180, la1 = -90, lo1 = -180;
    for (const Ring& r : p.rings)
      for (const Pt& q : r) {
        if (q.lat < la0) la0 = q.lat;
        if (q.lat > la1) la1 = q.lat;
        if (q.lon < lo0) lo0 = q.lon;
        if (q.lon > lo1) lo1 = q.lon;
      }
    b.push_back(p.kind);
    b.push_back((uint8_t)p.name.size());
    b.insert(b.end(), p.name.begin(), p.name.end());
    put32(b, e6(la0)); put32(b, e6(lo0)); put32(b, e6(la1)); put32(b, e6(lo1));
    put16(b, (uint16_t)p.rings.size());
    for (const Ring& r : p.rings) {
      put16(b, (uint16_t)r.size());
      for (const Pt& q : r) { put32(b, e6(q.lat)); put32(b, e6(q.lon)); }
    }
  }
  return b;
}

Ring square(double lat0, double lon0, double lat1, double lon1) {
  return {{lat0, lon0}, {lat0, lon1}, {lat1, lon1}, {lat1, lon0}, {lat0, lon0}};
}

const char* at(double lat, double lon, uint8_t* kind = nullptr) {
  return city::lookup(e6(lat), e6(lon), kind);
}

bool is(const char* got, const char* want) { return got && strcmp(got, want) == 0; }

}  // namespace

int main() {
  // Cities first, then CDPs: the order make_cities.py sorts them in.
  std::vector<Place> places = {
      // Roseville with a county island (hole) in its middle.
      {"Roseville", 0, {square(38.70, -121.35, 38.80, -121.20), square(38.74, -121.30, 38.76, -121.25)}},
      // A city in two separate pieces.
      {"Two Piece", 0, {square(39.00, -121.00, 39.10, -120.90), square(39.20, -121.00, 39.30, -120.90)}},
      // A triangle, so the edge crossing math is not only axis-aligned.
      {"Triangle", 0, {{{40.0, -120.0}, {40.0, -119.0}, {41.0, -119.5}, {40.0, -120.0}}}},
      // A CDP overlapping Roseville's east edge: the city, listed first, must win there.
      {"Granite Bay", 1, {square(38.70, -121.25, 38.80, -121.10)}},
      // A name longer than the 63 bytes lookup() hands back.
      {std::string(70, 'N'), 1, {square(-10.0, 10.0, -9.0, 11.0)}},
  };
  std::vector<uint8_t> file = build(places);

  assert(!city::loaded());
  assert(city::lookup(0, 0) == nullptr);   // nothing loaded: no answer, no crash
  assert(city::load(file.data(), file.size()));
  assert(city::loaded() && city::count() == 5);

  uint8_t kind = 99;
  assert(is(at(38.72, -121.33, &kind), "Roseville") && kind == 0);
  assert(at(38.75, -121.28) == nullptr);                  // inside the hole
  assert(is(at(38.745, -121.32), "Roseville"));           // beside the hole
  assert(is(at(39.05, -120.95), "Two Piece"));
  assert(is(at(39.25, -120.95), "Two Piece"));
  assert(at(39.15, -120.95) == nullptr);                  // between the pieces
  assert(is(at(40.2, -119.5), "Triangle"));
  assert(at(40.9, -119.9) == nullptr);                    // inside the box, outside the triangle
  assert(is(at(38.75, -121.22), "Roseville"));            // overlap: city before CDP
  assert(is(at(38.75, -121.15, &kind), "Granite Bay") && kind == 1);
  const char* longname = at(-9.5, 10.5);
  assert(longname && strlen(longname) == 63);
  assert(at(0.0, 0.0) == nullptr);                        // outside every place

  // Every truncation of a good file is refused, and a refused load leaves nothing loaded.
  for (size_t n = 0; n < file.size(); ++n) {
    assert(!city::load(file.data(), n));
    assert(!city::loaded() && city::lookup(e6(38.72), e6(-121.33)) == nullptr);
  }
  std::vector<uint8_t> bad = file;
  bad[0] = 'X';
  assert(!city::load(bad.data(), bad.size()));            // wrong magic
  bad = file;
  bad[4] = 2;
  assert(!city::load(bad.data(), bad.size()));            // unknown version
  bad = file;
  bad[6] = bad[7] = 0;
  assert(!city::load(bad.data(), bad.size()));            // zero places
  assert(!city::load(nullptr, 100));

  // Reloads cleanly after a refusal; unload() drops it.
  assert(city::load(file.data(), file.size()) && is(at(38.72, -121.33), "Roseville"));
  city::unload();
  assert(!city::loaded() && city::lookup(e6(38.72), e6(-121.33)) == nullptr);

  printf("city locator: all tests passed\n");
  return 0;
}
