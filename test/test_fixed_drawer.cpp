// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/ui-touch/FixedDrawer.h"
#include <cassert>

int main() {
  fixed_drawer::Page page;
  int entries[10];
  int sizes[4];
  auto render = [&](int count, int capacity, int withFooter) {
    for (int i = 0; i < count; ++i) entries[i] = i < 6 ? i : 5 - i;
    for (auto& size : sizes) size = 0;
    return fixed_drawer::select(entries, count, 6,
      [](int app) { return app < 0 ? 3 : app / 2; }, capacity, withFooter, page, sizes);
  };
  auto w = render(10, 10, 8);
  assert(!w.footer && w.first == 0 && w.end == 10 && entries[9] == -4);
  w = render(10, 3, 2);
  assert(w.footer && w.pages == 2 && entries[0] == 6 && sizes[3] == 4);
  page.index = 1;
  w = render(10, 3, 2);
  assert(w.first == 2 && w.end == 4 && entries[3] == 9);
  page.group = 3; page.index = 0;
  w = render(10, 3, 2);
  assert(w.pages == 2 && entries[w.first] == -1);
  page.index = 1;
  w = render(10, 3, 2);
  assert(entries[w.first] == -3 && entries[w.end - 1] == -4);
  assert(page.back() && page.index == 1 && page.group == -1);
  page.group = 3;
  w = render(6, 3, 2);
  assert(page.group == -1 && page.index == 0 && w.pages == 1 && !w.footer);
  w = render(5, 3, 2);
  assert(w.end == 3 && entries[2] == 4);  // a group with one app opens it directly
  page.group = 0;
  w = render(6, 3, 2);
  assert(w.footer && w.pages == 1 && w.end == 2);
  w = render(6, 10, 8);
  assert(page.group == -1 && !w.footer);
}
