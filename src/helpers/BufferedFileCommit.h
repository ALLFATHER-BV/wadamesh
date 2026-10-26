// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Write a payload that is already complete in RAM to a file, with whole-file retries.
//
// Used by "Save update bin to SD" (#734). That download used to write each network
// chunk to the card as it arrived, on the core-0 tile worker, next to the Wi-Fi
// driver's high-priority tasks. sd_diskio's busy-waits measure wall time, so a
// healthy card can time out there under heavy Wi-Fi RX (the reason the chat-history
// flush moved to core 1), and one failed FAT write leaves the file failed for good:
// FatFs keeps the error on the file object, so every later write returns 0. Retrying
// that one write cannot help, and the stream cannot be replayed.
//
// With the image downloaded into PSRAM first, the card is written after the Wi-Fi
// burst is over, the way map tiles are (download, then write), and a failed attempt
// can start over: discard the partial file, reopen, write again from byte 0.
//
// Bytes go through `bounce` (caller-provided, internal DMA-capable RAM): an SD card
// must never be written straight from a PSRAM buffer.
//
// Pure: no Arduino or ESP-IDF types, so test/test_buffered_file_commit.cpp runs it
// on the host. The Sink supplies the file system:
//   bool   open();                                  // create/truncate the target
//   size_t write(const uint8_t* p, size_t n);       // bytes taken, 0 = failed
//   bool   close();                                 // false if the file is short/bad
//   void   discard();                               // remove a partial file
//   void   pause(int next_attempt);                 // back off before a retry
//   void   progress(size_t done, size_t total);     // after each chunk
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace BufferedFileCommit {

template <class Sink>
bool commit(Sink& sink, const uint8_t* data, size_t len,
            uint8_t* bounce, size_t bounce_sz, int attempts) {
  if (!data || !bounce || bounce_sz == 0 || attempts < 1) return false;
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    if (attempt > 1) sink.pause(attempt);
    if (!sink.open()) continue;
    bool ok = true;
    size_t done = 0;
    while (ok && done < len) {
      size_t n = len - done;
      if (n > bounce_sz) n = bounce_sz;
      memcpy(bounce, data + done, n);
      size_t put = 0;
      while (put < n) {   // a busy card may take part of a chunk: push the rest
        const size_t w = sink.write(bounce + put, n - put);
        if (w == 0 || w > n - put) { ok = false; break; }
        put += w;
      }
      if (!ok) break;
      done += n;
      sink.progress(done, len);
    }
    if (!sink.close()) ok = false;
    if (ok) return true;
    sink.discard();
  }
  return false;
}

}  // namespace BufferedFileCommit
