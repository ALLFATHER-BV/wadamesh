// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The keypad cursor's spatial step (navMoveDir in UITask.cpp): from the focused
// control, which control does an arrow press move to? Pure rectangles in, an index
// out, so the ranking can be tested on the host with real screen geometry
// (test/test_nav_spatial.cpp). UITask filters the candidates (visible, in the right
// region, not a horizontal-only gear on Up/Down) and passes their laid-out coords.

#include <stdint.h>

enum NavDir { NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT };

struct NavRect { int32_t x1, y1, x2, y2; };   // inclusive corners, like lv_area_t

// Geometry of candidate `b` relative to the focused rect `a`: true when it lies in `dir`,
// with the gaps navSpatialPick ranks by. Direction uses the CENTRE delta (which side the
// candidate is on); the gaps are rect-to-rect, not centre-to-centre: `primary` is the gap
// along the pressed axis (how far past the current edge it sits) so the element IMMEDIATELY
// in that direction wins, and `cross` is the off-axis gap, 0 whenever the rects line up on
// that axis (a full-width row below a right-aligned switch still counts as "straight down").
static inline bool navSpatialMetrics(const NavRect& a, const NavRect& b, int dir, long* primary, long* cross) {
  const long dx = (long)(b.x1 + b.x2) / 2 - (long)(a.x1 + a.x2) / 2;
  const long dy = (long)(b.y1 + b.y2) / 2 - (long)(a.y1 + a.y2) / 2;
  long hgap = (long)b.x1 - a.x2; if ((long)a.x1 - b.x2 > hgap) hgap = (long)a.x1 - b.x2; if (hgap < 0) hgap = 0;
  long vgap = (long)b.y1 - a.y2; if ((long)a.y1 - b.y2 > vgap) vgap = (long)a.y1 - b.y2; if (vgap < 0) vgap = 0;
  bool ok;
  switch (dir) {
    case NAV_UP:    ok = dy < 0; *primary = vgap; *cross = hgap; break;
    case NAV_DOWN:  ok = dy > 0; *primary = vgap; *cross = hgap; break;
    case NAV_LEFT:  ok = dx < 0; *primary = hgap; *cross = vgap; break;
    default:        ok = dx > 0; *primary = hgap; *cross = vgap; break;  // NAV_RIGHT
  }
  return ok;
}

// The candidate an arrow press moves to, or -1. Pass 1 finds the NEAREST candidate along
// the pressed axis and takes its extent as the row (or column) band; pass 2 ranks only
// what sits inside that band, so a press never jumps over an intervening control (on Send
// advert, Down from Flood used to land on the "Flood every" dropdown two rows down, which
// is wide and so perfectly aligned, instead of the narrow "On boot" switch right below).
// In the band, alignment dominates: score = primary + 8 * cross, ties to the first listed.
static inline int navSpatialPick(const NavRect& cur, const NavRect* cand, int n, int dir) {
  long band_lo = 0, band_hi = 0;
  bool have_band = false;
  long near_primary = 0x7FFFFFFFL, near_cross = 0x7FFFFFFFL;
  const bool vert = (dir == NAV_UP || dir == NAV_DOWN);
  for (int i = 0; i < n; i++) {
    long primary, cross;
    if (!navSpatialMetrics(cur, cand[i], dir, &primary, &cross)) continue;
    if (primary > near_primary || (primary == near_primary && cross >= near_cross)) continue;
    near_primary = primary; near_cross = cross;
    band_lo = vert ? cand[i].y1 : cand[i].x1;
    band_hi = vert ? cand[i].y2 : cand[i].x2;
    have_band = true;
  }
  int best = -1;
  long best_score = 0x7FFFFFFFL;
  for (int i = 0; i < n; i++) {
    const NavRect& b = cand[i];
    long primary, cross;
    if (!navSpatialMetrics(cur, b, dir, &primary, &cross)) continue;
    // Out of band = entirely past the nearest candidate in the pressed direction, i.e. a
    // further row/column. Candidates that merely overlap the band (a taller neighbour, a
    // switch beside a button) stay in the running and are ranked by the score below.
    if (have_band) {
      bool past;
      switch (dir) {
        case NAV_UP:    past = b.y2 <  band_lo; break;
        case NAV_DOWN:  past = b.y1 >  band_hi; break;
        case NAV_LEFT:  past = b.x2 <  band_lo; break;
        default:        past = b.x1 >  band_hi; break;  // NAV_RIGHT
      }
      if (past) continue;
    }
    const long score = primary + 8 * cross;
    if (score < best_score) { best_score = score; best = i; }
  }
  return best;
}
