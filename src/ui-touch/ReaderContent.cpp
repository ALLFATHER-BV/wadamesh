// SPDX-License-Identifier: GPL-3.0-or-later

#include "ReaderContent.h"

#include <cstdio>
#include <cstring>

namespace ReaderContent {
namespace {

bool ciPrefix(const char* text, size_t text_len, const char* prefix) {
  const size_t prefix_len = strlen(prefix);
  if (text_len < prefix_len) return false;
  for (size_t i = 0; i < prefix_len; ++i) {
    char actual = text[i];
    char expected = prefix[i];
    if (actual >= 'A' && actual <= 'Z') actual += 32;
    if (expected >= 'A' && expected <= 'Z') expected += 32;
    if (actual != expected) return false;
  }
  return true;
}

size_t decodeEntity(const char* text, size_t text_len, char* out, int* wrote) {
  static const struct { const char* name; const char* utf8; } named[] = {
    {"amp;","&"},{"lt;","<"},{"gt;",">"},{"quot;","\""},{"apos;","'"},{"nbsp;"," "},
    {"mdash;","\xe2\x80\x94"},{"ndash;","\xe2\x80\x93"},{"hellip;","\xe2\x80\xa6"},
    {"lsquo;","\xe2\x80\x98"},{"rsquo;","\xe2\x80\x99"},{"ldquo;","\xe2\x80\x9c"},
    {"rdquo;","\xe2\x80\x9d"},{"copy;","\xc2\xa9"},{"reg;","\xc2\xae"},{"euro;","\xe2\x82\xac"},
    {"deg;","\xc2\xb0"},{"middot;","\xc2\xb7"},{"bull;","\xe2\x80\xa2"},{"trade;","\xe2\x84\xa2"},
  };
  for (const auto& entity : named) {
    const size_t name_len = strlen(entity.name);
    if (text_len > name_len && strncmp(text + 1, entity.name, name_len) == 0) {
      const int width = strlen(entity.utf8);
      memcpy(out, entity.utf8, width);
      *wrote = width;
      return name_len + 1;
    }
  }
  if (text_len > 3 && text[1] == '#') {
    long codepoint = 0;
    const bool hex = text[2] == 'x' || text[2] == 'X';
    size_t i = hex ? 3 : 2;
    const size_t start = i;
    for (; i < text_len && text[i] != ';'; ++i) {
      const char c = text[i];
      int digit;
      if (c >= '0' && c <= '9') digit = c - '0';
      else if (hex && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
      else if (hex && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
      else { i = start; break; }
      codepoint = codepoint * (hex ? 16 : 10) + digit;
    }
    if (i > start && i < text_len && text[i] == ';' && codepoint > 0 && codepoint <= 0x10FFFF) {
      int width = 0;
      if (codepoint < 0x80) out[width++] = static_cast<char>(codepoint);
      else if (codepoint < 0x800) {
        out[width++] = 0xC0 | (codepoint >> 6);
        out[width++] = 0x80 | (codepoint & 0x3F);
      } else if (codepoint < 0x10000) {
        out[width++] = 0xE0 | (codepoint >> 12);
        out[width++] = 0x80 | ((codepoint >> 6) & 0x3F);
        out[width++] = 0x80 | (codepoint & 0x3F);
      } else {
        out[width++] = 0xF0 | (codepoint >> 18);
        out[width++] = 0x80 | ((codepoint >> 12) & 0x3F);
        out[width++] = 0x80 | ((codepoint >> 6) & 0x3F);
        out[width++] = 0x80 | (codepoint & 0x3F);
      }
      *wrote = width;
      return i + 1;
    }
  }
  return 0;
}

bool tagAttribute(const char* tag, size_t tag_len, const char* attribute,
                  char* out, size_t cap) {
  const size_t attribute_len = strlen(attribute);
  for (size_t i = 0; i + attribute_len + 1 < tag_len; ++i) {
    if (!ciPrefix(tag + i, tag_len - i, attribute)) continue;
    if (i > 0 && tag[i - 1] != ' ' && tag[i - 1] != '\t' &&
        tag[i - 1] != '\r' && tag[i - 1] != '\n') continue;
    size_t cursor = i + attribute_len;
    while (cursor < tag_len && (tag[cursor] == ' ' || tag[cursor] == '\t')) ++cursor;
    if (cursor >= tag_len || tag[cursor] != '=') continue;
    ++cursor;
    while (cursor < tag_len && (tag[cursor] == ' ' || tag[cursor] == '\t')) ++cursor;
    char quote = 0;
    if (cursor < tag_len && (tag[cursor] == '"' || tag[cursor] == '\'')) {
      quote = tag[cursor++];
    }
    size_t written = 0;
    while (cursor < tag_len && written + 1 < cap) {
      const char c = tag[cursor];
      if (quote ? c == quote : (c == ' ' || c == '>' || c == '\t')) break;
      out[written++] = c;
      ++cursor;
    }
    out[written] = 0;
    return written > 0;
  }
  return false;
}

}  // namespace

bool resolveUrl(const char* base, const char* href, char* out, size_t cap) {
  if (!base || !href || !out || cap == 0) return false;
  while (*href == ' ') ++href;
  const size_t href_len = strlen(href);
  if (!href_len || href[0] == '#') return false;
  if (ciPrefix(href, href_len, "javascript:") || ciPrefix(href, href_len, "mailto:") ||
      ciPrefix(href, href_len, "tel:") || ciPrefix(href, href_len, "data:")) return false;

  if (ciPrefix(href, href_len, "http://") || ciPrefix(href, href_len, "https://") ||
      ciPrefix(href, href_len, "sd:/")) {
    snprintf(out, cap, "%s", href);
  } else if (ciPrefix(base, strlen(base), "sd:/")) {
    if (href[0] == '/' && href[1] == '/') {
      snprintf(out, cap, "https:%s", href);
    } else if (href[0] == '/') {
      snprintf(out, cap, "sd:%s", href);
    } else {
      const char* last_slash = strrchr(base + 3, '/');
      if (last_slash) snprintf(out, cap, "%.*s%s", static_cast<int>(last_slash - base + 1), base, href);
      else            snprintf(out, cap, "sd:/%s", href);
    }
  } else {
    const char* scheme_end = strstr(base, "://");
    if (!scheme_end) return false;
    const int scheme_len = static_cast<int>(scheme_end - base);
    const char* host = scheme_end + 3;
    const char* host_end = strchr(host, '/');
    const int host_len = host_end ? static_cast<int>(host_end - host) : static_cast<int>(strlen(host));
    if (href[0] == '/' && href[1] == '/') {
      snprintf(out, cap, "%.*s:%s", scheme_len, base, href);
    } else if (href[0] == '/') {
      snprintf(out, cap, "%.*s://%.*s%s", scheme_len, base, host_len, host, href);
    } else {
      const char* last_slash = strrchr(base, '/');
      if (last_slash && last_slash > scheme_end + 2)
        snprintf(out, cap, "%.*s%s", static_cast<int>(last_slash - base + 1), base, href);
      else
        snprintf(out, cap, "%.*s://%.*s/%s", scheme_len, base, host_len, host, href);
    }
  }
  char* fragment = strchr(out, '#');
  if (fragment) *fragment = 0;
  return out[0] != 0;
}

size_t htmlToText(const char* html, size_t html_len,
                  char* out, size_t out_cap, const char* base,
                  Link* links, size_t link_capacity, size_t* link_count) {
  if (link_count) *link_count = 0;
  if (!html || !out || out_cap == 0) return 0;

  size_t out_len = 0;
  size_t links_len = 0;
  int pending_newlines = 0;
  bool pending_space = false;
  bool started = false;
  bool in_anchor = false;
  uint32_t anchor_start = 0;
  char anchor_href[HREF_CAPACITY] = "";
  auto emit = [&](char c) { if (out_len + 1 < out_cap) out[out_len++] = c; };
  auto flush = [&]() {
    if (!started) {
      pending_newlines = 0;
      pending_space = false;
      return;
    }
    while (pending_newlines > 0) { emit('\n'); --pending_newlines; }
    if (pending_space) { emit(' '); pending_space = false; }
  };
  auto finishAnchor = [&]() {
    if (in_anchor && out_len > anchor_start && links && links_len < link_capacity) {
      links[links_len].start = anchor_start;
      links[links_len].end = static_cast<uint32_t>(out_len);
      snprintf(links[links_len].href, sizeof links[links_len].href, "%s", anchor_href);
      ++links_len;
    }
    in_anchor = false;
  };
  static const char* blocks[] = {
    "p","div","br","li","ul","ol","tr","h1","h2","h3","h4","h5","h6",
    "section","article","header","footer","table","blockquote","pre","hr","nav","title","body", nullptr
  };

  size_t i = 0;
  while (i < html_len && out_len + 5 < out_cap) {
    const char c = html[i];
    if (c == '<') {
      if (html_len - i >= 4 && html[i + 1] == '!' && html[i + 2] == '-' && html[i + 3] == '-') {
        size_t end = i + 4;
        while (end + 2 < html_len && !(html[end] == '-' && html[end + 1] == '-' && html[end + 2] == '>')) ++end;
        i = end + 2 < html_len ? end + 3 : html_len;
        continue;
      }
      const bool script = ciPrefix(html + i, html_len - i, "<script");
      const bool style = !script && ciPrefix(html + i, html_len - i, "<style");
      if (script || style) {
        const char* closing_tag = script ? "</script" : "</style";
        size_t end = i + 1;
        while (end < html_len && !(html[end] == '<' && ciPrefix(html + end, html_len - end, closing_tag))) ++end;
        while (end < html_len && html[end] != '>') ++end;
        i = end < html_len ? end + 1 : html_len;
        if (pending_newlines < 2) ++pending_newlines;
        continue;
      }

      size_t name_cursor = i + 1;
      const bool closing = name_cursor < html_len && html[name_cursor] == '/';
      if (closing) ++name_cursor;
      char name[12];
      int name_len = 0;
      while (name_cursor < html_len && name_len < 11) {
        char value = html[name_cursor];
        if ((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9')) {
          name[name_len++] = value >= 'A' && value <= 'Z' ? value + 32 : value;
          ++name_cursor;
        } else {
          break;
        }
      }
      name[name_len] = 0;
      size_t tag_end = i + 1;
      while (tag_end < html_len && html[tag_end] != '>') ++tag_end;
      if (name[0] == 'a' && name[1] == 0) {
        finishAnchor();
        if (!closing) {
          char raw_href[300];
          if (base && links &&
              tagAttribute(html + i, (tag_end < html_len ? tag_end : html_len) - i,
                           "href", raw_href, sizeof raw_href) &&
              resolveUrl(base, raw_href, anchor_href, sizeof anchor_href)) {
            flush();
            in_anchor = true;
            anchor_start = static_cast<uint32_t>(out_len);
          }
        }
      }
      i = tag_end < html_len ? tag_end + 1 : html_len;
      for (int block = 0; blocks[block]; ++block) {
        if (strcmp(name, blocks[block]) == 0) {
          pending_space = false;
          if (pending_newlines < 2) ++pending_newlines;
          break;
        }
      }
      continue;
    }
    if (c == '&') {
      char entity[8];
      int width = 0;
      const size_t used = decodeEntity(html + i, html_len - i, entity, &width);
      if (used) {
        for (int byte = 0; byte < width; ++byte) {
          if (entity[byte] == ' ') {
            if (started) pending_space = true;
          } else {
            flush(); emit(entity[byte]); started = true;
          }
        }
        i += used;
        continue;
      }
      flush(); emit('&'); started = true; ++i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (started) pending_space = true;
      ++i;
      continue;
    }
    flush(); emit(c); started = true; ++i;
  }
  finishAnchor();
  out[out_len < out_cap ? out_len : out_cap - 1] = 0;
  if (link_count) *link_count = links_len;
  return out_len;
}

}  // namespace ReaderContent