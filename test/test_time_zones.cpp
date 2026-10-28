// Host test for the time zone (#699): every picker zone's POSIX string through the C library's
// localtime(), the Custom offset's sign, and ClockFloorRTC's clock math with the zone in force
// (a later clock set, e.g. the GPS re-sync every 30 min, moves the time but never the zone).
// Also (Discord, Central Africa missing, "clock behind by a half hour" on Custom): the saved ids
// of the zones that existed before the list grew still mean the same zone, and EVERY possible
// pair of saved bytes (tz_zone 0..255 x time_offs -128..127) goes through the exact prefs path
// (TimeZones::buildFromStored) to localtime() and lands on a whole-hour offset for Custom.
// Build + run from the repo root (scripts/run-host-tests.sh does this):
//   c++ -std=c++17 -Wall -Wextra -I src -I test/clock_floor_fakes test/test_time_zones.cpp \
//       -o /tmp/t && /tmp/t
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

// The system clock ClockFloorRTC mirrors every accepted set into (settimeofday needs root on a
// desktop), simulated so the test owns it.
static long g_sys = 0;
static int fake_settimeofday(const struct timeval* tv, const void*) { g_sys = (long)tv->tv_sec; return 0; }
static int fake_gettimeofday(struct timeval* tv, void*) { tv->tv_sec = g_sys; tv->tv_usec = 0; return 0; }
#define settimeofday fake_settimeofday
#define gettimeofday fake_gettimeofday
#include "helpers/ClockFloorRTC.h"
#undef settimeofday
#undef gettimeofday

#include "helpers/TimeZones.h"

// UTC offset in minutes (local - UTC) that zone string `tz` gives at epoch `t`.
static int offsetMin(const char* tz, time_t t) {
  setenv("TZ", tz, 1);
  tzset();
  struct tm l, u;
  localtime_r(&t, &l);
  gmtime_r(&t, &u);
  int d = (l.tm_hour - u.tm_hour) * 60 + (l.tm_min - u.tm_min);
  const int dd = l.tm_yday - u.tm_yday;
  if (dd == 1 || dd < -1) d += 24 * 60;   // local is the next day (incl. year wrap)
  if (dd == -1 || dd > 1) d -= 24 * 60;
  return d;
}
static int zoneOffset(int zone, int custom_h, time_t t) {
  char tz[48];
  TimeZones::buildPosix(zone, custom_h, tz, sizeof tz);
  return offsetMin(tz, t);
}
static int indexOf(const char* label) {
  for (int i = 0; i < TimeZones::ID_N; i++) if (!strcmp(TimeZones::ZONES[i].label, label)) return i;
  assert(!"zone missing");
  return -1;
}

