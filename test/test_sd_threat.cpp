// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the SD-card malware policy (SdThreat.h), the rule wada.sd.remove()
// enforces for the ThinkNode M9 SD-card worm.
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_sd_threat.cpp -o /tmp/sdthreat && /tmp/sdthreat

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/SdThreat.h"

using namespace SdThreat;

// A minimal 128-byte file that carries a real DOS + PE header, and nothing else:
// no code, just the two signatures the check looks for.
static void makePe(uint8_t* buf, size_t n, uint32_t e_lfanew) {
  memset(buf, 0, n);
  buf[0] = 'M'; buf[1] = 'Z';
  buf[0x3C] = (uint8_t)(e_lfanew & 0xFF);
  buf[0x3D] = (uint8_t)((e_lfanew >> 8) & 0xFF);
  buf[0x3E] = (uint8_t)((e_lfanew >> 16) & 0xFF);
  buf[0x3F] = (uint8_t)((e_lfanew >> 24) & 0xFF);
  if (e_lfanew + 4 <= n) { buf[e_lfanew] = 'P'; buf[e_lfanew + 1] = 'E'; }
}

// The whole content check, the way LuaAppHost runs it against a file.
static bool renamedProgram(const uint8_t* file, uint32_t size) {
  if (!mzHeader(file, size)) return false;
  const uint32_t off = peOffset(file);
  if (!peOffsetPlausible(off, size)) return false;
  return peSignature(file + off, size - off);
}

int main() {
  // What the advisory names, in whatever case Windows wrote it.
  assert(byName("autorun.inf") == Autorun);
  assert(byName("AUTORUN.INF") == Autorun);
  assert(byName("AutoRun.Inf") == Autorun);

  // The real thing, from an infected M9 card (2026-09-18): autorun.inf in the
  // root runs xlfqf.pif on open, explore and autoplay. The Sality autorun
  // pattern: a random five-letter name with .pif, mixed-case shell verbs and
  // random-junk comment lines in between.
  assert(byName("xlfqf.pif") == Program);
  assert(byName("XLFQF.PIF") == Program);
  assert(byName("xlfqf.PiF") == Program);

  // The payload shapes of a shortcut worm.
  assert(byName("setup.exe") == Program);
  assert(byName("TF card.EXE") == Program);
  assert(byName("photo.jpg.exe") == Program);   // double extension: the last one wins
  assert(byName("screensaver.scr") == Program);
  assert(byName("payload.dll") == Program);
  assert(byName("install.vbs") == Script);
  assert(byName("run.bat") == Script);
  assert(byName("x.js") == Script);
  assert(byName("tweak.reg") == Script);
  assert(byName("maps.lnk") == Shortcut);        // a folder's name with a shortcut behind it
  assert(byName("tiles.LNK") == Shortcut);
  assert(byName("site.url") == Shortcut);
  assert(byName("desktop.scf") == Shortcut);

  // Everything a real card holds must stay untouched: Elecrow's own content,
  // wadamesh's data, the tile trees, and the "vaccine" folder some tools create.
  const char* keep[] = {
    "copyright.png", "test file.txt", "tiles", "maps", "15", "8.png", "123.jpg",
    "contacts3", "ui_threads_v1.bin", "identity", "prefs.kv", "wallpaper.jpg",
    "sdscan.lua", "sdscan.json", "hu.lang", "backup-2026-09-17.bin", "log.txt",
    "autorun", "autorun.inf.bak", "autorun.ini", "info", ".exe.txt", "exe",
    "program.exe~", "README", "", "notes.md", "track.gpx", "run.wav", "song.mp3",
  };
  for (const char* n : keep) {
    if (byName(n) != None) { printf("false positive: '%s'\n", n); return 1; }
  }
  assert(byName(nullptr) == None);
  assert(byName("trailingdot.") == None);

  // Labels exist for every threat kind and not for None.
  assert(label(None) == nullptr);
  assert(label(Autorun) && label(Program) && label(Script) && label(Shortcut) && label(RenamedProgram));

  // Content: a Windows program under an innocent name.
  uint8_t pe[128];
  makePe(pe, sizeof pe, 0x40);
  assert(renamedProgram(pe, sizeof pe));
  makePe(pe, sizeof pe, 0x78);                     // a typical real e_lfanew
  assert(renamedProgram(pe, sizeof pe));

  // "MZ" alone is not a program: 1 in 65,536 random blobs start with it.
  uint8_t mz_only[128];
  memset(mz_only, 0xA5, sizeof mz_only);
  mz_only[0] = 'M'; mz_only[1] = 'Z';
  assert(!renamedProgram(mz_only, sizeof mz_only));

  // e_lfanew pointing inside the DOS header, past the end, or far across the
  // card is rejected before anything is read there.
  makePe(pe, sizeof pe, 0x10);
  assert(!renamedProgram(pe, sizeof pe));
  makePe(pe, sizeof pe, 126);                      // no room for the 4-byte signature
  assert(!renamedProgram(pe, sizeof pe));
  assert(!peOffsetPlausible(0x40, 3));             // tiny file: size - 4 must not wrap
  assert(!peOffsetPlausible(70000, 1u << 20));     // plausible file, implausible offset
  assert(peOffsetPlausible(0x80, 1u << 20));

  // Too short to hold a DOS header at all.
  assert(!mzHeader(pe, 10));
  assert(!mzHeader(nullptr, 64));

  // Media and wadamesh data do not start with MZ.
  const uint8_t png[64] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  assert(!mzHeader(png, sizeof png));

  printf("test_sd_threat: all assertions passed\n");
  return 0;
}
