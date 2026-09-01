// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Lightweight civil sunrise/sunset calculator (sun altitude = -0.833°).
// NOAA-style algorithm, integer-friendly, no external dependencies.
// All inputs/outputs in minutes-from-local-midnight.
// Returns false when position/time is invalid or polar day/night applies.

#include <cmath>
#include <cstdint>
#include <ctime>

namespace SunCalc {

// Inputs: lat/lon in degrees, UTC unix timestamp (for date + UTC offset hours).
// Outputs: sunrise_min / sunset_min = minutes from local midnight (0..1439).
// Returns true if a normal sunrise and sunset exist for that date/location.
// Returns false on polar day (treat as day) or polar night (treat as night);
// caller can read is_polar_day to distinguish.
inline bool compute(double lat_deg, double lon_deg,
                    time_t utc_unix, float tz_offset_hours,
                    int& sunrise_min, int& sunset_min,
                    bool* is_polar_day = nullptr) {
  // --- date from UTC timestamp + tz offset ------------------------------------
  const time_t local_ts = utc_unix + (time_t)(tz_offset_hours * 3600.0f);
  struct tm tm_local;
  gmtime_r(&local_ts, &tm_local);   // treat shifted epoch as "UTC" to get local fields

  const int year  = tm_local.tm_year + 1900;
  const int month = tm_local.tm_mon + 1;
  const int day   = tm_local.tm_mday;

  // --- Julian Day Number (JD) -------------------------------------------------
  const int a = (14 - month) / 12;
  const int y = year + 4800 - a;
  const int m = month + 12 * a - 3;
  const double jd = day + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;

  // --- Julian century ---------------------------------------------------------
  const double jc = (jd - 2451545.0) / 36525.0;

  // --- Solar geometry (degrees) -----------------------------------------------
  const double pi = M_PI;
  auto d2r = [&](double d) { return d * pi / 180.0; };
  auto r2d = [&](double r) { return r * 180.0 / pi; };
  auto norm = [&](double x) { return x - 360.0 * floor(x / 360.0); };

  const double mean_long  = norm(280.46646 + jc * (36000.76983 + jc * 0.0003032));
  const double mean_anom  = d2r(357.52911 + jc * (35999.05029 - jc * 0.0001537));
  const double eqctr      = sin(mean_anom)  * (1.914602 - jc * (0.004817 + 0.000014 * jc))
                           + sin(2*mean_anom)* 0.019993 - sin(3*mean_anom) * 0.000289;
  const double sun_long   = d2r(norm(mean_long + eqctr));
  const double obliq_corr = d2r(23.0 + (26.0 + ((21.448 - jc * (46.8150 + jc * (0.00059 - jc * 0.001813)))) / 60.0) / 60.0
                               + 0.00256 * cos(d2r(125.04 - 1934.136 * jc)));
  const double decl       = asin(sin(obliq_corr) * sin(sun_long));

  // Equation of time (minutes)
  const double var_y      = tan(obliq_corr / 2.0) * tan(obliq_corr / 2.0);
  const double eq_time    = 4.0 * r2d(var_y * sin(2 * sun_long)
                             - 2.0 * 0.016708634 * sin(mean_anom)  // eccentricity ≈ e
                             + 4.0 * 0.016708634 * var_y * sin(mean_anom) * cos(2 * sun_long)
                             - 0.5 * var_y * var_y * sin(4 * sun_long)
                             - 1.25 * 0.016708634 * 0.016708634 * sin(2 * mean_anom));

  // Hour angle for civil sunrise (altitude = -0.833°)
  const double lat_r    = d2r(lat_deg);
  const double cos_ha   = (cos(d2r(90.833)) / (cos(lat_r) * cos(decl)))
                         - tan(lat_r) * tan(decl);

  if (cos_ha < -1.0) {
    if (is_polar_day) *is_polar_day = true;
    return false;   // polar day — sun never sets
  }
  if (cos_ha > 1.0) {
    if (is_polar_day) *is_polar_day = false;
    return false;   // polar night — sun never rises
  }
  if (is_polar_day) *is_polar_day = false;

  const double ha_deg = r2d(acos(cos_ha));   // hour angle in degrees

  // Solar noon in local solar minutes from midnight
  const double solar_noon = 720.0 - 4.0 * lon_deg - eq_time + tz_offset_hours * 60.0;

  sunrise_min = (int)round(solar_noon - ha_deg * 4.0);
  sunset_min  = (int)round(solar_noon + ha_deg * 4.0);

  // Clamp to [0, 1439] (edge cases near date boundary)
  auto clamp = [](int v) { return v < 0 ? 0 : v > 1439 ? 1439 : v; };
  sunrise_min = clamp(sunrise_min);
  sunset_min  = clamp(sunset_min);

  return true;
}

// Minutes from local midnight for the current moment.
inline int localMinutesNow(time_t utc_unix, float tz_offset_hours) {
  const time_t local_ts = utc_unix + (time_t)(tz_offset_hours * 3600.0f);
  struct tm tm_local;
  gmtime_r(&local_ts, &tm_local);
  return tm_local.tm_hour * 60 + tm_local.tm_min;
}

} // namespace SunCalc
