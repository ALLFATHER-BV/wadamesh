// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reply-quote ("@[sender] body") detection and formatting.
//
// A message beginning with "@[sender]" is a reply: the sender names the
// contact whose most recent message in this thread is being answered. The
// prefix is stripped for display and the original message is shown as a
// single-line mini bubble at the top of the reply bubble (see UITask.cpp's
// chatVirtCreateBubble).
//
// This file is self-contained text handling — message lookup stays in
// UITask.cpp, which has the thread's ring buffer and sender list.

#include <stddef.h>
#include <string.h>
#include <lvgl.h>

namespace ReplyQuote {

constexpr size_t kMaxSender = 31;   // UITask::MAX_SENDER_NAME
constexpr size_t kMaxText   = 160;  // UITask::MAX_MSG_TEXT

// ---------------------------------------------------------------------------
// Prefix parsing
// ---------------------------------------------------------------------------

// Does `text` begin with the "@[name]" reply prefix?
// Returns true only when a valid closing bracket follows a non-empty name.
inline bool hasPrefix(const char* text) {
  if (!text || text[0] != '@' || text[1] != '[') return false;
  const char* close = strchr(text + 2, ']');
  if (!close) return false;
  // Non-empty name and within wire bounds.
  return (close > text + 2) && (static_cast<size_t>(close - (text + 2)) <= kMaxSender);
}

// Extract the sender name and body start from a leading "@[name]" prefix.
// Returns nullptr if `text` does not carry a valid prefix; otherwise fills
// `sender_out` (always NUL-terminated) and returns a pointer to the first
// non-space character after the closing bracket.
inline const char* parse(const char* text, char* sender_out, size_t sender_cap) {
  if (!text || text[0] != '@' || text[1] != '[') return nullptr;
  if (!sender_out || sender_cap == 0) return nullptr;

  sender_out[0] = '\0';
  const char* close = strchr(text + 2, ']');
  if (!close) return nullptr;

  size_t nlen = static_cast<size_t>(close - (text + 2));
  if (nlen == 0 || nlen > kMaxSender) return nullptr;

  size_t copy = (nlen < sender_cap - 1) ? nlen : sender_cap - 1;
  memcpy(sender_out, text + 2, copy);
  sender_out[copy] = '\0';

  const char* body = close + 1;
  while (*body == ' ') body++;
  return body;
}

// ---------------------------------------------------------------------------
// Ellipsis truncation for the single-line mini bubble
// ---------------------------------------------------------------------------

inline bool utf8Continuation(char c) {
  return (static_cast<unsigned char>(c) & 0xC0u) == 0x80u;
}

// Pixel width of `s` rendered in `font`.
inline lv_coord_t textWidth(const char* s, const lv_font_t* font) {
  if (!s || !s[0]) return 0;
  lv_point_t size;
  lv_txt_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  return size.x;
}

// Copy `src` to `out`, truncating with a leading "..." if it exceeds `max_w`
// pixels in `font`. The result never exceeds `out_len` bytes and is always
// NUL-terminated (empty if even the ellipsis does not fit).
inline void fitLeadingEllipsis(const char* src, lv_coord_t max_w,
                               const lv_font_t* font, char* out, size_t out_len) {
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (!src || !src[0] || max_w <= 0) return;
  if (textWidth(src, font) <= max_w) {
    snprintf(out, out_len, "%s", src);
    return;
  }

  const char* ell = "...";
  if (textWidth(ell, font) > max_w) return;

  // Walk forward by whole code points; at each step, test if ell + tail fits.
  const size_t len = strlen(src);
  for (size_t i = 0; i < len; ) {
    if (utf8Continuation(src[i])) { ++i; continue; }
    // Build candidate "..." + src[i..] directly into out as a temp.
    size_t need = 3 + (len - i);
    if (need + 1 > out_len) break;
    char tmp[96];  // small temp; mini-bubble text is short
    if (need + 1 > sizeof(tmp)) break;
    memcpy(tmp, ell, 3);
    memcpy(tmp + 3, src + i, len - i);
    tmp[3 + (len - i)] = '\0';
    if (textWidth(tmp, font) <= max_w) {
      snprintf(out, out_len, "%s", tmp);
      return;
    }
    // Advance to next code point.
    ++i;
    while (i < len && utf8Continuation(src[i])) ++i;
  }
  snprintf(out, out_len, "%s", ell);
}

// ---------------------------------------------------------------------------
// Quote formatting
// ---------------------------------------------------------------------------

// Build the single-line mini-bubble text: "sender: original text" truncated
// to fit `max_text_width` pixels in `font`. `out` is always NUL-terminated.
inline void formatQuote(const char* sender, const char* text,
                        lv_coord_t max_text_width, const lv_font_t* font,
                        char* out, size_t out_len) {
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (!sender) sender = "";
  if (!text) text = "";

  char raw[kMaxText + kMaxSender + 4];
  snprintf(raw, sizeof(raw), "%s: %s", sender, text);
  fitLeadingEllipsis(raw, max_text_width, font, out, out_len);
}

}  // namespace ReplyQuote
