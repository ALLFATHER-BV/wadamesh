// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for GPS Auto speed (32533c45, CAP_GPS_BAUD_FIND on the T-Deck and the M9):
// the real src/helpers/WadaNmeaLocationProvider.h, with its UART probe, against a
// simulated receiver that talks NMEA at one speed (noise at every other speed), and
// the real speed rules of touchPrefsGetGpsBaud / touchPrefsSetGpsBaudFound
// (src/helpers/esp32/GpsBaudPrefs.h) over a fake NVS that counts writes.
//
//   c++ -std=c++17 -Wall -Wextra -DESP32 -I src -I test/gps_probe_fakes test/test_gps_baud_probe.cpp \
//       -o /tmp/test_gps_baud_probe && /tmp/test_gps_baud_probe
#include <assert.h>
#include <stdio.h>

#include "helpers/WadaNmeaLocationProvider.h"
#include "helpers/esp32/GpsBaudPrefs.h"

uint32_t g_test_ms = 0;

// ---- the prefs the provider calls, over a fake NVS (TouchPrefsStore.cpp's wrappers) ----
static struct {
  uint32_t chosen = 0;              // s_cfg.gps_baud (settings blob)
  bool have_found = false;          // isKey("gps_bd_fd")
  uint32_t found = 0;
  int found_writes = 0;             // prefsPutUInt("gps_bd_fd")
  int chosen_writes = 0;            // touchPrefsSetGpsBaud (cfgFlush)
} nvs;

uint32_t touchPrefsGetGpsBaud(uint32_t fallback) {
  if (nvs.chosen != 0) return nvs.chosen;
  return GpsBaudPrefs::effective(0, nvs.have_found ? nvs.found : 0, fallback);
}
bool touchPrefsGpsBaudChosen() { return nvs.chosen != 0; }
void touchPrefsSetGpsBaudFound(uint32_t baud) {
  if (!GpsBaudPrefs::sane(baud)) return;
  if (!GpsBaudPrefs::foundNeedsWrite(baud, nvs.have_found, nvs.have_found ? nvs.found : 0)) return;
  nvs.have_found = true; nvs.found = baud; ++nvs.found_writes;
}
bool touchPrefsSetGpsBaud(uint32_t baud) { nvs.chosen = baud; ++nvs.chosen_writes; return true; }

static void resetNvs() { nvs = {}; nvs.chosen = 0; }

// A boot: the core opens Serial1 at touchPrefsGetGpsBaud(GPS_BAUD_RATE) (initBasicGPS),
// then the provider begins. The receiver keeps talking at `true_baud`.
struct Board {
  HardwareSerial uart;
  mesh::RTCClock clock;
  WadaNmeaLocationProvider* gps = nullptr;
  uint32_t default_baud;
  Board(uint32_t true_baud, uint32_t def, bool keep_choice = true) : default_baud(def) {
    uart.true_baud = true_baud;
    uart.begin(touchPrefsGetGpsBaud(def));
    gps = new WadaNmeaLocationProvider(uart, &clock, -1, -1, 18, 17, def, keep_choice);
    gps->begin();
  }
  ~Board() { delete gps; }
  // Run the loop task for `ms`, a pass every 10 ms. Returns when the UART has carried
  // a valid sentence at the receiver's speed and stayed there, or at the end.
  void run(uint32_t ms) {
    const uint32_t end = g_test_ms + ms;
    while ((int32_t)(g_test_ms - end) < 0) { gps->loop(); g_test_ms += 10; }
  }
};

