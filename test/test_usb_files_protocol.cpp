#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "helpers/esp32/UsbFilesProtocol.h"

using namespace UsbFiles;

static uint8_t g_buf[2 * kMaxFrame];

static size_t frame(uint8_t* out, uint8_t type, uint16_t seq, const char* text) {
  return encodeFrame(out, kMaxFrame, type, seq, reinterpret_cast<const uint8_t*>(text),
                     text ? strlen(text) : 0);
}

static bool pathOk(const char* p) { return pathValid(p, strlen(p)); }

static Access acc(Root r, const char* rel, bool ci) { return access(r, rel, ci); }

int main() {
  // CRC is the IEEE one (check value of "123456789").
  assert(crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);

  // ---- round trip ----
  uint8_t f[kMaxFrame];
  size_t n = frame(f, T_HELLO, 7, "hi");
  assert(n == kHeaderBytes + 2 + kTrailerBytes);
  assert(f[0] == kMagic0 && f[1] == kMagic1 && f[2] == T_HELLO);
  {
    FrameParser p(g_buf, sizeof g_buf);
    assert(p.push(f, n) == n);
    assert(p.next());
    assert(p.type() == T_HELLO && p.seq() == 7 && p.length() == 2);
    assert(memcmp(p.payload(), "hi", 2) == 0);
    assert(!p.next());
  }

  // ---- empty payload, and max payload ----
  {
    FrameParser p(g_buf, sizeof g_buf);
    n = encodeFrame(f, sizeof f, T_PING, 1, nullptr, 0);
    assert(n == kHeaderBytes + kTrailerBytes);
    p.push(f, n);
    assert(p.next() && p.type() == T_PING && p.length() == 0);

    static uint8_t big[kMaxPayload];
    for (size_t i = 0; i < sizeof big; ++i) big[i] = static_cast<uint8_t>(i * 7);
    n = encodeFrame(f, sizeof f, T_WRITE_DATA, 2, big, sizeof big);
    assert(n == kMaxFrame);
    p.push(f, n);
    assert(p.next() && p.length() == kMaxPayload && memcmp(p.payload(), big, sizeof big) == 0);
    assert(encodeFrame(f, sizeof f, T_WRITE_DATA, 2, big, sizeof big + 1) == 0);   // too long
    assert(encodeFrame(f, n - 1, T_WRITE_DATA, 2, big, sizeof big) == 0);          // no room
  }

  // ---- log text around frames, byte-at-a-time delivery ----
  {
    FrameParser p(g_buf, sizeof g_buf);
    const char* noise = "[E][vfs_api.cpp:104] open(): /spiffs/x does not exist\r\n";
    uint8_t stream[3 * kMaxFrame];
    size_t s = 0;
    memcpy(stream + s, noise, strlen(noise)); s += strlen(noise);
    s += frame(stream + s, T_LIST, 10, "/sd");
    memcpy(stream + s, noise, strlen(noise)); s += strlen(noise);
    stream[s++] = kMagic0;   // a stray half-magic
    s += frame(stream + s, T_STAT, 11, "/internal");
    int got = 0;
    uint16_t seqs[2] = {0, 0};
    for (size_t i = 0; i < s; ++i) {
      p.push(stream + i, 1);
      while (p.next()) seqs[got++] = p.seq();
    }
    assert(got == 2 && seqs[0] == 10 && seqs[1] == 11);
    assert(p.droppedBytes() > 0);
  }

  // ---- a frame with a lost byte, then its retransmission: the retransmit is found ----
  {
    FrameParser p(g_buf, sizeof g_buf);
    uint8_t a[kMaxFrame], b[kMaxFrame];
    const size_t na = frame(a, T_READ, 20, "0123456789abcdef/some/path");
    const size_t nb = frame(b, T_READ, 20, "0123456789abcdef/some/path");
    p.push(a, 10);
    p.push(a + 11, na - 11);   // byte 10 lost
    assert(!p.next());         // still waiting: the header claims more bytes
    p.push(b, nb);
    bool found = false;
    while (p.next()) { found = (p.seq() == 20 && p.type() == T_READ); }
    assert(found);
    assert(p.badFrames() >= 1);
  }

  // ---- corrupted CRC is rejected, the next frame still parses ----
  {
    FrameParser p(g_buf, sizeof g_buf);
    uint8_t a[kMaxFrame], b[kMaxFrame];
    const size_t na = frame(a, T_MKDIR, 30, "/sd/x");
    const size_t nb = frame(b, T_MKDIR, 31, "/sd/y");
    a[na - 1] ^= 0xFF;
    p.push(a, na);
    p.push(b, nb);
    assert(p.next() && p.seq() == 31);
    assert(!p.next());
  }

  // ---- impossible length does not stall the parser ----
  {
    FrameParser p(g_buf, sizeof g_buf);
    uint8_t junk[7] = {kMagic0, kMagic1, 1, 0, 0, 0xFF, 0xFF};   // len 65535
    p.push(junk, sizeof junk);
    n = frame(f, T_PING, 40, nullptr);
    p.push(f, n);
    assert(p.next() && p.seq() == 40);
  }

  // ---- paths ----
  assert(pathOk("/"));
  assert(pathOk("/sd"));
  assert(pathOk("/sd/tiles/12/2100/1300.png"));
  assert(pathOk("/internal/meshcore-2026.json"));
  assert(pathOk("/sd/caf\xC3\xA9 notes.txt"));   // UTF-8 and spaces are fine
  assert(!pathOk(""));
  assert(!pathOk("sd/x"));
  assert(!pathOk("/sd/"));
  assert(!pathOk("/sd//x"));
  assert(!pathOk("/sd/./x"));
  assert(!pathOk("/sd/../internal/identity"));
  assert(!pathOk("/sd/.."));
  assert(!pathOk("/sd/a\\b"));
  assert(!pathOk("/sd/a:b"));
  assert(!pathOk("/sd/a\x01" "b"));
  assert(pathOk("/sd/..hidden"));   // only exact "." and ".." components are special
  {
    char longp[kMaxPath + 2];
    memset(longp, 'a', sizeof longp);
    longp[0] = '/';
    assert(pathValid(longp, kMaxPath));
    assert(!pathValid(longp, kMaxPath + 1));
  }

  Root r;
  char rel[kMaxPath + 1];
  assert(splitPath("/", 1, &r, rel, sizeof rel) && r == ROOT_NONE && strcmp(rel, "/") == 0);
  assert(splitPath("/sd", 3, &r, rel, sizeof rel) && r == ROOT_SD && strcmp(rel, "/") == 0);
  assert(splitPath("/sd/a/b.txt", 11, &r, rel, sizeof rel) && r == ROOT_SD &&
         strcmp(rel, "/a/b.txt") == 0);
  assert(splitPath("/internal/x", 11, &r, rel, sizeof rel) && r == ROOT_INTERNAL &&
         strcmp(rel, "/x") == 0);
  assert(splitPath("/tiles/12", 9, &r, rel, sizeof rel) && r == ROOT_TILES &&
         strcmp(rel, "/12") == 0);
  assert(!splitPath("/sdx/a", 6, &r, rel, sizeof rel));
  assert(!splitPath("/SD/a", 5, &r, rel, sizeof rel));   // root names are exact
  assert(!splitPath("/nope", 5, &r, rel, sizeof rel));

  // ---- access policy ----
  // Roots themselves are read-only.
  assert(acc(ROOT_SD, "/", true) == A_READ);
  assert(acc(ROOT_INTERNAL, "/", false) == A_READ);
  // SD: everything but the firmware's data folder, including FAT's case folding
  // and 8.3 aliases.
  assert(acc(ROOT_SD, "/tiles/1/2/3.png", true) == A_WRITE);
  assert(acc(ROOT_SD, "/transfer/a.bin", true) == A_WRITE);
  assert(acc(ROOT_SD, "/meshcomod", true) == A_READ);
  assert(acc(ROOT_SD, "/meshcomod/identity/_main.id", true) == A_READ);
  assert(acc(ROOT_SD, "/MeshCoMod/contacts3", true) == A_READ);
  assert(acc(ROOT_SD, "/MESHCO~1/contacts3", true) == A_READ);
  assert(acc(ROOT_SD, "/meshcomodx", true) == A_WRITE);   // a different name
  // Internal: only the user folders and top-level backups / crash reports.
  assert(acc(ROOT_INTERNAL, "/transfer", false) == A_WRITE);
  assert(acc(ROOT_INTERNAL, "/transfer/notes.txt", false) == A_WRITE);
  assert(acc(ROOT_INTERNAL, "/screenshots/s1.png", false) == A_WRITE);
  assert(acc(ROOT_INTERNAL, "/lock/wall.jpg", false) == A_WRITE);
  assert(acc(ROOT_INTERNAL, "/meshcore-2026-09-19.json", false) == A_WRITE);
  assert(acc(ROOT_INTERNAL, "/wadamesh-crash.elf", false) == A_WRITE);
  assert(acc(ROOT_INTERNAL, "/wadamesh-crash.elf.txt", false) == A_WRITE);
  assert(acc(ROOT_INTERNAL, "/identity/_main.id", false) == A_READ);
  assert(acc(ROOT_INTERNAL, "/contacts3", false) == A_READ);
  assert(acc(ROOT_INTERNAL, "/new_prefs", false) == A_READ);
  assert(acc(ROOT_INTERNAL, "/prefs/touch.kv", false) == A_READ);
  assert(acc(ROOT_INTERNAL, "/msgs/seg_0001", false) == A_READ);
  assert(acc(ROOT_INTERNAL, "/apps/sdscan/main.lua", false) == A_READ);
  assert(acc(ROOT_INTERNAL, "/sub/meshcore-x.json", false) == A_READ);   // top level only
  assert(acc(ROOT_INTERNAL, "/meshcore-.json", false) == A_READ);        // too short to be one
  assert(acc(ROOT_INTERNAL, "/Transfer/x", false) == A_READ);            // SPIFFS is case-sensitive
  assert(acc(ROOT_INTERNAL, "/Transfer/x", true) == A_WRITE);            // FAT is not
  // Tiles: the whole partition.
  assert(acc(ROOT_TILES, "/12/2100/1300.png", false) == A_WRITE);
  assert(acc(ROOT_NONE, "/x", false) == A_NONE);

  // ---- JSON escaping ----
  {
    char out[64];
    size_t pos = jsonAppendEscaped(out, 0, sizeof out, "a\"b\\c\n\xC3\xA9");
    assert(strcmp(out, "a\\\"b\\\\c\\u000a\xC3\xA9") == 0);
    assert(pos == strlen(out));
    char tiny[4];
    assert(jsonAppendEscaped(tiny, 0, sizeof tiny, "abcdef") == sizeof tiny);
  }

  printf("usb files protocol: all tests passed\n");
  return 0;
}
