// SPDX-License-Identifier: GPL-3.0-or-later
// Cardputer Zero GPS: NMEA from the Cap LoRa-1262's GPS on the EXT UART (/dev/serial0)
// becomes the UI's GPS fix (the web board's host-location path, platform/WebLocation.h).
//   WADA_ZERO_GPS=/dev/serial0   the port (default); WADA_ZERO_GPS=off to skip it
//   WADA_ZERO_GPS_BAUD=115200    fixed baud (default: try 115200, 9600, 38400 in turn)
//   WADA_ZERO_GPS_FORCE=1        read it even if another program has it open
// A port another program already has open (gpsd, a MeshCore build using the GPS) is left
// alone, since two readers would each get half the sentences.
#include "zero_gps.h"
#include <Arduino.h>
#include "WebLocation.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>
#include <sys/stat.h>

namespace {
int s_fd = -1;
std::string s_path;
const int kBauds[] = {115200, 9600, 38400};
int s_baud_i = 0, s_fixed_baud = 0;
uint32_t s_try_ms = 0, s_last_good_ms = 0, s_last_fix_ms = 0;
bool s_locked = false, s_had_fix = false;
char s_line[128];
size_t s_len = 0;
double s_alt = NAN;
int s_sats = 0;
double s_lat = NAN, s_lon = NAN, s_speed = NAN;
bool s_fix_now = false;

// ---- logging: a GPX track and a WiGLE-format Wi-Fi log, in $WADA_DATA_DIR/logs ----------
bool s_gpx_on = false, s_wifi_on = false;
FILE* s_gpx = nullptr;
std::string s_gpx_path;
double s_gpx_lat = NAN, s_gpx_lon = NAN;
int s_gpx_points = 0;
std::mutex s_wifi_mu;
std::atomic<bool> s_wifi_run{false};
std::thread s_wifi_thread;
int s_wifi_seen = 0;

std::string logDir() {
  const char* d = getenv("WADA_DATA_DIR");
  std::string p = std::string(d && d[0] ? d : ".") + "/logs";
  mkdir(p.c_str(), 0755);
  return p;
}
std::string stamp(const char* fmt) {
  char b[64]; time_t t = time(nullptr); tm u{}; gmtime_r(&t, &u); strftime(b, sizeof b, fmt, &u); return b;
}
double metres(double la1, double lo1, double la2, double lo2) {
  const double k = M_PI / 180.0, x = (lo2 - lo1) * k * cos((la1 + la2) * k / 2), y = (la2 - la1) * k;
  return sqrt(x * x + y * y) * 6371000.0;
}
void gpxClose() {
  if (!s_gpx) return;
  fputs("</trkseg></trk></gpx>\n", s_gpx);
  fclose(s_gpx); s_gpx = nullptr;
}
void gpxPoint(double lat, double lon) {
  if (!s_gpx_on) return;
  if (!isnan(s_gpx_lat) && metres(s_gpx_lat, s_gpx_lon, lat, lon) < 10.0) return;   // a point every 10 m
  if (!s_gpx) {
    s_gpx_path = logDir() + "/track-" + stamp("%Y%m%d-%H%M%S") + ".gpx";
    s_gpx = fopen(s_gpx_path.c_str(), "w");
    if (!s_gpx) return;
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"wadamesh-zero\" "
          "xmlns=\"http://www.topografix.com/GPX/1/1\">\n<trk><name>Cardputer Zero</name><trkseg>\n", s_gpx);
    s_gpx_points = 0;
  }
  if (isnan(s_alt)) fprintf(s_gpx, "<trkpt lat=\"%.6f\" lon=\"%.6f\"><time>%s</time></trkpt>\n", lat, lon, stamp("%Y-%m-%dT%H:%M:%SZ").c_str());
  else fprintf(s_gpx, "<trkpt lat=\"%.6f\" lon=\"%.6f\"><ele>%.1f</ele><time>%s</time></trkpt>\n", lat, lon, s_alt, stamp("%Y-%m-%dT%H:%M:%SZ").c_str());
  fflush(s_gpx);
  s_gpx_lat = lat; s_gpx_lon = lon; s_gpx_points++;
}

