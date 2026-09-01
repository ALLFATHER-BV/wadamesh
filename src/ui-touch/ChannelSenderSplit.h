// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// A channel/group post arrives on the wire as "SenderName: body" — the author is
// part of the text, not a separate field (unlike a DM, where the sender rides
// along as from_name). Split it so the UI can label the bubble with the author
// and show only the body underneath.
namespace ChannelSenderSplit {

// The widest author a channel post can legitimately name. MeshCore keeps every
// name in a char[32] (ContactInfo::name, ChannelDetails::name,
// NodePrefs::node_name), so 31 characters is the whole wire width.
constexpr size_t kMaxWireName = 31;

// Copies the embedded author into `sender_out` (always NUL-terminated, empty when
// there is nothing to split) and returns the start of the body — `text` itself
// when the string carries no author prefix.
//
// A name longer than the destination is TRUNCATED, not rejected. MeshCore names
// run to 31 chars while UIMessage::sender holds 24, and refusing the split for
// the overflowing ones is what put the channel name in the bubble header and left
// "nick: message" in the body.
//
// A prefix wider than kMaxWireName is a different thing entirely and IS rejected:
// no name can be that long, so the ": " belongs to an unprefixed message ("the
// repeater at Ouderkerk: it works"). Splitting there would invent an author and
// hide the start of the body. That plausibility check is what the old
// `slen <= MAX_SENDER_NAME` test provided; it has to be the WIRE width and not
// the destination width, or a long real name gets rejected all over again.
//
// A truncated name ends in "..." (ASCII, like chatFitLeadingEllipsis — the baked
// fonts carry no U+2026), so a cut name reads as cut rather than as a different
// node with a shorter name. The marker is part of the stored sender, so the
// bubble label, the chat-list preview and block-by-name all agree on one string.
inline const char* split(const char* text, char* sender_out, size_t sender_cap) {
  if (sender_out && sender_cap > 0) sender_out[0] = '\0';
  if (!text) return "";
  if (!sender_out || sender_cap < 2) return text;

  const char* colon = strstr(text, ": ");
  if (!colon || colon == text) return text;
  if (static_cast<size_t>(colon - text) > kMaxWireName) return text;   // not a name — see above

  static const char kEllipsis[] = "...";
  const size_t kEllipsisLen = sizeof(kEllipsis) - 1;

  const size_t cap = sender_cap - 1;   // bytes available, NUL excluded
  size_t slen = static_cast<size_t>(colon - text);
  const bool truncated = slen > cap;
  // Only mark when the name still gets at least as many bytes as the marker —
  // on a destination too small for that, "nick" says more than "n...".
  const bool mark = truncated && cap >= 2 * kEllipsisLen;
  if (truncated) {
    slen = mark ? cap - kEllipsisLen : cap;
    // Never cut a multi-byte character in half: step back to the start of the
    // sequence the cap lands inside, so the label gets valid UTF-8 rather than a
    // stray '*' from the missing-glyph sanitiser.
    while (slen > 0 && (static_cast<uint8_t>(text[slen]) & 0xC0U) == 0x80U) --slen;
    if (slen == 0) return text;   // one long sequence and nothing fits — leave it whole
  }

  memcpy(sender_out, text, slen);
  if (mark) {
    memcpy(sender_out + slen, kEllipsis, kEllipsisLen);
    slen += kEllipsisLen;
  }
  sender_out[slen] = '\0';
  return colon + 2;
}

}  // namespace ChannelSenderSplit
