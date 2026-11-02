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
// FsSink below is that Sink over an Arduino fs::FS (the production one).
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace BufferedFileCommit {

// Close a file just written and check how many bytes really landed, by opening it
// again. Never ask the still-open File: on arduino-esp32 File::size() of a file that
// has been written is stat(path), and FAT writes a file's size into its directory
// entry only on sync/close, so a fresh file reads 0 bytes until it is closed (plus
// up to 4 KB of stdio buffer that has not reached FatFs at all). Comparing that
// against the image length failed every attempt on every card (#734, beta_100).
// The reopen also catches a tail that failed in the final flush inside close().
template <class FsT, class FileT>
bool closeAndCheckSize(FsT& fs, FileT& f, const char* path, size_t want) {
  f.close();
  FileT r = fs.open(path, "r");
  const bool full = r && !r.isDirectory() && (size_t)r.size() == want;
  if (r) r.close();
  return full;
}

// The Sink over an Arduino-style FS (fs::FS + fs::File, or a host fake with the same
// calls). Hooks supplies pause(int) and progress(size_t, size_t).
template <class FsT, class FileT, class Hooks>
struct FsSink {
  FsT& fs; const char* path; size_t len; Hooks& hooks; FileT f;
  bool   open() { f = fs.open(path, "w"); return (bool)f; }
  size_t write(const uint8_t* p, size_t n) { return f.write(p, n); }
  bool   close() { return closeAndCheckSize(fs, f, path, len); }
  void   discard() { fs.remove(path); }
  void   pause(int attempt) { hooks.pause(attempt); }
  void   progress(size_t done, size_t total) { hooks.progress(done, total); }
};

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
