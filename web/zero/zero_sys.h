// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
struct ZeroSys {
  std::string host, iface, ip, ssid;
  double cpu_temp_c = -1, load1 = 0;
  long uptime_s = 0, disk_free_mb = 0;
  int mem_total_mb = 0, mem_avail_mb = 0;
};
ZeroSys zeroSys();
std::string zeroUptimeText(long secs);
