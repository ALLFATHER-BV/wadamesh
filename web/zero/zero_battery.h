// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The Cardputer Zero's battery (zero_battery.cpp): Linux power_supply, else the BQ27220.
struct ZeroBattery {
  bool valid = false;        // a gauge answered
  int soc = 100;             // %
  int mv = 0, ma = 0;        // ma > 0 charging, < 0 discharging
  int remaining_mah = 0, full_mah = 0;
  int to_empty_min = -1, to_full_min = -1;
  int temp_c10 = 250, cycles = 0;
  bool charging = false, full = false, external = false;
};
const ZeroBattery& zeroBattery();
