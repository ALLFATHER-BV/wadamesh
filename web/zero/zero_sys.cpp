// SPDX-License-Identifier: GPL-3.0-or-later
// Cardputer Zero system facts for the boot log, the HUD and `wadamesh-cli status`:
// network (interface, IP, SSID), CPU temperature, load, uptime, memory, disk.
#include "zero_sys.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

namespace {
std::string s_ssid;
time_t s_ssid_at = 0;

std::string firstLine(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) return "";
  char b[256] = {0};
  if (!fgets(b, sizeof b, f)) b[0] = 0;
  fclose(f);
  size_t n = strlen(b);
  while (n && (b[n - 1] == '\n' || b[n - 1] == ' ')) b[--n] = 0;
  return b;
}
}  // namespace

ZeroSys zeroSys() {
  ZeroSys s;
  char host[64] = {0};
  gethostname(host, sizeof host - 1);
  s.host = host;
  // The first up, non-loopback IPv4 address; wlan before eth before anything else.
  ifaddrs* ifs = nullptr;
  if (getifaddrs(&ifs) == 0) {
    int best = 99;
    for (ifaddrs* i = ifs; i; i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
      const int rank = !strncmp(i->ifa_name, "wl", 2) ? 0 : !strncmp(i->ifa_name, "eth", 3) || !strncmp(i->ifa_name, "en", 2) ? 1 : 2;
      if (rank >= best) continue;
      char ip[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &((sockaddr_in*)i->ifa_addr)->sin_addr, ip, sizeof ip);
      s.iface = i->ifa_name; s.ip = ip; best = rank;
    }
    freeifaddrs(ifs);
  }
  // SSID: iwgetid is cheap but still a process; refresh at most every 15 s.
  if (time(nullptr) - s_ssid_at > 15) {
    s_ssid_at = time(nullptr);
    s_ssid.clear();
    if (FILE* p = popen("iwgetid -r 2>/dev/null", "r")) {
      char b[64] = {0};
      if (fgets(b, sizeof b, p)) { size_t n = strlen(b); while (n && b[n - 1] == '\n') b[--n] = 0; s_ssid = b; }
      pclose(p);
    }
  }
  s.ssid = s_ssid;
  const std::string t = firstLine("/sys/class/thermal/thermal_zone0/temp");
  s.cpu_temp_c = t.empty() ? -1 : atoi(t.c_str()) / 1000.0;
  const std::string la = firstLine("/proc/loadavg");
  s.load1 = atof(la.c_str());
  const std::string up = firstLine("/proc/uptime");
  s.uptime_s = (long)atof(up.c_str());
  if (FILE* f = fopen("/proc/meminfo", "r")) {
    char k[64]; long v; char unit[16];
    while (fscanf(f, "%63s %ld %15s", k, &v, unit) == 3) {
      if (!strcmp(k, "MemTotal:")) s.mem_total_mb = (int)(v / 1024);
      else if (!strcmp(k, "MemAvailable:")) s.mem_avail_mb = (int)(v / 1024);
    }
    fclose(f);
  }
  struct statvfs fs{};
  if (statvfs("/", &fs) == 0) s.disk_free_mb = (long)((double)fs.f_bavail * fs.f_frsize / (1024.0 * 1024.0));
  return s;
}

std::string zeroUptimeText(long secs) {
  char b[32];
  if (secs >= 86400) snprintf(b, sizeof b, "%ldd %02ldh%02ldm", secs / 86400, secs / 3600 % 24, secs / 60 % 60);
  else snprintf(b, sizeof b, "%ldh%02ldm%02lds", secs / 3600, secs / 60 % 60, secs % 60);
  return b;
}
