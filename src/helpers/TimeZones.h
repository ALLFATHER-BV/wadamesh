#pragma once
// The time-zone picker's zones and the POSIX TZ string each one becomes (#699). Pure: no
// Arduino, no prefs, so test/test_time_zones.cpp checks every entry through the C library's
// own localtime() on the host, the same parse newlib does on the device.
//
// Curated zones, each with the correct POSIX DST rules for that region, so non-European
// users get the right time year-round instead of the CET base + EU DST dates. Index 0
// (Europe/CET) is the default and matches the old behaviour. The entry after the last one
// is "Custom (UTC offset)": a fixed whole-hour offset, no DST.
#include <stdio.h>

namespace TimeZones {

struct Zone { const char* label; const char* posix; };

static const Zone ZONES[] = {
  { "Europe (CET/CEST)",   "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "UK (GMT/BST)",        "GMT0BST,M3.5.0/1,M10.5.0" },
  { "UTC",                 "UTC0" },
  { "US Eastern",          "EST5EDT,M3.2.0,M11.1.0" },
  { "US Central",          "CST6CDT,M3.2.0,M11.1.0" },
  { "US Mountain",         "MST7MDT,M3.2.0,M11.1.0" },
  { "US Arizona (no DST)", "MST7" },
  { "US Pacific",          "PST8PDT,M3.2.0,M11.1.0" },
  { "US Alaska",           "AKST9AKDT,M3.2.0,M11.1.0" },
  { "US Hawaii",           "HST10" },
  { "Canada Atlantic",     "AST4ADT,M3.2.0,M11.1.0" },
  { "Brazil (Brasilia)",   "<-03>3" },
  { "India (IST)",         "IST-5:30" },
  { "China (CST)",         "CST-8" },
  { "Japan (JST)",         "JST-9" },
  { "Sydney (AEST/AEDT)",  "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};
static const int ZONE_N = (int)(sizeof(ZONES) / sizeof(ZONES[0]));
static const int CUSTOM = ZONE_N;          // the "Custom (UTC offset)" index
static const int COUNT  = ZONE_N + 1;      // pickable entries

// POSIX TZ string for picker entry `zone`; `custom_offset_h` (UTC+N hours) is used only by
// the Custom entry. An index past the list falls back to the default (0), as the saved
// setting does. `out` is always NUL-terminated when out_cap > 0.
inline void buildPosix(int zone, int custom_offset_h, char* out, int out_cap) {
  if (!out || out_cap <= 0) return;
  if (zone < 0 || zone > CUSTOM) zone = 0;
  if (zone < ZONE_N) { snprintf(out, out_cap, "%s", ZONES[zone].posix); return; }
  // POSIX std-offset is the negation of the UTC offset (the time to ADD to local to reach
  // UTC), so UTC-7 -> "<-07>7", UTC+2 -> "<+02>-2".
  if (custom_offset_h < -23) custom_offset_h = -23;
  if (custom_offset_h >  23) custom_offset_h =  23;
  if (custom_offset_h == 0) { snprintf(out, out_cap, "UTC0"); return; }
  snprintf(out, out_cap, "<%+03d>%d", custom_offset_h, -custom_offset_h);
}

}  // namespace TimeZones