int main() {
  // 2026 reference instants (UTC).
  const time_t JAN = 1768478400;   // 2026-01-15 12:00Z
  const time_t JUL = 1784116800;   // 2026-07-15 12:00Z
  const time_t OCT26 = 1793017200; // 2026-10-26 12:20Z: EU back on winter time, US still on DST

  // ---- saved ids are permanent ----
  // The ids every earlier build saved (0..15 the zones, 16 Custom = "one past the list"). A
  // zone added later must never shift one of these, or an existing install silently changes
  // zone on upgrade.
  static const char* const kLegacy[17] = {
    "Europe (CET/CEST)", "UK (GMT/BST)", "UTC", "US Eastern", "US Central", "US Mountain",
    "US Arizona (no DST)", "US Pacific", "US Alaska", "US Hawaii", "Canada Atlantic",
    "Brazil (Brasilia)", "India (IST)", "China (CST)", "Japan (JST)", "Sydney (AEST/AEDT)",
    "Custom (UTC offset)",
  };
  for (int i = 0; i < 17; i++) assert(!strcmp(TimeZones::ZONES[i].label, kLegacy[i]));
  assert(TimeZones::CUSTOM == 16 && TimeZones::ZONES[16].posix == nullptr);
  for (int i = 0; i < TimeZones::ID_N; i++) assert((TimeZones::ZONES[i].posix == nullptr) == (i == TimeZones::CUSTOM));
  // The picker shows every id exactly once, Custom last, in west-to-east order.
  {
    int seen[TimeZones::ID_N] = {0};
    for (int r = 0; r < TimeZones::COUNT; r++) seen[TimeZones::idAt(r)]++;
    for (int i = 0; i < TimeZones::ID_N; i++) assert(seen[i] == 1);
    assert(TimeZones::idAt(TimeZones::COUNT - 1) == TimeZones::CUSTOM);
    int prev = -24 * 60;
    for (int r = 0; r < TimeZones::COUNT - 1; r++) {
      const int o = zoneOffset(TimeZones::idAt(r), 0, JAN);   // January: standard time in the north
      const int std_o = (TimeZones::idAt(r) == indexOf("Sydney (AEST/AEDT)") ||
                         TimeZones::idAt(r) == indexOf("Adelaide (ACST/ACDT)") ||
                         TimeZones::idAt(r) == indexOf("New Zealand (NZST/NZDT)")) ? o - 60 : o;
      assert(std_o >= prev);
      prev = std_o;
    }
    assert(TimeZones::idAt(-1) == 0 && TimeZones::idAt(TimeZones::COUNT) == 0);
  }

  struct { const char* label; int jan, jul, oct26; } want[] = {
    { "Europe (CET/CEST)",    60,  120,   60 },
    { "UK (GMT/BST)",          0,   60,    0 },
    { "UTC",                   0,    0,    0 },
    { "US Eastern",         -300, -240, -240 },
    { "US Central",         -360, -300, -300 },
    { "US Mountain",        -420, -360, -360 },
    { "US Arizona (no DST)",-420, -420, -420 },
    { "US Pacific",         -480, -420, -420 },
    { "US Alaska",          -540, -480, -480 },
    { "US Hawaii",          -600, -600, -600 },
    { "Canada Atlantic",    -240, -180, -180 },
    { "Brazil (Brasilia)",  -180, -180, -180 },
    { "India (IST)",         330,  330,  330 },
    { "China (CST)",         480,  480,  480 },
    { "Japan (JST)",         540,  540,  540 },
    { "Sydney (AEST/AEDT)",  660,  600,  660 },
    // appended zones (ids 17+)
    { "Central Africa (CAT)",    120,  120,  120 },
    { "South Africa (SAST)",     120,  120,  120 },
    { "East Africa (EAT)",       180,  180,  180 },
    { "West Africa (WAT)",        60,   60,   60 },
    { "Egypt (EET/EEST)",        120,  180,  180 },   // still summer time: it ends Thu 29 Oct
    { "Eastern Europe (EET)",    120,  180,  120 },
    { "Western Europe (WET)",      0,   60,    0 },
    { "Moscow (MSK)",            180,  180,  180 },
    { "Turkey (TRT)",            180,  180,  180 },
    { "Arabia (AST)",            180,  180,  180 },
    { "Iran (IRST)",             210,  210,  210 },
    { "Gulf (GST)",              240,  240,  240 },
    { "Pakistan (PKT)",          300,  300,  300 },
    { "Nepal (NPT)",             345,  345,  345 },
    { "Bangladesh (BST)",        360,  360,  360 },
    { "Indochina (ICT)",         420,  420,  420 },
    { "Singapore (SGT)",         480,  480,  480 },
    { "Perth (AWST)",            480,  480,  480 },
    { "Korea (KST)",             540,  540,  540 },
    { "Adelaide (ACST/ACDT)",    630,  570,  630 },
    { "Brisbane (AEST)",         600,  600,  600 },
    { "New Zealand (NZST/NZDT)", 780,  720,  780 },
    { "Argentina (ART)",        -180, -180, -180 },
    { "Mexico City (CST)",      -360, -360, -360 },
    { "Newfoundland (NST/NDT)", -210, -150, -150 },
  };
  static_assert(sizeof(want) / sizeof(want[0]) == (size_t)TimeZones::ID_N - 1, "a zone without a test row");
  for (auto& w : want) {
    const int z = indexOf(w.label);
    if (zoneOffset(z, 0, JAN) != w.jan || zoneOffset(z, 0, JUL) != w.jul || zoneOffset(z, 0, OCT26) != w.oct26)
      printf("FAIL %s: %d %d %d\n", w.label, zoneOffset(z, 0, JAN), zoneOffset(z, 0, JUL), zoneOffset(z, 0, OCT26));
    assert(zoneOffset(z, 0, JAN) == w.jan);
    assert(zoneOffset(z, 0, JUL) == w.jul);
    assert(zoneOffset(z, 0, OCT26) == w.oct26);
  }

  // DST edges to the minute: US 2026-03-08 07:00Z, EU 2026-03-29 01:00Z.
  const int us = indexOf("US Eastern"), eu = indexOf("Europe (CET/CEST)");
  assert(zoneOffset(us, 0, 1772953200 - 60) == -300 && zoneOffset(us, 0, 1772953200) == -240);
  assert(zoneOffset(eu, 0, 1774746000 - 60) ==   60 && zoneOffset(eu, 0, 1774746000) ==  120);
  // Egypt 2026: summer time from Fri 24 Apr 00:00 local (23 Apr 22:00Z) to the end of
  // Thu 29 Oct (24:00 EEST = 29 Oct 21:00Z).
  const int eg = indexOf("Egypt (EET/EEST)");
  assert(zoneOffset(eg, 0, 1776981600 - 60) == 120 && zoneOffset(eg, 0, 1776981600) == 180);
  assert(zoneOffset(eg, 0, 1793307600 - 60) == 180 && zoneOffset(eg, 0, 1793307600) == 120);

  // Custom (UTC offset): UTC+N reads N hours ahead of UTC, all year, both signs.
  for (int h = -12; h <= 14; h++) {
    assert(zoneOffset(TimeZones::CUSTOM, h, JAN) == h * 60);
    assert(zoneOffset(TimeZones::CUSTOM, h, JUL) == h * 60);
  }
  assert(zoneOffset(TimeZones::CUSTOM, 99, JAN) == 23 * 60);     // clamped like the setter
  // A stale id (a newer build's longer list, after a downgrade) reads as the default, as the
  // saved setting does.
  char tz[48];
  TimeZones::buildPosix(TimeZones::ID_N + 3, 0, tz, sizeof tz);
  assert(!strcmp(tz, TimeZones::ZONES[0].posix));
  TimeZones::buildPosix(0, 0, tz, 4);                             // truncates, stays terminated
  assert(strlen(tz) == 3);

  // ---- every saved byte pair, through the exact prefs path, to localtime() ----
  // touchPrefsBuildLocalTz() = buildFromStored(s_cfg.tz_zone, s_cfg.time_offs): no other
  // arithmetic sits between the blob and setenv("TZ"). So for Custom, every stored offset
  // must read as that many WHOLE hours (no half hour from anywhere), at every quarter of an
  // hour of the day, and every other stored zone byte must read as its own zone (or Europe).
  int pairs = 0;
  for (int z = 0; z <= 255; z++) {
    const int id = (z == TimeZones::STORED_MIGRATE_TO_CUSTOM) ? TimeZones::CUSTOM : (z < TimeZones::ID_N ? z : 0);
    assert(TimeZones::resolveStored((uint8_t)z) == id);
    for (int o = -128; o <= 127; o++) {
      TimeZones::buildFromStored((uint8_t)z, (int8_t)o, tz, sizeof tz);
      char ref[48];
      TimeZones::buildPosix(id, o < -23 ? -23 : (o > 23 ? 23 : o), ref, sizeof ref);
      assert(!strcmp(tz, ref));
      ++pairs;
    }
  }
  for (int o = -128; o <= 127; o++) {
    const int h = o < -23 ? -23 : (o > 23 ? 23 : o);
    TimeZones::buildFromStored((uint8_t)TimeZones::CUSTOM, (int8_t)o, tz, sizeof tz);
    for (time_t t = JAN; t < JAN + 86400; t += 900) {
      assert(offsetMin(tz, t) == h * 60);
      struct tm l, u; localtime_r(&t, &l); gmtime_r(&t, &u);
      assert(l.tm_min == u.tm_min && l.tm_sec == u.tm_sec);   // minutes never shift on Custom
    }
    assert(offsetMin(tz, JUL) == h * 60);                      // and no DST
  }
  // The Discord case: Custom UTC+2 at 10:05:00Z reads 12:05 local, same as Central Africa.
  {
    const time_t t = JAN - 2 * 3600 + 5 * 60;   // 10:05Z
    TimeZones::buildFromStored((uint8_t)TimeZones::CUSTOM, 2, tz, sizeof tz);
    setenv("TZ", tz, 1); tzset();
    struct tm l; localtime_r(&t, &l);
    assert(l.tm_hour == 12 && l.tm_min == 5);
    TimeZones::buildFromStored((uint8_t)indexOf("Central Africa (CAT)"), 0, tz, sizeof tz);
    setenv("TZ", tz, 1); tzset();
    localtime_r(&t, &l);
    assert(l.tm_hour == 12 && l.tm_min == 5);
  }

  // ---- ClockFloorRTC with the zone in force ----
  struct Dummy : mesh::RTCClock {
    uint32_t getCurrentTime() override { return 0; }
    void setCurrentTime(uint32_t) override {}
  } fallback;
  ClockFloorRTC clk(fallback);
  const uint32_t T = (uint32_t)OCT26;

  // A sync (NTP / GPS / phone) lands: chip, floor and system clock all follow.
  g_sys = ClockFloorRTC::MIN_VALID_EPOCH;          // the power-on seed
  FakeRtcChip::now() = ClockFloorRTC::MIN_VALID_EPOCH;
  clk.setCurrentTime(T);
  assert(FakeRtcChip::now() == T && g_sys == (long)T && clk.getCurrentTime() == T);
  assert(clk.timeIsCurrent());

  // The display reads the system clock through the zone: US Eastern = 08:20 local.
  TimeZones::buildPosix(us, 0, tz, sizeof tz);
  setenv("TZ", tz, 1); tzset();
  time_t now = (time_t)g_sys; struct tm l; localtime_r(&now, &l);
  assert(l.tm_hour == 8 && l.tm_min == 20);
  // 30 minutes on, the GPS re-sync sets the clock again: the time moves, the zone does not.
  clk.setCurrentTime(T + 1800);
  assert(!strcmp(getenv("TZ"), tz));
  now = (time_t)g_sys; localtime_r(&now, &l);
  assert(l.tm_hour == 8 && l.tm_min == 50);

  // Garbage sets are refused whatever the source (1902 GPS date, the unset seed, 2043 chip).
  clk.setCurrentTime(0);
  clk.setCurrentTime(ClockFloorRTC::MIN_VALID_EPOCH);
  clk.setCurrentTime(ClockFloorRTC::MAX_PLAUSIBLE_EPOCH + 1);
  assert(g_sys == (long)(T + 1800) && FakeRtcChip::now() == T + 1800);

  // The chip reads behind the floor (a write that did not take, a stopped oscillator after
  // the floor moved on) while the system clock runs: the system clock wins (#746/#756), and
  // the floor follows it.
  assert(clk.getCurrentTime() == T + 1800 && clk.getFloor() == T + 1800);
  FakeRtcChip::now() = T;                // the chip lost the last set
  g_sys = T + 3600;
  assert(clk.getCurrentTime() == T + 3600 && clk.getFloor() == T + 3600);
  // ...and the chip asserting garbage-future is ignored the same way.
  FakeRtcChip::now() = ClockFloorRTC::MAX_PLAUSIBLE_EPOCH + 100000;
  g_sys = T + 3700;
  assert(clk.getCurrentTime() == T + 3700);
  // Both behind the floor: hold the floor (protocol timestamps never step back).
  FakeRtcChip::now() = T; g_sys = T;
  assert(clk.getCurrentTime() == T + 3700);
  // A real correction more than 10 min back pulls the floor down with it.
  clk.setCurrentTime(T);
  assert(clk.getFloor() == T && clk.getCurrentTime() == T);

  printf("test_time_zones: all passed (%d zones + custom, %d saved byte pairs)\n", TimeZones::ID_N - 1, pairs);
  return 0;
}
