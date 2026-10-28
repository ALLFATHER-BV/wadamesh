// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Reply-quote ("@[sender] body") detection and formatting.
//
// A message beginning with "@[sender]" is a reply: the sender names the
// contact whose most recent message in this thread is being answered. The
// prefix is stripped for display and the original message is shown as a
// two-line mini bubble (sender name + quoted text) inside the reply bubble
// (see UITask.cpp's chatVirtCreateBubble).
//
// This file is self-contained text handling — message lookup stays in
// UITask.cpp, which has the thread's ring buffer and sender list.

#include <stddef.h>
#include <string.h>

#ifndef LVGL_H
#include <lvgl.h>
#endif

namespace ReplyQuote {

constexpr size_t kMaxSender = 31;   // UITask::MAX_SENDER_NAME

// ---------------------------------------------------------------------------
// Prefix parsing
// ---------------------------------------------------------------------------

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
// Ellipsis truncation for mini-bubble text
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
    // Skip candidates that can't fit the buffer, but keep looking for
    // shorter tails that would.
    if (need + 1 > out_len) { ++i; while (i < len && utf8Continuation(src[i])) ++i; continue; }
    char tmp[96];  // small temp; mini-bubble text is short
    if (need + 1 > sizeof(tmp)) { ++i; while (i < len && utf8Continuation(src[i])) ++i; continue; }
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

// Build the two-line mini-bubble content for a reply quote. The sender name
// goes on the first line (bold font), the quoted text on the second. Each is
// independently truncated with leading ellipsis to fit `max_text_width`
// pixels in its respective font. Both outputs are always NUL-terminated.
inline void formatQuote(const char* sender, const char* text,
                        lv_coord_t max_text_width,
                        const lv_font_t* sender_font,
                        const lv_font_t* text_font,
                        char* sender_out, size_t sender_out_len,
                        char* text_out, size_t text_out_len) {
  if (!sender) sender = "";
  if (!text) text = "";
  fitLeadingEllipsis(sender, max_text_width, sender_font,
                     sender_out, sender_out_len);
  fitLeadingEllipsis(text, max_text_width, text_font,
                     text_out, text_out_len);
}

}  // namespace ReplyQuote
