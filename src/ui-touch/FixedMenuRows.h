// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

struct FixedMenuRow {
  int top;
  int bottom;
  int page;
  int y;
};

// A vertical band can contain two buttons side by side, or a label and its switch.
// Keep the whole band on a single page.
template <typename Row>
inline int packFixedMenuRows(Row* rows, std::size_t count, int top, int height) {
  if (!count) return 0;
  int page = 0;
  int origin = top;
  for (std::size_t first = 0; first < count;) {
    int bandTop = rows[first].top;
    int bandBottom = rows[first].bottom;
    std::size_t last = first + 1;
    while (last < count && rows[last].top < bandBottom) {
      if (rows[last].top < bandTop) bandTop = rows[last].top;
      if (rows[last].bottom > bandBottom) bandBottom = rows[last].bottom;
      ++last;
    }
    if (first == 0 && bandBottom - origin > height) origin = bandTop;
    if (bandTop > origin && bandBottom - origin > height) {
      ++page;
      origin = bandTop;
    }
    for (std::size_t i = first; i < last; ++i) {
      rows[i].page = page;
      rows[i].y = rows[i].top - origin;
    }
    first = last;
  }
  return page + 1;
}
