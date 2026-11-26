// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include "ui-touch/FixedMenuRows.h"

int main() {
  FixedMenuRow rows[] = {
      {4, 22, 0, 0}, {0, 24, 0, 0},      // One label and switch.
      {30, 58, 0, 0}, {30, 58, 0, 0},    // Two buttons in one row.
      {64, 95, 0, 0},
  };
  assert(packFixedMenuRows(rows, 5, 0, 58) == 2);
  assert(rows[0].page == 0 && rows[0].y == 4);
  assert(rows[1].page == 0 && rows[1].y == 0);
  assert(rows[2].page == 0 && rows[3].page == 0);
  assert(rows[4].page == 1 && rows[4].y == 0);
  assert(packFixedMenuRows(rows, 5, 0, 95) == 1);
  assert(rows[4].page == 0 && rows[4].y == 64);
}