int main() {
  g_test_ms = 100000;

  // ---- T-Deck (default 38400), a 9600 receiver, nothing picked or found ----
  {
    resetNvs();
    Board b(9600, 38400);
    assert(b.uart.baud == 38400);              // the first start opens at the default
    b.run(60000);
    assert(b.uart.baud == 9600);               // the probe found it...
    assert(b.uart.begun_at.size() == 2 && b.uart.begun_at[1] == 9600);   // ...on the first try after 38400
    assert(nvs.have_found && nvs.found == 9600 && nvs.found_writes == 1); // stored once
    assert(nvs.chosen == 0 && nvs.chosen_writes == 0);   // never as the user's choice
    const int begins = b.uart.begins;
    b.run(120000);                             // two more minutes of valid NMEA
    assert(b.uart.begins == begins && nvs.found_writes == 1);   // locked: no reopen, no write
    // the GPS saver rests it and wakes it: still no write, no probing
    b.gps->stop(); b.run(5000); b.gps->begin(); b.run(30000);
    assert(b.uart.baud == 9600 && nvs.found_writes == 1);
    assert(b.uart.begins == begins);
  }
  // ---- the next start opens at the found 9600 and locks at once, writing nothing ----
  {
    Board b(9600, 38400);
    assert(b.uart.baud == 9600);
    b.run(60000);
    assert(b.uart.begins == 1 && nvs.found_writes == 1);
  }

  // ---- a 38400 receiver, M9-style default 115200: found, stored once ----
  {
    resetNvs();
    Board b(38400, 115200);
    b.run(90000);
    assert(b.uart.baud == 38400);
    assert(nvs.found == 38400 && nvs.found_writes == 1);
    // probe order after the default: 9600, 19200, 38400, at 9 s then every 5 s
    assert(b.uart.begun_at.size() == 4);
    assert(b.uart.begun_at[1] == 9600 && b.uart.begun_at[2] == 19200 && b.uart.begun_at[3] == 38400);
  }

  // ---- a 38400 receiver on the T-Deck default 38400: locks, nothing stored ----
  {
    resetNvs();
    Board b(38400, 38400);
    b.run(60000);
    assert(b.uart.begins == 1 && b.uart.baud == 38400);
    assert(nvs.found_writes == 0 && !nvs.have_found);
  }

  // ---- a picked speed is used as it is: no probing, nothing stored ----
  {
    resetNvs();
    nvs.chosen = 115200;                       // the user picked 115200; the receiver talks 9600
    Board b(9600, 38400);
    assert(b.uart.baud == 115200);
    b.run(120000);
    assert(b.uart.begins == 1 && b.uart.baud == 115200);
    assert(nvs.found_writes == 0 && nvs.chosen == 115200 && nvs.chosen_writes == 0);
  }
  // ---- ...and a found speed from before does not override the pick ----
  {
    resetNvs();
    nvs.have_found = true; nvs.found = 9600; nvs.chosen = 57600;
    Board b(9600, 38400);
    assert(b.uart.baud == 57600);
    b.run(60000);
    assert(b.uart.begins == 1 && nvs.found == 9600 && nvs.found_writes == 0);
  }

  // ---- a swapped receiver: found 9600 stored, the new one talks 38400 ----
  {
    resetNvs();
    nvs.have_found = true; nvs.found = 9600;
    Board b(38400, 38400);
    assert(b.uart.baud == 9600);               // opens at the old found speed
    b.run(60000);
    assert(b.uart.baud == 38400 && nvs.found == 38400 && nvs.found_writes == 1);
  }

  // ---- no receiver at all: the probe keeps cycling, never stores anything ----
  {
    resetNvs();
    Board b(0, 38400);
    b.run(10 * 60000);
    assert(nvs.found_writes == 0 && !nvs.have_found);
    assert(b.uart.begins > 50);                // every 5 s after the first 9 s
    for (uint32_t s : b.uart.begun_at)
      assert(s == 9600 || s == 19200 || s == 38400 || s == 57600 || s == 115200);
  }

  // ---- the GPS saver stops it mid-probe (receiver still silent), then it talks ----
  // begin() takes the stored speed as the UART's without reopening it, so the probe
  // skips that speed on its first pass; it must still find it on the next one.
  {
    resetNvs();
    Board b(0, 38400);                         // asleep: nothing on the line
    b.run(30000);                              // the probe has moved the UART off 38400
    assert(b.uart.baud != 38400);
    b.gps->stop(); b.run(2000);
    b.uart.true_baud = 38400;                  // awake, talking at the default
    b.gps->begin();
    b.run(120000);
    assert(b.uart.baud == 38400);
    assert(nvs.found_writes == 0);             // the default needs no stored speed
  }

  // ---- noise only (a receiver at a speed not in the list): nothing locks on noise ----
  {
    resetNvs();
    Board b(4800, 38400);
    b.run(10 * 60000);
    assert(nvs.found_writes == 0);
  }

  // ---- CAP_GPS_BAUD_AUTO boards (keep_choice false): the found speed becomes the setting ----
  {
    resetNvs();
    Board b(9600, 38400, false);
    b.run(60000);
    assert(b.uart.baud == 9600 && nvs.chosen == 9600 && nvs.chosen_writes == 1);
    assert(nvs.found_writes == 0);
  }

  // ---- the rules themselves ----
  assert(GpsBaudPrefs::effective(0, 0, 38400) == 38400);
  assert(GpsBaudPrefs::effective(0, 9600, 38400) == 9600);
  assert(GpsBaudPrefs::effective(0, 1234, 38400) == 38400);       // insane found: ignored
  assert(GpsBaudPrefs::effective(0, 0xFFFFFFFFu, 38400) == 38400);
  assert(GpsBaudPrefs::effective(115200, 9600, 38400) == 115200); // a choice wins
  assert(GpsBaudPrefs::foundNeedsWrite(9600, false, 0));
  assert(!GpsBaudPrefs::foundNeedsWrite(9600, true, 9600));
  assert(GpsBaudPrefs::foundNeedsWrite(38400, true, 9600));
  assert(!GpsBaudPrefs::foundNeedsWrite(0, false, 0));
  assert(!GpsBaudPrefs::foundNeedsWrite(1000000, false, 0));

  printf("test_gps_baud_probe: ok\n");
  return 0;
}
