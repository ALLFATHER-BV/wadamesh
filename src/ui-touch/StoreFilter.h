// SPDX-License-Identifier: GPL-3.0-or-later
// Store > Apps search and sort (#863). Plain C strings, no LVGL, so the host test
// (test/test_store_filter.cpp) covers exactly what the device runs.
//
// The catalog array is never reordered: callbacks carry the catalog index, and the
// update count and re-parse assume catalog order. Sorting works on a view of indices.
#pragma once
#include <stdint.h>
#include <string.h>

#include <algorithm>

namespace storefilter {

enum SortMode : uint8_t { SORT_AZ = 0, SORT_ZA = 1, SORT_NEWEST = 2, SORT_COUNT = 3 };

inline uint8_t nextSort(uint8_t m) { return (uint8_t)((m + 1) % SORT_COUNT); }

// ASCII-only folding: UTF-8 multi-byte sequences compare byte for byte.
inline char fold(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c; }

// Case-insensitive substring test. An empty needle matches everything.
inline bool containsFold(const char* hay, const char* needle) {
  if (!needle || !needle[0]) return true;
  if (!hay) return false;
  for (const char* h = hay; *h; ++h) {
    const char* a = h;
    const char* b = needle;
    while (*a && *b && fold(*a) == fold(*b)) { ++a; ++b; }
    if (!*b) return true;
  }
  return false;
}

// Copy `src` into `dst` without leading or trailing spaces, so a query of only
// spaces reads as no query at all.
inline void trimQuery(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  dst[0] = 0;
  if (!src) return;
  while (*src == ' ' || *src == '\t') ++src;
  size_t n = strlen(src);
  while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t')) --n;
  if (n >= cap) n = cap - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

// Catalog cards match on name, id and description; sideloads pass desc = nullptr.
inline bool appMatches(const char* name, const char* id, const char* desc, const char* q) {
  if (!q || !q[0]) return true;
  return containsFold(name, q) || containsFold(id, q) || containsFold(desc, q);
}

inline int cmpNameFold(const char* a, const char* b) {
  if (!a) a = "";
  if (!b) b = "";
  for (;; ++a, ++b) {
    const unsigned char x = (unsigned char)fold(*a), y = (unsigned char)fold(*b);
    if (x != y) return x < y ? -1 : 1;
    if (!x) return 0;
  }
}

// "YYYY-MM-DD" shape. ISO dates then order correctly with strcmp.
inline bool validDate(const char* d) {
  if (!d) return false;
  for (int i = 0; i < 10; ++i) {
    const char c = d[i];
    if (i == 4 || i == 7) { if (c != '-') return false; }
    else if (c < '0' || c > '9') return false;
  }
  return d[10] == 0;
}

struct SortKey {
  const char* name;
  const char* released;   // may be empty or malformed: sorts last under Newest
  bool        update;     // installed and the catalog ver is newer: pinned on top
};

// Strict weak ordering for one mode. Updates come first in every mode, and the
// pinned group follows the active sort inside itself.
inline bool before(const SortKey& a, const SortKey& b, uint8_t mode) {
  if (a.update != b.update) return a.update;
  if (mode == SORT_NEWEST) {
    const bool da = validDate(a.released), db = validDate(b.released);
    if (da != db) return da;
    if (da) {
      const int c = strcmp(a.released, b.released);
      if (c != 0) return c > 0;
    }
    return cmpNameFold(a.name, b.name) < 0;
  }
  const int c = cmpNameFold(a.name, b.name);
  return mode == SORT_ZA ? c > 0 : c < 0;
}

// Order `view` (indices into `keys`) in place. Stable, so equal names keep catalog order.
inline void sortView(int* view, int n, const SortKey* keys, uint8_t mode) {
  if (!view || n <= 1 || !keys) return;
  std::stable_sort(view, view + n, [keys, mode](int x, int y) { return before(keys[x], keys[y], mode); });
}

}  // namespace storefilter
