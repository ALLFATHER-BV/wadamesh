// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reply quotes in chat bubbles (#641).
//
// MeshCore messages carry no reply-to field. MeshCore Open (and the clients that
// copy it) send a reply as a leading mention, "@[Name] text", exactly the way a
// plain mention is written, and so do our own Mention and Ack actions. A quote
// is therefore a guess, and the rules below keep it to the cases where it cannot
// be wrong in practice:
//   - the mention leads the message and is followed by a space and some text
//     (MeshCore Open's parseReplyMention: ^@\[([^\]]+)\]\s+(.+)$), so an Ack
//     ("@[Name], ack: ...") or a MeshCore One reaction ("@[Name]<emoji>") is
//     never a reply;
//   - Name has exactly one message among the few messages before the reply;
//   - that message is recent.
// The message text is never changed: the @[Name] tag, its mention colour and what
// every other client shows for the same bytes all stay as they are. The quote is
// only drawn above it.
//
// Header-only and free of Arduino/LVGL so the host test can include it.
// Build the test: g++ -std=c++17 -I src -o /tmp/test_reply_quote test/test_reply_quote.cpp

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ReplyQuote {

constexpr size_t   kMaxName   = 31;        // UITask::MAX_SENDER_NAME
constexpr int      kLookBack  = 10;        // messages before the reply that are searched
constexpr uint32_t kMaxAgeS   = 30 * 60;   // and how much older the quoted one may be

// A leading "@[Name] text": copies Name to name_out (always NUL-terminated) and
// returns the text after the spaces, or nullptr when `text` is not a reply.
inline const char* parse(const char* text, char* name_out, size_t name_cap) {
  if (name_out && name_cap) name_out[0] = '\0';
  if (!text || text[0] != '@' || text[1] != '[' || !name_out || name_cap == 0) return nullptr;
  const char* close = strchr(text + 2, ']');
  if (!close) return nullptr;
  const size_t nlen = static_cast<size_t>(close - (text + 2));
  if (nlen == 0 || nlen > kMaxName || nlen >= name_cap) return nullptr;
  const char* body = close + 1;
  if (*body != ' ' && *body != '\t' && *body != '\n') return nullptr;
  while (*body == ' ' || *body == '\t' || *body == '\n' || *body == '\r') body++;
  if (!*body) return nullptr;
  memcpy(name_out, text + 2, nlen);
  name_out[nlen] = '\0';
  return body;
}

// Picks the quoted message. `get(i, &sender, &ts)` describes the i-th message
// before the reply (0 = the one right before it) and returns false for a slot to
// skip; `count` is how many there are. Returns the index of the only message
// from `name` among the first kLookBack, or -1 when there is none, more than
// one, or it is more than kMaxAgeS away from the reply (when both times are known).
template <class Get>
inline int pick(const char* name, uint32_t reply_ts, int count, Get get) {
  if (!name || !name[0]) return -1;
  if (count > kLookBack) count = kLookBack;
  int found = -1;
  uint32_t found_ts = 0;
  for (int i = 0; i < count; ++i) {
    const char* sender = nullptr;
    uint32_t ts = 0;
    if (!get(i, &sender, &ts) || !sender || strcmp(sender, name) != 0) continue;
    if (found >= 0) return -1;   // two candidates: a guess either way
    found = i;
    found_ts = ts;
  }
  if (found < 0) return -1;
  if (reply_ts && found_ts) {
    const uint32_t gap = reply_ts > found_ts ? reply_ts - found_ts : found_ts - reply_ts;
    if (gap > kMaxAgeS) return -1;
  }
  return found;
}

}  // namespace ReplyQuote