// Wi-Fi: NetworkManager's scan list every 30 s, written with the fix in WiGLE's CSV
// format (upload-ready). nmcli runs as the normal user; no monitor mode, nothing sent.
void wifiLoop() {
  const std::string path = logDir() + "/wifi-" + stamp("%Y%m%d-%H%M%S") + ".wiglecsv";
  FILE* f = fopen(path.c_str(), "w");
  if (!f) return;
  fputs("WigleWifi-1.4,appRelease=wadamesh-zero,model=CardputerZero,release=1,device=CardputerZero,display=,board=,brand=M5Stack\n"
        "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type\n", f);
  fflush(f);
  while (s_wifi_run) {
    double lat, lon, alt; bool fix;
    { std::lock_guard<std::mutex> l(s_wifi_mu); lat = s_lat; lon = s_lon; alt = s_alt; fix = s_fix_now; }
    if (fix) {
      if (FILE* p = popen("nmcli -t -e yes -f BSSID,SSID,SECURITY,CHAN,SIGNAL dev wifi list --rescan auto 2>/dev/null", "r")) {
        char line[512];
        const std::string when = stamp("%Y-%m-%d %H:%M:%S");
        while (fgets(line, sizeof line, p)) {
          // nmcli -t escapes ':' inside fields as '\:'; split on the unescaped ones.
          std::string fld[5]; int k = 0;
          for (char* c = line; *c && *c != '\n' && k < 5; ++c) {
            if (*c == '\\' && c[1]) { fld[k] += *++c; continue; }
            if (*c == ':') { ++k; continue; }
            fld[k] += *c;
          }
          if (fld[0].empty()) continue;
          for (auto& ch : fld[1]) if (ch == ',') ch = ' ';
          const int rssi = atoi(fld[4].c_str()) / 2 - 100;   // nmcli gives 0..100 quality
          fprintf(f, "%s,%s,[%s],%s,%s,%d,%.6f,%.6f,%.1f,10,WIFI\n", fld[0].c_str(), fld[1].c_str(),
                  fld[2].c_str(), when.c_str(), fld[3].c_str(), rssi, lat, lon, isnan(alt) ? 0.0 : alt);
          s_wifi_seen++;
        }
        pclose(p);
        fflush(f);
      }
    }
    for (int i = 0; i < 300 && s_wifi_run; ++i) usleep(100 * 1000);   // 30 s, quick to stop
  }
  fclose(f);
}

speed_t speedOf(int b) { return b == 9600 ? B9600 : b == 38400 ? B38400 : b == 57600 ? B57600 : B115200; }

void setBaud(int b) {
  termios t{};
  if (tcgetattr(s_fd, &t)) return;
  cfmakeraw(&t);
  cfsetispeed(&t, speedOf(b)); cfsetospeed(&t, speedOf(b));
  t.c_cflag |= CLOCAL | CREAD;
  tcsetattr(s_fd, TCSANOW, &t);
  tcflush(s_fd, TCIFLUSH);
  s_try_ms = millis();
}

bool checksumOk(const char* l) {
  const char* star = strchr(l, '*');
  if (l[0] != '$' || !star || strlen(star) < 3) return false;
  uint8_t c = 0;
  for (const char* p = l + 1; p < star; ++p) c ^= (uint8_t)*p;
  return c == (uint8_t)strtol(star + 1, nullptr, 16);
}

// Splits on ',' in place; returns the field count.
int fields(char* l, char** f, int max) {
  int n = 0;
  if (char* star = strchr(l, '*')) *star = 0;
  for (char* p = l; n < max;) {
    f[n++] = p;
    char* c = strchr(p, ',');
    if (!c) break;
    *c = 0; p = c + 1;
  }
  return n;
}

double coord(const char* v, const char* hemi, int deg_digits) {
  if (!v[0] || !hemi[0]) return NAN;
  char d[4] = {0};
  memcpy(d, v, deg_digits);
  double deg = atof(d) + atof(v + deg_digits) / 60.0;
  return (hemi[0] == 'S' || hemi[0] == 'W') ? -deg : deg;
}

void sentence(char* l) {
  char* f[24];
  const int n = fields(l, f, 24);
  const char* type = f[0] + 3;   // skip "$GP" / "$GN" / "$GB"...
  if (!strcmp(type, "GGA") && n > 9) {
    if (atoi(f[6]) > 0 && f[9][0]) s_alt = atof(f[9]);
    s_sats = atoi(f[7]);
    return;
  }
  if (strcmp(type, "RMC") || n < 10) return;
  if (f[2][0] != 'A') {   // no fix
    { std::lock_guard<std::mutex> l(s_wifi_mu); s_fix_now = false; }
    if (s_had_fix && millis() - s_last_fix_ms > 30000) { wadaHostLocationLost(2); s_had_fix = false; }
    return;
  }
  const double lat = coord(f[3], f[4], 2), lon = coord(f[5], f[6], 3);
  if (isnan(lat) || isnan(lon)) return;
  const double spd = f[7][0] ? atof(f[7]) * 0.514444 : NAN;   // knots -> m/s
  const double crs = f[8][0] ? atof(f[8]) : NAN;
  double epoch = (double)time(nullptr);
  if (strlen(f[1]) >= 6 && strlen(f[9]) == 6) {
    tm t{};
    t.tm_hour = (f[1][0] - '0') * 10 + (f[1][1] - '0');
    t.tm_min = (f[1][2] - '0') * 10 + (f[1][3] - '0');
    t.tm_sec = (f[1][4] - '0') * 10 + (f[1][5] - '0');
    t.tm_mday = (f[9][0] - '0') * 10 + (f[9][1] - '0');
    t.tm_mon = (f[9][2] - '0') * 10 + (f[9][3] - '0') - 1;
    t.tm_year = (f[9][4] - '0') * 10 + (f[9][5] - '0') + 100;
    epoch = (double)timegm(&t);
  }
  wadaHostLocationFix(lat, lon, NAN, s_alt, spd, crs, epoch);
  { std::lock_guard<std::mutex> l(s_wifi_mu); s_lat = lat; s_lon = lon; s_speed = spd; s_fix_now = true; }
  gpxPoint(lat, lon);
  if (!s_had_fix) fprintf(stderr, "[zero] gps: fix %.5f, %.5f\n", lat, lon);
  s_had_fix = true;
  s_last_fix_ms = millis();
}
}  // namespace

