#pragma once
// The time-zone picker's zones and the POSIX TZ string each one becomes (#699). Pure: no
// Arduino, no prefs, so test/test_time_zones.cpp checks every entry through the C library's
// own localtime() on the host, the same parse newlib does on the device.
//
// Curated zones, each with the correct POSIX DST rules for that region, so non-European
// users get the right time year-round instead of the CET base + EU DST dates.
//
// STORED IDS: the saved setting (TouchPrefsSchema tz_zone) is an index into ZONES[]. So an
// entry's position is its id forever: never reorder, never insert, only APPEND. Id 0
// (Europe/CET) is the default and matches the old behaviour. Id 16 is "Custom (UTC offset)",
// a fixed whole-hour offset with no DST: it used to be "one past the list", and installs
// that picked it hold 16, so it stays at 16 and the zones added later sit after it. The
// order the picker SHOWS is PICK_ORDER[], west to east, which can change freely.
#include <stdint.h>
#include <stdio.h>

namespace TimeZones {

struct Zone { const char* label; const char* posix; };   // posix == nullptr: the Custom entry

static const Zone ZONES[] = {
  /*  0 */ { "Europe (CET/CEST)",   "CET-1CEST,M3.5.0,M10.5.0/3" },
  /*  1 */ { "UK (GMT/BST)",        "GMT0BST,M3.5.0/1,M10.5.0" },
  /*  2 */ { "UTC",                 "UTC0" },
  /*  3 */ { "US Eastern",          "EST5EDT,M3.2.0,M11.1.0" },
  /*  4 */ { "US Central",          "CST6CDT,M3.2.0,M11.1.0" },
  /*  5 */ { "US Mountain",         "MST7MDT,M3.2.0,M11.1.0" },
  /*  6 */ { "US Arizona (no DST)", "MST7" },
  /*  7 */ { "US Pacific",          "PST8PDT,M3.2.0,M11.1.0" },
  /*  8 */ { "US Alaska",           "AKST9AKDT,M3.2.0,M11.1.0" },
  /*  9 */ { "US Hawaii",           "HST10" },
  /* 10 */ { "Canada Atlantic",     "AST4ADT,M3.2.0,M11.1.0" },
  /* 11 */ { "Brazil (Brasilia)",   "<-03>3" },
  /* 12 */ { "India (IST)",         "IST-5:30" },
  /* 13 */ { "China (CST)",         "CST-8" },
  /* 14 */ { "Japan (JST)",         "JST-9" },
  /* 15 */ { "Sydney (AEST/AEDT)",  "AEST-10AEDT,M10.1.0,M4.1.0/3" },
  /* 16 */ { "Custom (UTC offset)", nullptr },
  // ---- appended 2026-10 (Discord: no Central Africa zone); ids are permanent from here ----
  /* 17 */ { "Central Africa (CAT)",    "CAT-2" },                          // Maputo, Harare, Lusaka
  /* 18 */ { "South Africa (SAST)",     "SAST-2" },
  /* 19 */ { "East Africa (EAT)",       "EAT-3" },                          // Nairobi, Addis Ababa
  /* 20 */ { "West Africa (WAT)",       "WAT-1" },                          // Lagos, Kinshasa
  /* 21 */ { "Egypt (EET/EEST)",        "EET-2EEST,M4.5.5/0,M10.5.4/24" },  // DST again since 2023
  /* 22 */ { "Eastern Europe (EET)",    "EET-2EEST,M3.5.0/3,M10.5.0/4" },   // Athens, Bucharest, Kyiv
  /* 23 */ { "Western Europe (WET)",    "WET0WEST,M3.5.0/1,M10.5.0" },      // Lisbon, Canary Islands
  /* 24 */ { "Moscow (MSK)",            "MSK-3" },
  /* 25 */ { "Turkey (TRT)",            "<+03>-3" },
  /* 26 */ { "Arabia (AST)",            "AST-3" },                          // Riyadh, Baghdad, Doha
  /* 27 */ { "Iran (IRST)",             "<+0330>-3:30" },                   // no DST since 2022
  /* 28 */ { "Gulf (GST)",              "<+04>-4" },                        // Dubai, Muscat
  /* 29 */ { "Pakistan (PKT)",          "PKT-5" },
  /* 30 */ { "Nepal (NPT)",             "<+0545>-5:45" },
  /* 31 */ { "Bangladesh (BST)",        "<+06>-6" },
  /* 32 */ { "Indochina (ICT)",         "<+07>-7" },                        // Bangkok, Hanoi, Jakarta
  /* 33 */ { "Singapore (SGT)",         "<+08>-8" },                        // also Manila, Kuala Lumpur
  /* 34 */ { "Perth (AWST)",            "AWST-8" },
  /* 35 */ { "Korea (KST)",             "KST-9" },
  /* 36 */ { "Adelaide (ACST/ACDT)",    "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
  /* 37 */ { "Brisbane (AEST)",         "AEST-10" },
  /* 38 */ { "New Zealand (NZST/NZDT)", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
  /* 39 */ { "Argentina (ART)",         "<-03>3" },
  /* 40 */ { "Mexico City (CST)",       "CST6" },                           // no DST since 2022
  /* 41 */ { "Newfoundland (NST/NDT)",  "NST3:30NDT,M3.2.0,M11.1.0" },
};
static const int ID_N   = (int)(sizeof(ZONES) / sizeof(ZONES[0]));   // valid stored ids: 0..ID_N-1
static const int CUSTOM = 16;                                        // the Custom id, fixed forever
static_assert(CUSTOM < ID_N, "Custom must be a ZONES entry");

// What the picker shows, top to bottom: west to east by standard time, Custom last.
static const uint8_t PICK_ORDER[] = {
   9,  8,  7,  5,  6,  4, 40,  3, 10, 41, 11, 39,          // UTC-10 .. UTC-3
   2,  1, 23,  0, 20,                                       // UTC+0, UTC+1
  22, 21, 17, 18,                                           // UTC+2
  24, 25, 26, 19, 27, 28, 29, 12, 30, 31, 32,               // UTC+3 .. UTC+7
  13, 33, 34, 14, 35, 36, 15, 37, 38,                       // UTC+8 .. UTC+12
  CUSTOM,
};
static const int COUNT = (int)(sizeof(PICK_ORDER) / sizeof(PICK_ORDER[0]));   // picker rows
static_assert(COUNT == ID_N, "every zone id must appear in PICK_ORDER exactly once");

// Picker row -> stored id (an out-of-range row reads as the default).
inline int idAt(int row) { return (row >= 0 && row < COUNT) ? PICK_ORDER[row] : 0; }

inline const char* label(int id) { return (id >= 0 && id < ID_N) ? ZONES[id].label : ""; }

// The saved tz_zone byte -> the id in force. 0xFE is the v2->v3 migration sentinel (a user
// who had a manual hour offset becomes Custom); anything past the list (garbage, or a newer
// build's zone after a downgrade) falls back to the default, Europe.
static const uint8_t STORED_MIGRATE_TO_CUSTOM = 0xFE;
inline int resolveStored(uint8_t stored) {
  if (stored == STORED_MIGRATE_TO_CUSTOM) return CUSTOM;
  return (stored < ID_N) ? (int)stored : 0;
}

// The saved time_offs byte (UTC+N whole hours) -> the offset in force, as the setter clamps.
inline int clampOffsetHours(int h) { return h < -23 ? -23 : (h > 23 ? 23 : h); }

// POSIX TZ string for zone id `zone`; `custom_offset_h` (UTC+N hours) is used only by the
// Custom entry. An id past the list falls back to the default (0), as the saved setting
// does. `out` is always NUL-terminated when out_cap > 0.
inline void buildPosix(int zone, int custom_offset_h, char* out, int out_cap) {
  if (!out || out_cap <= 0) return;
  if (zone < 0 || zone >= ID_N) zone = 0;
  if (ZONES[zone].posix) { snprintf(out, out_cap, "%s", ZONES[zone].posix); return; }
  // POSIX std-offset is the negation of the UTC offset (the time to ADD to local to reach
  // UTC), so UTC-7 -> "<-07>7", UTC+2 -> "<+02>-2".
  custom_offset_h = clampOffsetHours(custom_offset_h);
  if (custom_offset_h == 0) { snprintf(out, out_cap, "UTC0"); return; }
  snprintf(out, out_cap, "<%+03d>%d", custom_offset_h, -custom_offset_h);
}

// The whole device path: the two saved bytes exactly as the prefs blob holds them -> the TZ
// string setenv("TZ") gets (touchPrefsBuildLocalTz).
inline void buildFromStored(uint8_t stored_zone, int8_t stored_offs, char* out, int out_cap) {
  buildPosix(resolveStored(stored_zone), clampOffsetHours((int)stored_offs), out, out_cap);
}

}  // namespace TimeZones
