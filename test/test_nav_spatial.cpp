// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the keypad cursor's spatial step (src/ui-touch/NavSpatial.h, used by
// navMoveDir). The geometry is the telemetry window on the ThinkNode M9 (320x240):
// with a history chart showing, the Request button could not be reached with the
// d-pad (reported on Discord by benanderson, beta_100) because the chart reached up
// into the row of the Request / gear / X buttons and so counted as the nearest thing
// left of the gear.
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_nav_spatial.cpp -o /tmp/navsp && /tmp/navsp

#include <assert.h>
#include <stdio.h>

#include "ui-touch/NavSpatial.h"

// The window's controls in the card's content coordinates, in the order UITask collects
// them (tree order): the X badge, the gear, Request, then the chart, the two hour
// fields and Show. Content width 282 (card 304, padding 10, border 1).
enum { X_BADGE, GEAR, REQUEST, CHART, FROM, TO, SHOW, N_CTRL };

static void telemetryLayout(NavRect* r, int chart_top) {
  const int cw = 282;
  r[X_BADGE] = { cw - 24 - 2, 2, cw - 2 - 1, 2 + 24 - 1 };        // 24 px, TOP_RIGHT -2,2
  r[GEAR]    = { cw - 32 - 34, 0, cw - 34 - 1, 31 };              // 32 px, TOP_RIGHT -34
  r[REQUEST] = { cw - 32 - 68, 0, cw - 68 - 1, 31 };              // 32 px, TOP_RIGHT -68
  r[CHART]   = { 30, chart_top, 30 + (304 - 20 - 60) - 1, chart_top + 108 - 1 };
  const int fy = chart_top - 4 + 108 + 28 + 18;                    // legend, then "Time range"
  r[FROM]    = { 38, fy, 38 + 44 - 1, fy + 31 };
  r[TO]      = { 132, fy, 132 + 44 - 1, fy + 31 };
  r[SHOW]    = { 200, fy, 200 + 64 - 1, fy + 31 };
}

// One arrow press from `from`: the candidates are every other control.
static int step(const NavRect* r, int from, int dir) {
  NavRect cand[N_CTRL];
  int idx[N_CTRL], n = 0;
  for (int i = 0; i < N_CTRL; i++) {
    if (i == from) continue;
    cand[n] = r[i];
    idx[n++] = i;
  }
  const int pick = navSpatialPick(r[from], cand, n, dir);
  return pick < 0 ? from : idx[pick];
}

// Every control the cursor can reach from `start` with any number of arrow presses.
static unsigned reachable(const NavRect* r, int start) {
  unsigned seen = 1u << start;
  bool grew = true;
  while (grew) {
    grew = false;
    for (int c = 0; c < N_CTRL; c++) {
      if (!(seen & (1u << c))) continue;
      for (int d = NAV_UP; d <= NAV_RIGHT; d++) {
        const int to = step(r, c, d);
        if (!(seen & (1u << to))) { seen |= 1u << to; grew = true; }
      }
    }
  }
  return seen;
}

int main() {
  NavRect r[N_CTRL];

  // Before: the chart started 26 px down (the title line plus 4), inside the 32 px
  // button row. Left from the gear went to the chart, Up from the chart to the gear:
  // Request was never reached, from anywhere.
  telemetryLayout(r, 26);
  assert(step(r, GEAR, NAV_LEFT) == CHART);
  assert(step(r, CHART, NAV_UP) == GEAR);
  for (int s = 0; s < N_CTRL; s++)
    if (s != REQUEST) assert(!(reachable(r, s) & (1u << REQUEST)));

  // After: the chart starts below the button row (32 + 4). Left from the gear is
  // Request, and Request is reachable from every control in the window.
  telemetryLayout(r, 36);
  assert(step(r, X_BADGE, NAV_LEFT) == GEAR);
  assert(step(r, GEAR, NAV_LEFT) == REQUEST);
  assert(step(r, REQUEST, NAV_RIGHT) == GEAR);
  assert(step(r, REQUEST, NAV_DOWN) == CHART);
  assert(step(r, CHART, NAV_UP) == GEAR);
  assert(step(r, CHART, NAV_DOWN) == FROM);
  for (int s = 0; s < N_CTRL; s++) assert(reachable(r, s) == (1u << N_CTRL) - 1);

  // The band rule the ranking is built on (Send advert): Down from a button lands on
  // the narrow switch right below it, not the wide dropdown a row further down.
  {
    const NavRect flood = { 0, 0, 99, 29 };
    const NavRect cand[] = {
      { 0, 80, 299, 109 },     // "Flood every" dropdown, full width, two rows down
      { 250, 40, 299, 69 },    // "On boot" switch, right-aligned, the next row
    };
    assert(navSpatialPick(flood, cand, 2, NAV_DOWN) == 1);
  }
  // Nothing in that direction: -1, and the caller keeps the focus.
  {
    const NavRect cur = { 0, 0, 10, 10 };
    const NavRect cand[] = { { 0, 20, 10, 30 } };
    assert(navSpatialPick(cur, cand, 1, NAV_UP) == -1);
  }

  printf("test_nav_spatial: OK\n");
  return 0;
}
