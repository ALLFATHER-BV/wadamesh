// SPDX-License-Identifier: GPL-3.0-or-later
// Cardputer Zero battery: the real pack instead of the Pager simulator's fixed 76 %.
// Source 1: a Linux power_supply of type Battery (the bq27xxx driver for the BQ27220).
// Source 2: the BQ27220 fuel gauge read directly over I2C (address 0x55) when no driver
// has claimed it. Readings are cached for 2 s (the UI asks several times a second).
#include "zero_battery.h"
#include <Arduino.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string>

namespace {
ZeroBattery s_b;
uint32_t s_at = 0;
bool s_probed = false;
std::string s_sys_bat, s_sys_ext;   // /sys/class/power_supply/<name>
int s_i2c_fd = -1;

bool readStr(const std::string& path, char* out, size_t cap) {
  FILE* f = fopen(path.c_str(), "r");
  if (!f) return false;
  bool ok = fgets(out, (int)cap, f) != nullptr;
  fclose(f);
  if (ok) { size_t n = strlen(out); while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0; }
  return ok;
}
bool readLong(const std::string& path, long* v) {
  char b[64];
  if (!readStr(path, b, sizeof b)) return false;
  *v = strtol(b, nullptr, 10);
  return true;
}

int i2cWord(uint8_t reg, bool* ok) {
  uint8_t r = reg, d[2] = {0, 0};
  *ok = write(s_i2c_fd, &r, 1) == 1 && read(s_i2c_fd, d, 2) == 2;
  return d[0] | (d[1] << 8);
}

void probe() {
  s_probed = true;
  const char* base = getenv("WADA_ZERO_PSU_DIR") ? getenv("WADA_ZERO_PSU_DIR") : "/sys/class/power_supply";   // tests
  if (DIR* d = opendir(base)) {
    for (dirent* e; (e = readdir(d));) {
      if (e->d_name[0] == '.') continue;
      std::string p = std::string(base) + "/" + e->d_name;
      char type[32] = "";
      readStr(p + "/type", type, sizeof type);
      if (!strcmp(type, "Battery") && s_sys_bat.empty()) s_sys_bat = p;
      else if ((!strcmp(type, "Mains") || !strncmp(type, "USB", 3)) && s_sys_ext.empty()) s_sys_ext = p;
    }
    closedir(d);
  }
  if (!s_sys_bat.empty()) { fprintf(stderr, "[zero] battery: %s\n", s_sys_bat.c_str()); return; }
  // No driver: look for the BQ27220 on the I2C buses (0x55; its DeviceType answer is 0x0220).
  for (int bus = 0; bus < 23 && s_i2c_fd < 0; ++bus) {
    char dev[32]; snprintf(dev, sizeof dev, "/dev/i2c-%d", bus);
    int fd = open(dev, O_RDWR);
    if (fd < 0) continue;
    if (ioctl(fd, I2C_SLAVE_FORCE, 0x55) == 0) {
      s_i2c_fd = fd;
      bool ok = false;
      const int v = i2cWord(0x08, &ok);   // Voltage, mV
      if (ok && v > 2500 && v < 5000) { fprintf(stderr, "[zero] battery: BQ27220 on %s (%d mV)\n", dev, v); return; }
      s_i2c_fd = -1;
    }
    close(fd);
  }
  fprintf(stderr, "[zero] battery: no gauge found (power_supply or BQ27220 @0x55)\n");
}

void refresh() {
  if (!s_probed) probe();
  ZeroBattery b{};
  if (!s_sys_bat.empty()) {
    long v;
    if (readLong(s_sys_bat + "/capacity", &v)) { b.valid = true; b.soc = (int)v; }
    if (readLong(s_sys_bat + "/voltage_now", &v)) b.mv = (int)(v / 1000);
    if (readLong(s_sys_bat + "/current_now", &v)) b.ma = (int)(v / 1000);
    if (readLong(s_sys_bat + "/charge_full", &v)) b.full_mah = (int)(v / 1000);
    if (readLong(s_sys_bat + "/charge_now", &v)) b.remaining_mah = (int)(v / 1000);
    if (readLong(s_sys_bat + "/time_to_empty_now", &v)) b.to_empty_min = (int)(v / 60);
    if (readLong(s_sys_bat + "/time_to_full_now", &v)) b.to_full_min = (int)(v / 60);
    if (readLong(s_sys_bat + "/temp", &v)) b.temp_c10 = (int)v;
    if (readLong(s_sys_bat + "/cycle_count", &v)) b.cycles = (int)v;
    char st[32] = "";
    readStr(s_sys_bat + "/status", st, sizeof st);
    b.charging = !strcmp(st, "Charging");
    b.full = !strcmp(st, "Full");
    b.external = b.charging || b.full || !strcmp(st, "Not charging");
    if (!s_sys_ext.empty() && readLong(s_sys_ext + "/online", &v)) b.external = b.external || v != 0;
  } else if (s_i2c_fd >= 0) {
    bool ok = false;
    const int soc = i2cWord(0x2C, &ok);                      // StateOfCharge, %
    if (ok && soc >= 0 && soc <= 100) { b.valid = true; b.soc = soc; }
    b.mv = i2cWord(0x08, &ok);                                // Voltage, mV
    b.ma = (int16_t)i2cWord(0x0C, &ok);                       // Current, mA (+ charging)
    b.remaining_mah = i2cWord(0x10, &ok);                     // RemainingCapacity
    b.full_mah = i2cWord(0x12, &ok);                          // FullChargeCapacity
    const int tte = i2cWord(0x16, &ok);                       // TimeToEmpty, min (65535 = n/a)
    b.to_empty_min = (ok && tte != 0xFFFF) ? tte : -1;
    const int ttf = i2cWord(0x18, &ok);                       // TimeToFull
    b.to_full_min = (ok && ttf != 0xFFFF) ? ttf : -1;
    const int tk = i2cWord(0x06, &ok);                        // Temperature, 0.1 K
    b.temp_c10 = ok ? tk - 2732 : 250;
    b.cycles = i2cWord(0x2A, &ok);
    const int st = i2cWord(0x0A, &ok);                        // BatteryStatus: bit0 DSG (discharging)
    b.charging = ok && !(st & 1) && b.ma > 5;
    b.full = ok && b.soc >= 100 && !(st & 1);
    b.external = ok && !(st & 1);
  }
  if (b.valid && b.soc > 100) b.soc = 100;
  if (!b.valid) { b.soc = 100; b.mv = 4100; }   // no gauge: don't alarm with low-battery warnings
  s_b = b;
}
}  // namespace

const ZeroBattery& zeroBattery() {
  const uint32_t now = millis();
  if (!s_at || now - s_at >= 2000) { s_at = now ? now : 1; refresh(); }
  return s_b;
}
