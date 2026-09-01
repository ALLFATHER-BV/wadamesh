// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <string.h>

#include "ui-touch/ChannelSenderSplit.h"

// UIMessage::sender is char[MAX_SENDER_NAME + 1] with MAX_SENDER_NAME == 31 — the full
// MeshCore name width (ContactInfo::name[32]), so every real name now fits and the
// truncation path is unreachable from the firmware's own callers. It is still exercised
// below against narrower destinations: the helper is documented to truncate to whatever
// it is handed, and the beta that shipped a 24-char field relies on exactly that.
static const size_t kSenderCap = 32;

// The frozen on-disk width (k_ui_disk_sender_len) — and what the previous beta's
// UIMessage::sender was. The truncation + UTF-8 back-off cases run against this.
static const size_t kNarrowCap = 25;

int main() {
  char sender[kSenderCap];

  // Ordinary channel post: author out, body in.
  const char* body = ChannelSenderSplit::split("nick: hello there", sender, sizeof sender);
  assert(strcmp(sender, "nick") == 0);
  assert(strcmp(body, "hello there") == 0);

  // The regression this all started from: a 27-char name. It used to fail the split
  // outright, so the bubble header showed the channel and the body kept reading
  // "nick: message". At the full width it is stored whole, with no marker.
  const char* long_name = "Ouderkerk Repeater Node Two: on air";
  body = ChannelSenderSplit::split(long_name, sender, sizeof sender);
  assert(strcmp(sender, "Ouderkerk Repeater Node Two") == 0);
  assert(strlen(sender) == 27);
  assert(strcmp(body, "on air") == 0);

  // The longest name the wire can carry: 31 chars, stored exactly, no marker.
  const char* longest = "1234567890123456789012345678901: body";
  body = ChannelSenderSplit::split(longest, sender, sizeof sender);
  assert(strlen(sender) == 31);
  assert(strcmp(sender, "1234567890123456789012345678901") == 0);
  assert(strcmp(body, "body") == 0);

  // One char PAST the wire width is not a name at all — no node can be called this — so
  // the ": " belongs to the message. Leave the text whole rather than inventing an author
  // and hiding the start of the body.
  const char* not_a_name = "12345678901234567890123456789012: body";
  assert(strlen(not_a_name) == 38);
  body = ChannelSenderSplit::split(not_a_name, sender, sizeof sender);
  assert(sender[0] == '\0');
  assert(body == not_a_name);

  // The realistic shape of that: an unprefixed post whose body just happens to contain
  // ": ". It must survive intact, whole, with the caller's own sender label.
  const char* prose = "the repeater at Ouderkerk is back up: full quieting again";
  body = ChannelSenderSplit::split(prose, sender, sizeof sender);
  assert(sender[0] == '\0');
  assert(body == prose);

  // ---- Narrower destination: truncate the label, never reject the split -------------
  char narrow[kNarrowCap];

  // The 27-char name again. Too long for this field, so it is cut and marked with "..."
  // — a shortened name must not read as a different node.
  body = ChannelSenderSplit::split(long_name, narrow, sizeof narrow);
  assert(strlen(narrow) == 24);
  assert(strcmp(narrow, "Ouderkerk Repeater No...") == 0);
  assert(strcmp(body, "on air") == 0);

  // The widest name the wire can carry still splits here — cut, but the author is out
  // of the body, which is the whole point.
  body = ChannelSenderSplit::split(longest, narrow, sizeof narrow);
  assert(strcmp(narrow, "123456789012345678901...") == 0);
  assert(strcmp(body, "body") == 0);

  // One char over the cap still gets the marker (name budget 21, not 24).
  const char* one_over = "1234567890123456789012345: body";
  body = ChannelSenderSplit::split(one_over, narrow, sizeof narrow);
  assert(strcmp(narrow, "123456789012345678901...") == 0);
  assert(strcmp(body, "body") == 0);

  // Exactly at the cap: 24 chars, no truncation, no marker.
  const char* exact = "123456789012345678901234: body";
  body = ChannelSenderSplit::split(exact, narrow, sizeof narrow);
  assert(strcmp(narrow, "123456789012345678901234") == 0);
  assert(strcmp(body, "body") == 0);

  // Truncation lands on a character boundary, never inside a multi-byte sequence.
  // "Ouderkerkk" (10) + U+2600 (3 bytes) x 5 = 25 bytes; the 21-byte name budget
  // (24 minus the marker) falls inside the fourth sun, so it backs off to 19.
  const char* utf8_name = "Ouderkerkk\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80: hi";
  body = ChannelSenderSplit::split(utf8_name, narrow, sizeof narrow);
  assert(strcmp(narrow, "Ouderkerkk\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80...") == 0);
  assert(strlen(narrow) == 22);
  assert(strcmp(body, "hi") == 0);

  // Same shape one char shorter, so the budget already sits on a boundary: the back-off
  // must not eat a whole character that fits.
  const char* utf8_fits = "Ouderkerk\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80: hi";
  body = ChannelSenderSplit::split(utf8_fits, narrow, sizeof narrow);
  assert(strcmp(narrow, "Ouderkerk\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80\xE2\x98\x80...") == 0);
  assert(strlen(narrow) == 24);
  assert(strcmp(body, "hi") == 0);

  // Destination too small to hold a marker: truncate plainly rather than spending the
  // whole label on dots.
  char tiny[5];
  body = ChannelSenderSplit::split("nickname: body", tiny, sizeof tiny);
  assert(strcmp(tiny, "nick") == 0);
  assert(strcmp(body, "body") == 0);

  // ---- Nothing to split --------------------------------------------------------------

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
