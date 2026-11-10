// SPDX-License-Identifier: GPL-3.0-or-later
// MeshCore Open TAK / Wadamesh TAK texts (ui-touch/TakTexts.h). The table is the one
// in jade-on-mesh's hiding-tak-control-texts.md.
// Build: g++ -std=c++17 -Wall -Wextra -I src -o /tmp/test_tak_texts test/test_tak_texts.cpp
#include <stdio.h>
#include <string.h>

#include "ui-touch/TakTexts.h"

using namespace taktext;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b)) != 0) { \
  printf("FAIL %s:%d %s = \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a, (a), (b)); ++failures; } } while (0)

int main() {
  const bool DM = true, CH = false;

  CHECK(classify("VR3:1ps9wxb:a:a1b2c3d4e5f6", DM) == kControl);
  CHECK(classify("VT1:0a1b2c3d:12345:3:17/17", CH) == kControl);
  CHECK(classify("z3s1a", CH) == kControl);
  CHECK(classify("z3PA2:5", DM) == kControl);
  CHECK(classify("z4Cz", DM) == kControl);
  CHECK(classify("#TEL:AbCdEfGhIjKlMnO", CH) == kControl);

  CHECK(classify("VI1:79e6a5:14:a:46 IMAGE Look at this", CH) == kPhoto);
  CHECK(classify("VI1:79e6a5:14:a:46", DM) == kPhoto);
  CHECK(classify("VE3:1ps9wxb:1:4:3 PTT", CH) == kTalk);
  CHECK(classify("VE3:1ps9wxb:1:4:3", DM) == kVoice);
  CHECK(classify("VM1:2h7k1:w:6:5 VOICE", DM) == kVoice);
  CHECK(classify("z21k|9f3ab2c4", CH) == kEncrypted);
  CHECK(classify("4f2a9c0b1d3e5f7a8b9c0d1e", DM) == kEncrypted);

  CHECK(classify("4f2a9c0b1d3e5f7a8b9c0d1e", CH) == kNone);
  CHECK(classify("cafe", DM) == kNone);
  for (bool as : {DM, CH}) {
    CHECK(classify("VE3: hello", as) == kNone);
    CHECK(classify("VI1 is cool", as) == kNone);
  }
  CHECK(classify("z4", DM) == kNone);
  CHECK(classify("#TEL:x", DM) == kNone);
  CHECK(classify("hello", CH) == kNone);
  CHECK(classify("#test ok", CH) == kNone);
  CHECK(classify(nullptr, CH) == kNone);

  CHECK_STR(caption("VI1:79e6a5:14:a:46 IMAGE Look at this"), "Look at this");
  CHECK_STR(caption("VI1:79e6a5:14:a:46 IMAGE"), "");
  CHECK_STR(caption("VI1:79e6a5:14:a:46"), "");
  CHECK_STR(caption("VE3:1ps9wxb:1:4:3 PTT"), "");
  CHECK_STR(caption("VM1:2h7k1:w:6:5 VOICE on my way"), "on my way");
  CHECK_STR(caption("VI1:79e6a5:14:a:46 IMAGES of the hill"), "IMAGES of the hill");

  printf("%s\n", failures ? "FAILED" : "ok");
  return failures ? 1 : 0;
}
