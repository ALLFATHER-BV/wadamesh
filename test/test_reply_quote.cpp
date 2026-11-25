// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for ReplyQuote.h (#641).
// Build: g++ -std=c++17 -I src -o /tmp/test_reply_quote test/test_reply_quote.cpp

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/ReplyQuote.h"

struct Msg { const char* sender; uint32_t ts; };

// Messages before the reply, newest first (index 0 = right before it).
static int pickIn(const char* name, uint32_t reply_ts, const Msg* msgs, int n) {
  return ReplyQuote::pick(name, reply_ts, n, [&](int i, const char** s, uint32_t* ts) {
    if (!msgs[i].sender) return false;
    *s = msgs[i].sender;
    *ts = msgs[i].ts;
    return true;
  });
}

int main() {
  char name[32];

  // parse: the MeshCore Open reply shape.
  const char* body = ReplyQuote::parse("@[Alice] hello", name, sizeof name);
  assert(body && strcmp(name, "Alice") == 0 && strcmp(body, "hello") == 0);
  body = ReplyQuote::parse("@[Alice]   two spaces", name, sizeof name);
  assert(body && strcmp(body, "two spaces") == 0);
  body = ReplyQuote::parse("@[Name with space] x", name, sizeof name);
  assert(body && strcmp(name, "Name with space") == 0);

  // Not replies.
  assert(!ReplyQuote::parse("hello @[Alice] hi", name, sizeof name));   // not leading
  assert(!ReplyQuote::parse("@[Alice], ack: SNR 5", name, sizeof name)); // our Ack
  assert(!ReplyQuote::parse("@[Alice]\xF0\x9F\x91\x8D", name, sizeof name)); // MeshCore One reaction
  assert(!ReplyQuote::parse("@[Alice]", name, sizeof name));            // no text
  assert(!ReplyQuote::parse("@[Alice]   ", name, sizeof name));         // only spaces
  assert(!ReplyQuote::parse("@[] hi", name, sizeof name));
  assert(!ReplyQuote::parse("@[Alice hi", name, sizeof name));
  assert(!ReplyQuote::parse(nullptr, name, sizeof name));
  assert(name[0] == '\0');   // cleared on failure

  // Name length: 31 bytes fit, 32 do not.
  assert(ReplyQuote::parse("@[1234567890123456789012345678901] t", name, sizeof name) && strlen(name) == 31);
  assert(!ReplyQuote::parse("@[12345678901234567890123456789012] t", name, sizeof name));
  char small[8];
  assert(!ReplyQuote::parse("@[Alexander] t", small, sizeof small));   // never truncated

  // pick: exactly one message from the name.
  {
    Msg m[] = { {"Bob", 1000}, {"Alice", 990}, {"Carol", 980} };
    assert(pickIn("Alice", 1010, m, 3) == 1);
    assert(pickIn("Dave", 1010, m, 3) == -1);
    assert(pickIn("alice", 1010, m, 3) == -1);   // names are exact
  }
  // Two from the name: ambiguous, no quote.
  {
    Msg m[] = { {"Alice", 1000}, {"Bob", 995}, {"Alice", 990} };
    assert(pickIn("Alice", 1010, m, 3) == -1);
  }
  // Skipped slots do not count.
  {
    Msg m[] = { {nullptr, 0}, {"Alice", 990} };
    assert(pickIn("Alice", 1000, m, 2) == 1);
  }
  // Only the look-back window is searched; a second Alice outside it does not
  // make the one inside ambiguous.
  {
    Msg m[12];
    for (int i = 0; i < 12; ++i) m[i] = {"Bob", 1000};
    m[11] = {"Alice", 1000};
    assert(pickIn("Alice", 1000, m, 12) == -1);
    m[ReplyQuote::kLookBack - 1] = {"Alice", 1000};
    assert(pickIn("Alice", 1000, m, 12) == ReplyQuote::kLookBack - 1);
  }
  // Too old: no quote. Unknown times do not block it.
  {
    Msg m[] = { {"Alice", 1000} };
    assert(pickIn("Alice", 1000 + ReplyQuote::kMaxAgeS, m, 1) == 0);
    assert(pickIn("Alice", 1000 + ReplyQuote::kMaxAgeS + 1, m, 1) == -1);
    assert(pickIn("Alice", 0, m, 1) == 0);
    Msg z[] = { {"Alice", 0} };
    assert(pickIn("Alice", 999999, z, 1) == 0);
  }

  printf("test_reply_quote: all assertions passed\n");
  return 0;
}
