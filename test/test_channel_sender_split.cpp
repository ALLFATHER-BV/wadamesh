// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <string.h>

#include "ui-touch/ChannelSenderSplit.h"

// UIMessage::sender is char[MAX_SENDER_NAME + 1] with MAX_SENDER_NAME == 24.
static const size_t kSenderCap = 25;

int main() {
  char sender[kSenderCap];

  // Ordinary channel post: author out, body in.
  const char* body = ChannelSenderSplit::split("nick: hello there", sender, sizeof sender);
  assert(strcmp(sender, "nick") == 0);
  assert(strcmp(body, "hello there") == 0);

  // The regression: a 27-char MeshCore name (they run to 31) used to fail the
  // split outright, so the bubble header showed the channel and the body kept
  // reading "nick: message". It must split, truncating the label instead — and a
  // truncated label ends in "..." so it does not read as a shorter name.
  const char* long_name = "Ouderkerk Repeater Node Two: on air";
  body = ChannelSenderSplit::split(long_name, sender, sizeof sender);
  assert(strlen(sender) == 24);
  assert(strcmp(sender, "Ouderkerk Repeater No...") == 0);
  assert(strcmp(body, "on air") == 0);

  // Truncation lands on a character boundary, never inside a multi-byte sequence.
  // "Ouderkerkk" (10) + U+2600 (3 bytes) x 5 = 25 bytes; the 21-byte name budget
  // (24 minus the marker) falls inside the fourth sun, so it backs off to 19.
  const char* utf8_name = "Ouderkerkk\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80: hi";
  body = ChannelSenderSplit::split(utf8_name, sender, sizeof sender);
  assert(strcmp(sender, "Ouderkerkk\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80...") == 0);
  assert(strlen(sender) == 22);
  assert(strcmp(body, "hi") == 0);

  // Same shape one char shorter, so the budget already sits on a boundary: the
  // back-off must not eat a whole character that fits.
  const char* utf8_fits = "Ouderkerk\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80: hi";
  body = ChannelSenderSplit::split(utf8_fits, sender, sizeof sender);
  assert(strcmp(sender, "Ouderkerk\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80...") == 0);
  assert(strlen(sender) == 24);
  assert(strcmp(body, "hi") == 0);

  // One char over the cap still gets the marker (name budget 21, not 24).
  const char* one_over = "1234567890123456789012345: body";
  body = ChannelSenderSplit::split(one_over, sender, sizeof sender);
  assert(strcmp(sender, "123456789012345678901...") == 0);
  assert(strcmp(body, "body") == 0);

  // Destination too small to hold a marker: truncate plainly rather than
  // spending the whole label on dots.
  char tiny[5];
  body = ChannelSenderSplit::split("nickname: body", tiny, sizeof tiny);
  assert(strcmp(tiny, "nick") == 0);
  assert(strcmp(body, "body") == 0);

  // Exactly at the cap: 24 chars, no truncation.
  const char* exact = "123456789012345678901234: body";
  body = ChannelSenderSplit::split(exact, sender, sizeof sender);
  assert(strcmp(sender, "123456789012345678901234") == 0);
  assert(strcmp(body, "body") == 0);

  // No author prefix — text passes through untouched, sender stays empty so the
  // caller falls back to from_name.
  const char* plain = "just a message";
  body = ChannelSenderSplit::split(plain, sender, sizeof sender);
  assert(sender[0] == '\0');
  assert(body == plain);

  // A bare colon without the space is not a separator.
  const char* no_space = "nick:hello";
  body = ChannelSenderSplit::split(no_space, sender, sizeof sender);
  assert(sender[0] == '\0');
  assert(body == no_space);

  // Leading separator: empty author is not a name.
  const char* leading = ": body";
  body = ChannelSenderSplit::split(leading, sender, sizeof sender);
  assert(sender[0] == '\0');
  assert(body == leading);

  // First separator wins — a colon in the body does not re-split.
  body = ChannelSenderSplit::split("nick: ratio 3: 1", sender, sizeof sender);
  assert(strcmp(sender, "nick") == 0);
  assert(strcmp(body, "ratio 3: 1") == 0);

  // Degenerate inputs.
  assert(strcmp(ChannelSenderSplit::split(nullptr, sender, sizeof sender), "") == 0);
  assert(sender[0] == '\0');
  const char* keep = "nick: hi";
  assert(ChannelSenderSplit::split(keep, nullptr, 0) == keep);
  return 0;
}
