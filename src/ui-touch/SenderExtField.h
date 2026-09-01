// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <string.h>

// A stored sender name is split across two on-disk fields: the head lives in the frozen
// UiHistoryMsg::sender, and anything past it in UiSegMsg::sender_ext, which is appended at
// the very end of the segment record. The split exists so the record can hold a full-width
// name without moving any pre-existing field — see the comment on UiSegMsg::sender_ext.
//
// Nothing outside the persistence layer sees the split: UIMessage::sender is one string,
// and these two calls are the only places the halves come apart and go back together.
//
// A multi-byte character may straddle the boundary. That is harmless — the halves are
// always rejoined before the name is measured, drawn or compared — so this deliberately
// does NOT try to split on a character boundary, which would waste bytes of a field sized
// to hold the longest name exactly.
namespace SenderExtField {

// Writes `sender` into the two destination fields. `base` is always NUL-terminated; `ext`
// carries the overflow and is NOT NUL-terminated when it is full (it is a fixed-width tail,
// and its length is implied by base being full). Both are zero-filled first, so a short
// name leaves a clean record.
inline void store(const char* sender, char* base, size_t base_cap, char* ext, size_t ext_cap) {
  if (base && base_cap > 0) memset(base, 0, base_cap);
  if (ext && ext_cap > 0) memset(ext, 0, ext_cap);
  if (!sender || !base || base_cap == 0) return;

  // Walked with a pointer rather than indexed + memcpy'd: the copy length and the bounds
  // check are then the same expression, which keeps -Warray-bounds quiet on inlined
  // short literals. These are 31 bytes at most, so byte-at-a-time costs nothing.
  const char* p = sender;
  size_t head = 0;
  while (head + 1 < base_cap && *p) base[head++] = *p++;
  base[head] = '\0';
  if (head + 1 < base_cap || !ext || ext_cap == 0) return;   // fitted, or nowhere to spill

  size_t tail = 0;
  while (tail < ext_cap && *p) ext[tail++] = *p++;
}

// Rejoins the two fields into `out`. Reads are bounded by the FIELD sizes, not by strlen:
// a corrupt record may leave `base` unterminated, and it is followed on disk by the message
// body, so an unbounded read would splice the text onto the name.
inline void load(const char* base, size_t base_cap, const char* ext, size_t ext_cap,
                 char* out, size_t out_cap) {
  if (!out || out_cap == 0) return;
  out[0] = '\0';
  if (!base || base_cap == 0) return;

  const size_t head_cap = base_cap - 1;
  const char* p = base;
  size_t head = 0;
  while (head < head_cap && head + 1 < out_cap && *p) out[head++] = *p++;
  out[head] = '\0';

  // The tail only means anything when the head field is exactly full — that is the only
  // way a name could have spilled. Anything else is an old record's zero-fill or garbage
  // behind a short name, and must not be appended.
  if (head != head_cap || !ext || ext_cap == 0) return;

  const char* q = ext;
  size_t n = head;
  size_t tail = 0;
  while (tail < ext_cap && n + 1 < out_cap && *q) { out[n++] = *q++; ++tail; }
  out[n] = '\0';
}

}  // namespace SenderExtField
