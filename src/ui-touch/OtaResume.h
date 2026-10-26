// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// The on-device update's copy loop (otaWorkerRun in UITask.cpp), lifted out so a host
// test can run it against fake HTTP, stream and Update objects (test/test_ota_resume.cpp).
//
// Copy by hand, yielding, instead of Update.writeStream(): that reads through
// Stream::timedRead, which spins without yielding until http's 20 s timeout, so a
// slow start starved IDLE0 on this core-0 worker and the 5 s task watchdog
// rebooted the device, "stuck at 0%, then a restart" (#689). Same loop as the SD
// download: wait with a delay, give up after 15 s without data.
//
// A connection that drops or stalls part way is picked up where it stopped with a Range
// request, up to 3 times, instead of failing with "short read" and starting over (#689:
// "after re-trying a few times, device was updated"). Only a 206 for exactly the rest is
// appended; anything else ends the update.
//
// Sys supplies the platform: uint32_t millis(), void delayMs(uint32_t) (vTaskDelay of
// pdMS_TO_TICKS), void yieldTick() (vTaskDelay(1)) and void log(const char*).
namespace OtaResume {

static constexpr uint32_t STALL_MS = 15000;
static constexpr int MAX_RESUMES = 3;

// Returns the bytes written to `upd`; write_failed is set when upd.write() came up short.
// `http` has had GET() answer 200 for `len` bytes already.
template <class Http, class Client, class Updater, class Sys>
size_t copy(Http& http, Client& client, const char* url, int len, Updater& upd,
            uint8_t* buf, size_t bufsz, bool& write_failed, Sys& sys) {
  size_t written = 0;
  write_failed = false;
  // No buffer (both mallocs failed): end the connection all the same. The worker reuses
  // this HTTPClient for the next job, and before the resume loop http.end() always ran.
  if (!buf) { http.end(); return 0; }
  for (int resume = 0; buf && !write_failed; ++resume) {
    auto* stream = http.getStreamPtr();   // NetworkClient* on arduino 3.x (P4: C6Client behind it)
    uint32_t last_data = sys.millis();
    while (stream && written < (size_t)len) {
      const int avail = stream->available();
      if (avail <= 0) {
        if (!http.connected() || (sys.millis() - last_data) > STALL_MS) break;
        sys.delayMs(10);
        continue;
      }
      const size_t want = ((size_t)avail > bufsz) ? bufsz : (size_t)avail;
      const int n = stream->read(buf, want);
      if (n <= 0) {
        if ((sys.millis() - last_data) > STALL_MS) break;
        sys.delayMs(5);
        continue;
      }
      last_data = sys.millis();
      if (upd.write(buf, (size_t)n) != (size_t)n) { write_failed = true; break; }   // reported by the caller
      written += (size_t)n;
      sys.yieldTick();   // let IDLE0 (and Wi-Fi) run between blocks
    }
    http.end();
    if (written >= (size_t)len || write_failed || resume >= MAX_RESUMES) break;
    char line[64];
    snprintf(line, sizeof line, "[OTA] stopped at %u/%d, resuming (%d)\n", (unsigned)written, len, resume + 1);
    sys.log(line);
    sys.delayMs(1000);
    if (!http.begin(client, url)) break;
    char range[40];
    snprintf(range, sizeof range, "bytes=%u-", (unsigned)written);
    http.addHeader("Range", range);
    const int rc = http.GET();
    // Only a 206 for exactly the rest is safe to append; anything else ends the update.
    if (rc != 206 || http.getSize() != (int)((size_t)len - written)) {
      snprintf(line, sizeof line, "[OTA] resume -> %d (%d bytes)\n", rc, http.getSize());
      sys.log(line);
      http.end();
      break;
    }
  }
  return written;
}

}  // namespace OtaResume
