// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// CityLocator: "which city am I in?" from an offline boundary file.
//
// The file (cities.bin, built by scripts/build/make_cities.py from Census TIGER/Line
// place boundaries) holds each place's name, bounding box and polygon rings.
// A point is inside a place when it crosses that place's rings an odd number
// of times (even-odd rule), which handles holes (county islands inside a city)
// and places made of several separate pieces.
//
// Pure C++: no Arduino calls, so it builds on a PC too; test/test_city_locator.cpp
// covers it (scripts/run-host-tests.sh).
// The caller owns the file bytes and must keep them alive while loaded.

#include <stddef.h>
#include <stdint.h>

namespace city {

enum Kind : uint8_t { City = 0, Cdp = 1 };   // Cdp = census-designated place (e.g. Orangevale)

// Parse and index the file. Returns false (and stays unloaded) on a bad file.
bool load(const uint8_t* data, size_t len);
void unload();
bool loaded();
int  count();

// Name of the place containing the point (micro-degrees), or nullptr when the
// point is outside every place. `kind` (optional) receives the place type.
const char* lookup(int32_t lat_e6, int32_t lon_e6, uint8_t* kind = nullptr);

}  // namespace city
