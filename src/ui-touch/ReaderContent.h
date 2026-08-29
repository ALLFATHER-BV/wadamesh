// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace ReaderContent {

constexpr size_t HREF_CAPACITY = 240;

struct Link {
  uint32_t start;
  uint32_t end;
  char href[HREF_CAPACITY];
};

bool resolveUrl(const char* base, const char* href, char* out, size_t cap);

size_t htmlToText(const char* html, size_t html_len,
                  char* out, size_t out_cap, const char* base,
                  Link* links, size_t link_capacity, size_t* link_count);

}  // namespace ReaderContent