#pragma once
// Post-mount micro-SD operating-clock raise (perf pass 2026-08-20; Heltec V4-R8 first).
//
// Every SPI-SD board mounts the card with a conservative ladder that tops out at 4 MHz,
// and SD.begin()'s clock is the operating clock for the whole session — so the card that
// is the PRIMARY store on the R8 (contacts, chat history, sync log, file manager) ran at
// ~400 KB/s on traces that carry the display at 80 MHz. Standard SD bring-up is "initialise
// slow, then raise": once the 4 MHz mount has proved the card, re-begin at SD_SPI_FAST_HZ
// and READ-VERIFY it — a probe file's first 512 bytes are read at the proven clock first
// and compared byte-for-byte after the raise (SPI-mode SD has no data CRC by default, so a
// bare "mount succeeded" would not catch a marginal clock). Any mismatch or failure drops
// straight back to the clock that just worked. Boards that do not define SD_SPI_FAST_HZ
// (or set it <= 4 MHz) get a no-op — nothing changes for the T-Deck/M9/Pager ladders.
//
// Returns the operating clock after the call (cur_hz if the raise was refused or failed),
// or 0 if the card could not be re-mounted at all (the caller treats that as unmounted).

#include <Arduino.h>

#if defined(ESP32)
#include <SD.h>
#include <SPI.h>
#include <string.h>

#ifndef SD_SPI_FAST_HZ
  #define SD_SPI_FAST_HZ 0
#endif

static inline uint32_t sdTryFastClock(uint8_t cs_pin, SPIClass& spi, uint32_t cur_hz, const char* tag) {
#if SD_SPI_FAST_HZ > 4000000
  if (cur_hz == 0 || cur_hz >= (uint32_t)SD_SPI_FAST_HZ) return cur_hz;

  // 1) Pick a probe file and capture its head at the proven clock.
  char    probe_path[96] = {0};
  uint8_t ref[512];
  size_t  ref_len = 0;
  {
    File root = SD.open("/");
    if (root && root.isDirectory()) {
      for (int i = 0; i < 16; ++i) {
        File e = root.openNextFile();
        if (!e) break;
        if (!e.isDirectory() && e.size() > 0 && e.path() && strlen(e.path()) < sizeof probe_path) {
          strlcpy(probe_path, e.path(), sizeof probe_path);
          ref_len = e.size() < sizeof ref ? e.size() : sizeof ref;
          if (e.read(ref, ref_len) != ref_len) { probe_path[0] = 0; ref_len = 0; }
          e.close();
          break;
        }
        e.close();
      }
      root.close();
    }
  }

  // 2) Re-begin at the fast clock.
  SD.end();
  delay(20);
  bool ok = SD.begin(cs_pin, spi, SD_SPI_FAST_HZ, "/sd", 6) && SD.cardType() != CARD_NONE;

  // 3) Verify: directory walk always; byte-compare of the probe head when we have one.
  if (ok) {
    File root = SD.open("/");
    ok = root && root.isDirectory();
    if (ok) { File e = root.openNextFile(); if (e) e.close(); root.close(); }
  }
  if (ok && probe_path[0]) {
    uint8_t now[512];
    File p = SD.open(probe_path, FILE_READ);
    ok = p && p.read(now, ref_len) == ref_len && memcmp(now, ref, ref_len) == 0;
    if (p) p.close();
  }
  if (ok) {
    Serial.printf("[%s] SD clock %lu -> %lu Hz (%s)\n", tag, (unsigned long)cur_hz,
                  (unsigned long)SD_SPI_FAST_HZ, probe_path[0] ? "read-verified" : "dir-verified, no probe file");
    return (uint32_t)SD_SPI_FAST_HZ;
  }

  // 4) Fall back to the clock that just worked.
  Serial.printf("[%s] SD %lu Hz verify FAILED; back to %lu Hz\n", tag,
                (unsigned long)SD_SPI_FAST_HZ, (unsigned long)cur_hz);
  for (int attempt = 0; attempt < 2; ++attempt) {
    SD.end();
    delay(attempt == 0 ? 60 : 150);
    if (SD.begin(cs_pin, spi, cur_hz, "/sd", 6) && SD.cardType() != CARD_NONE) return cur_hz;
  }
  Serial.printf("[%s] SD remount at %lu Hz failed\n", tag, (unsigned long)cur_hz);
  return 0;
#else
  (void)cs_pin; (void)spi; (void)tag;
  return cur_hz;
#endif
}
#endif  // ESP32
