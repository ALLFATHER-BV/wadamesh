// SPDX-License-Identifier: GPL-3.0-or-later
// Emoji reactions from MeshCore Open / MeshCore One (ui-touch/ReactionCodec.h, #791).
// Build: g++ -std=c++17 -Wall -Wextra -I src -o /tmp/test_reaction_codec test/test_reaction_codec.cpp
//
// The hash vectors are the known answers from Yeraze/meshmonitor's codec tests
// (src/utils/meshcoreReactions.test.ts, PR #5771), which were checked against
// MeshCore Open's Dart implementation.
#include <stdio.h>
#include <string.h>

#include "ui-touch/ReactionCodec.h"

using namespace ReactionCodec;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b)) != 0) { \
  printf("FAIL %s:%d %s = \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a, (a), (b)); ++failures; } } while (0)

static const char* HEART = "❤️";
static const char* H8 = "a1b2c3d4";

static void hex4(uint16_t h, char out[5]) { snprintf(out, 5, "%04x", (unsigned)h); }

static void testDartHash() {
  struct { const char* s; uint32_t want; } v[] = {
    { "", 1 }, { "a", 170824770 }, { "1700000000Bobhello", 542107513 },
    { "1700000000hi", 963884477 }, { "Bob", 124362681 }, { "hello", 150804507 },
    { "Alice", 210078619 }, { "Hello world", 780881625 },
  };
  for (auto& t : v) {
    const uint32_t got = dartStringHash(t.s);
    if (got != t.want) { printf("FAIL dartStringHash(\"%s\") = %u, want %u\n", t.s, got, t.want); ++failures; }
  }
}

static void testMcoHash() {
  struct { uint32_t ts; const char* sender; const char* text; const char* want; } v[] = {
    { 1700000000, "Bob", "hello", "e779" },
    { 1700000000, nullptr, "hi", "b5bd" },
    { 1700000000, "Привет\U0001F600", "x", "7fee" },
    { 1234567890, "Alice", "Hello world", "a51e" },
    { 1704067200, "AlphaNode", "Hello", "6df6" },
    { 1704067200, nullptr, "Hello", "92c7" },
    { 0, "A", "", "4bd9" },
    { 4294967295u, "A", "", "fd84" },
    // The 5-unit cut lands inside the surrogate pair: only its high half is hashed.
    { 1704067200, "A", "abcd\U0001F600", "917f" },
    { 1704067200, "A", "abcdefgh", "39f3" },
    { 1704067200, "A", "abcde", "39f3" },
    { 1704067200, "a]b:c", "hey", "f04f" },
    { 1704067200, "", "hey", "0be1" },
  };
  for (auto& t : v) {
    char got[5];
    hex4(mcoHash(t.ts, t.sender, t.text), got);
    if (strcmp(got, t.want) != 0) {
      printf("FAIL mcoHash(%u, %s, \"%s\") = %s, want %s\n", t.ts, t.sender ? t.sender : "null",
             t.text, got, t.want);
      ++failures;
    }
  }
}

static void testMc1Hash() {
  struct { const char* text; uint32_t ts; const char* want; } v[] = {
    { "Hello", 1704067200, "b45pc4ek" },
    { "Hello world", 1704067200, "7a6r3xy1" },
    { "Hello", 1704067201, "3wh8x8z5" },
    { "World", 1704067200, "vp1vxnf4" },
    { "Message 0", 0, "7mt5pe1m" },
    { "café \U0001F44D ok", 1760000000, "8b5qzwf6" },
    { "", 4294967295u, "npah66y0" },
    { "Hello world!", 1234567890, "tyvbkb33" },
    { "", 0, "vwzp3604" },
    { "Hello", 4294967295u, "vs4bnq2w" },
    { "line1\r\nline2", 1704067200, "ynzeazsh" },
    { "\U0001F44D", 1704067200, "z5kxjbn0" },
    { "a�b", 1, "29htgvj2" },
  };
  for (auto& t : v) {
    char got[9];
    mc1Hash(t.ts, t.text, got);
    if (strcmp(got, t.want) != 0) {
      printf("FAIL mc1Hash(\"%s\", %u) = %s, want %s\n", t.text, t.ts, got, t.want);
      ++failures;
    }
  }
}

static void testSha256() {
  // FIPS 180-2 "abc" and the two-block message.
  const char* in[] = { "abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" };
  const char* want[] = {
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
  };
  for (int i = 0; i < 2; ++i) {
    Sha256 s; s.update(in[i], strlen(in[i]));
    uint8_t d[32]; s.final(d);
    char hex[65];
    for (int k = 0; k < 32; ++k) snprintf(hex + 2 * k, 3, "%02x", d[k]);
    CHECK_STR(hex, want[i]);
  }
  // The emoji table, '\n'-joined, matches meshmonitor's vendored copy byte for byte.
  Sha256 s;
  for (int i = 0; i < kMcoEmojiCount; ++i) {
    if (i) s.update("\n", 1);
    s.update(kMcoEmoji[i], strlen(kMcoEmoji[i]));
  }
  uint8_t d[32]; s.final(d);
  char hex[65];
  for (int k = 0; k < 32; ++k) snprintf(hex + 2 * k, 3, "%02x", d[k]);
  CHECK_STR(hex, "7150024d68867be0191c8e1cec6fea1ba76c8bf656fd5327fe16658d04c134ee");
}

static void testTable() {
  CHECK(kMcoEmojiCount == 184);
  CHECK_STR(kMcoEmoji[0x00], "\U0001F44D");
  CHECK_STR(kMcoEmoji[0x01], HEART);
  CHECK_STR(kMcoEmoji[0x02], "\U0001F602");
  CHECK_STR(kMcoEmoji[0x05], "\U0001F525");
  CHECK_STR(kMcoEmoji[0x66], "\U0001F4AA");
  CHECK_STR(kMcoEmoji[0x83], "\U0001F441️‍\U0001F5E8️");
  CHECK_STR(kMcoEmoji[0xb1], "⌚");
  CHECK_STR(kMcoEmoji[0xb7], "\U0001F680");
}

static void testParseMcoV3() {
  Reaction r;
  CHECK(parse("r:6801:02", true, &r));   // the report in #791
  CHECK(r.kind == MCO_V3); CHECK_STR(r.hash, "6801"); CHECK_STR(r.emoji, "\U0001F602");
  CHECK(parse("r:e779:01", false, &r) && r.kind == MCO_V3); CHECK_STR(r.emoji, HEART);
  CHECK(parse("r:a1b2:0b", true, &r)); CHECK_STR(r.emoji, "\U0001F602");
  CHECK(parse("r:a1b2:46", true, &r)); CHECK_STR(r.emoji, "\U0001F44D");
  CHECK(parse("r:0000:b7", true, &r)); CHECK_STR(r.emoji, "\U0001F680");
  const char* refuse[] = {
    "r:ffff:b8", "r:ffff:ff", "r:A1B2:00", "r:a1b2:0B", "r:zzzz:00", "r:a1b2:0", "r:a1b2:000",
    "r:a1b2:00\n", " r:a1b2:00", "R:a1b2:00", "r:12:34:5", "r:hello:x", "r: see me",
    "r:1700000000000_12345_67890:ok", "r:1700000000000_12345_67890:\U0001F44D\U0001F44D",
    "r:1700000000000:\U0001F44D", "r:abc_def_ghi:\U0001F44D",
  };
  for (const char* t : refuse) {
    if (parse(t, true, &r) || parse(t, false, &r)) { printf("FAIL parsed \"%s\"\n", t); ++failures; }
    CHECK(r.kind == NONE);
  }
}

static void testParseMcoV1() {
  Reaction r;
  CHECK(parse("r:1700000000000_12345_67890:\U0001F44D", true, &r) && r.kind == MCO_V1);
  CHECK_STR(r.emoji, "\U0001F44D");
  CHECK(parse("r:1700000000000_12345_67890:⌚", true, &r) && r.kind == MCO_V1);
  CHECK(parse("r:1700000000000_a1b2c3d4:❤️", false, &r) && r.kind == MCO_V1);
  CHECK(parse("r:1700000000000_12345678:\U0001F44D", false, &r) && r.kind == MCO_V1);
}

static void testParseMc1Channel() {
  Reaction r;
  CHECK(parse("@[AlphaNode]\U0001F44D\n7f3a9c12", true, &r) && r.kind == MC1);
  CHECK_STR(r.emoji, "\U0001F44D"); CHECK_STR(r.target_sender, "AlphaNode"); CHECK_STR(r.hash, "7f3a9c12");
  CHECK(parse("@[Node]\U0001F44D\nABCDEF12", true, &r)); CHECK_STR(r.hash, "abcdef12");
  CHECK(parse("@[Node]\U0001F44D\nOOOOOOOO", true, &r)); CHECK_STR(r.hash, "00000000");
  CHECK(parse("@[Node]\U0001F44D\niiiiiiii", true, &r)); CHECK_STR(r.hash, "11111111");
  CHECK(parse("@[Alice]\U0001F44D\nreceived", true, &r)); CHECK_STR(r.hash, "rece1ved");
  // Legacy order.
  CHECK(parse("\U0001F44D@[AlphaNode]\n7f3a9c12", true, &r) && r.kind == MC1);
  CHECK_STR(r.target_sender, "AlphaNode");
  CHECK(parse("\U0001F44D@[A]@[B]\na1b2c3d4", true, &r)); CHECK_STR(r.target_sender, "A]@[B");
  // Sender kept verbatim.
  CHECK(parse("@[Node:Alpha]\U0001F44D\na1b2c3d4", true, &r)); CHECK_STR(r.target_sender, "Node:Alpha");
  CHECK(parse("@[ Node ]\U0001F44D\na1b2c3d4", true, &r)); CHECK_STR(r.target_sender, " Node ");
  // Whole emoji: skin tone, ZWJ family, flag, keycap, bare heart, VS16.
  const char* emojis[] = {
    "\U0001F44D\U0001F3FD", "\U0001F468‍\U0001F469‍\U0001F467", "\U0001F1FA\U0001F1F8",
    "1️⃣", "❤", "\U0001F44D️",
  };
  for (const char* e : emojis) {
    char t[64]; snprintf(t, sizeof t, "@[Node]%s\n%s", e, H8);
    if (!parse(t, true, &r)) { printf("FAIL channel emoji not parsed: %s\n", e); ++failures; continue; }
    CHECK_STR(r.emoji, e);
  }
  const char* refuse[] = {
    "Just a normal message", "@[Node]\U0001F44D", "\U0001F44D [Node]\na1b2c3d4", "\U0001F44D@Node\na1b2c3d4",
    "@[Node]\U0001F44D\nabc", "@[Node]\U0001F44D\nuuuuuuuu", "@[Node]\U0001F44D\na1b2-3d4",
    "@[]\U0001F44D\na1b2c3d4", "\U0001F44D@[]\na1b2c3d4", "@[Node]\na1b2c3d4",
    "@[Alice]\U0001F389 on my way\npassword", "\U0001F389 on my way@[Alice]\npassword",
    "A@[Node]\na1b2c3d4", "@[Node]1\na1b2c3d4", "@[A]\U0001F44D@[B]\na1b2c3d4",
    "@[Node]\U0001F44D\r\na1b2c3d4", "@[Node]\U0001F44D\na1b2c3d4\r", "@[Node]\U0001F44D\na1b2c3d4\n",
    "@[Node]\U0001F44D\n\na1b2c3d4", " @[Node]\U0001F44D\na1b2c3d4", "@[Node] \U0001F44D\na1b2c3d4",
    "\U0001F44D\n7f3a9c12",   // the DM form is not a channel reaction
    "", "\n",
  };
  for (const char* t : refuse) {
    if (parse(t, true, &r)) { printf("FAIL channel parsed \"%s\"\n", t); ++failures; }
  }
}

static void testParseMc1Dm() {
  Reaction r;
  CHECK(parse("\U0001F44D\n7f3a9c12", false, &r) && r.kind == MC1);
  CHECK_STR(r.emoji, "\U0001F44D"); CHECK_STR(r.hash, "7f3a9c12"); CHECK_STR(r.target_sender, "");
  CHECK(parse("❤️\ne4d8b1a0", false, &r)); CHECK_STR(r.emoji, HEART);
  CHECK(parse("\U0001F44D\U0001F3FD\na1b2c3d4", false, &r));
  const char* refuse[] = {
    "\U0001F44D", "@[Node]\U0001F44D\nabcd1234", "\U0001F44D@[\na1b2c3d4", "\U0001F44D\nuuuuuuuu",
    "A\na1b2c3d4", "1\na1b2c3d4", "ok\nreceived", "\U0001F389 on my way\npassword",
    "\U0001F44D\r\na1b2c3d4", "\U0001F44D\n\na1b2c3d4", "\na1b2c3d4", "",
  };
  for (const char* t : refuse) {
    if (parse(t, false, &r)) { printf("FAIL dm parsed \"%s\"\n", t); ++failures; }
  }
}

static void testMatches() {
  Reaction r;
  // MeshCore Open, channel: e779 = (1700000000, "Bob", "hello").
  CHECK(parse("r:e779:02", true, &r));
  CHECK(matches(r, 1700000000, "Bob", "hello"));
  CHECK(matches(r, 1700000000, "Bob", "hello there"));   // only the first 5 units count
  CHECK(!matches(r, 1700000001, "Bob", "hello"));
  CHECK(!matches(r, 1700000000, "Alice", "hello"));
  CHECK(!matches(r, 1700000000, nullptr, "hello"));
  // MeshCore Open, DM: b5bd = (1700000000, null, "hi").
  CHECK(parse("r:b5bd:00", false, &r));
  CHECK(matches(r, 1700000000, nullptr, "hi"));
  CHECK(matches(r, 1700000000, "", "hi"));   // an empty name adds nothing to the hash input
  CHECK(!matches(r, 1700000000, "Bob", "hi"));
  // MeshCore One, channel: b45pc4ek = ("Hello", 1704067200), sender named in the reaction.
  CHECK(parse("@[AlphaNode]\U0001F44D\nb45pc4ek", true, &r));
  CHECK(matches(r, 1704067200, "AlphaNode", "Hello"));
  CHECK(!matches(r, 1704067200, "BetaNode", "Hello"));
  CHECK(!matches(r, 1704067201, "AlphaNode", "Hello"));
  // MeshCore One, DM.
  CHECK(parse("\U0001F44D\nb45pc4ek", false, &r));
  CHECK(matches(r, 1704067200, nullptr, "Hello"));
  // Folded aliases still match: 'B45PC4EK' and an O for the 0... (none here), case only.
  CHECK(parse("\U0001F44D\nB45PC4EK", false, &r));
  CHECK(matches(r, 1704067200, nullptr, "Hello"));
  // v1 is shown, never matched.
  CHECK(parse("r:1700000000000_12345_67890:\U0001F44D", true, &r));
  CHECK(!matches(r, 1700000000, "Bob", "hello"));
}

static void testUtf8Prefix() {
  CHECK(utf8Prefix("hello", 10) == 5);
  CHECK(utf8Prefix("hello", 3) == 3);
  // "caf" + 2-byte é: a cut at 4 would split it.
  CHECK(utf8Prefix("café!", 4) == 3);
  CHECK(utf8Prefix("café!", 5) == 5);
  CHECK(utf8Prefix("\U0001F44D\U0001F44D", 6) == 4);
}

int main() {
  testDartHash();
  testMcoHash();
  testMc1Hash();
  testSha256();
  testTable();
  testParseMcoV3();
  testParseMcoV1();
  testParseMc1Channel();
  testParseMc1Dm();
  testMatches();
  testUtf8Prefix();
  if (failures) { printf("%d failure(s)\n", failures); return 1; }
  printf("test_reaction_codec: all passed\n");
  return 0;
}