void zeroGpsBegin() {
  const char* e = getenv("WADA_ZERO_GPS");
  if (e && !strcmp(e, "off")) return;
  s_path = (e && e[0]) ? e : "/dev/serial0";
  if (const char* b = getenv("WADA_ZERO_GPS_BAUD")) s_fixed_baud = atoi(b);
  // Leave a port that something else is reading.
  std::string cmd = "fuser " + s_path + " 2>/dev/null";
  if (FILE* p = getenv("WADA_ZERO_GPS_FORCE") ? nullptr : popen(cmd.c_str(), "r")) {
    char buf[128] = {0}; const size_t n = fread(buf, 1, sizeof buf - 1, p); pclose(p);
    if (n && strspn(buf, " \t\n") != strlen(buf)) {
      fprintf(stderr, "[zero] gps: %s is in use by pid%s- not reading it (WADA_ZERO_GPS=off to silence)\n", s_path.c_str(), buf);
      return;
    }
  }
  s_fd = open(s_path.c_str(), O_RDONLY | O_NOCTTY | O_NONBLOCK);
  if (s_fd < 0) { fprintf(stderr, "[zero] gps: %s: %s\n", s_path.c_str(), strerror(errno)); return; }
  setBaud(s_fixed_baud ? s_fixed_baud : kBauds[0]);
  fprintf(stderr, "[zero] gps: reading %s\n", s_path.c_str());
}

void zeroGpsPoll() {
  if (s_fd < 0) return;
  char buf[256];
  for (;;) {
    const ssize_t n = read(s_fd, buf, sizeof buf);
    if (n <= 0) break;
    for (ssize_t i = 0; i < n; ++i) {
      const char c = buf[i];
      if (c == '$') s_len = 0;
      if (c == '\r' || c == '\n') {
        if (s_len > 6) {
          s_line[s_len] = 0;
          if (checksumOk(s_line)) {
            if (!s_locked) {
              s_locked = true;
              fprintf(stderr, "[zero] gps: NMEA at %d baud\n", s_fixed_baud ? s_fixed_baud : kBauds[s_baud_i]);
            }
            s_last_good_ms = millis();
            sentence(s_line);
          }
        }
        s_len = 0;
        continue;
      }
      if (s_len < sizeof s_line - 1) s_line[s_len++] = c; else s_len = 0;
    }
  }
  // Nothing readable at this speed for 3 s: try the next one (until a speed works).
  if (!s_fixed_baud && !s_locked && millis() - s_try_ms > 3000) {
    s_baud_i = (s_baud_i + 1) % (int)(sizeof kBauds / sizeof kBauds[0]);
    setBaud(kBauds[s_baud_i]);
  }
}

ZeroGpsStatus zeroGpsStatus() {
  ZeroGpsStatus g;
  g.port = s_fd >= 0; g.path = s_path; g.nmea = s_locked && millis() - s_last_good_ms < 5000;
  { std::lock_guard<std::mutex> l(s_wifi_mu); g.fix = s_fix_now && millis() - s_last_fix_ms < 5000; g.lat = s_lat; g.lon = s_lon; g.speed_mps = s_speed; }
  g.sats = s_sats; g.alt = s_alt;
  g.gpx_on = s_gpx_on; g.gpx_points = s_gpx_points; g.gpx_path = s_gpx_path;
  g.wifi_on = s_wifi_on; g.wifi_seen = s_wifi_seen;
  return g;
}
void zeroGpsSetGpx(bool on) {
  if (on == s_gpx_on) return;
  s_gpx_on = on;
  if (!on) gpxClose();
  s_gpx_lat = s_gpx_lon = NAN;
  fprintf(stderr, "[zero] gpx log %s\n", on ? "on" : "off");
}
void zeroGpsSetWifiLog(bool on) {
  if (on == s_wifi_on) return;
  s_wifi_on = on;
  if (on) { s_wifi_run = true; s_wifi_thread = std::thread(wifiLoop); }
  else { s_wifi_run = false; if (s_wifi_thread.joinable()) s_wifi_thread.join(); }
  fprintf(stderr, "[zero] wifi log %s\n", on ? "on" : "off");
}
void zeroGpsShutdown() { zeroGpsSetWifiLog(false); gpxClose(); }

long WebLocationProvider::satellitesCount() { return s_sats; }
