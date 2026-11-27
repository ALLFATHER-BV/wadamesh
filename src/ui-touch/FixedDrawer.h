// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace fixed_drawer {

struct Page {
  int group = -1;
  int index = 0;
  int rootIndex = 0;

  bool back() {
    if (group < 0) return false;
    group = -1;
    index = rootIndex;
    return true;
  }
};

struct Window {
  int first, end, pages;
  bool footer;
};

// Entries are the launcher's filtered tile indices. Folders occupy unused indices
// above the built-ins. Installed apps keep their existing negative indices.
template <int Groups, class GroupOf>
Window select(int* entries, int count, int folderBase, GroupOf groupOf,
              int fullCapacity, int footerCapacity, Page& page, int (&sizes)[Groups]) {
  int firstTile[Groups] = {};
  for (int i = 0; i < count; ++i) {
    const int group = groupOf(entries[i]);
    if (!sizes[group]++) firstTile[group] = entries[i];
  }
  if (count <= fullCapacity) {
    page = Page{};
  } else {
    if (page.group >= 0 && !sizes[page.group]) page.back();
    int visible = 0;
    if (page.group < 0) {
      for (int group = 0; group < Groups; ++group)
        if (sizes[group]) entries[visible++] = sizes[group] == 1 ? firstTile[group] : folderBase + group;
    } else {
      for (int i = 0; i < count; ++i)
        if (groupOf(entries[i]) == page.group) entries[visible++] = entries[i];
    }
    count = visible;
  }
  const bool footer = page.group >= 0 || count > fullCapacity;
  const int capacity = footer ? footerCapacity : fullCapacity;
  const int pages = count ? (count + capacity - 1) / capacity : 1;
  if (page.index >= pages) page.index = pages - 1;
  if (page.index < 0) page.index = 0;
  if (page.group < 0) page.rootIndex = page.index;
  const int first = page.index * capacity;
  const int end = first + capacity < count ? first + capacity : count;
  return { first, end, pages, footer };
}

}  // namespace fixed_drawer
