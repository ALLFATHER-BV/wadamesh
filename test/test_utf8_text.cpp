// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <string.h>
#include <string>

#include "ui-touch/Utf8Text.h"
#include "ui-touch/ChatHashtag.h"

int main()
{
  int start, end;
  const char tags[] = "Try #caf\xC3\xA8 and #cr\xC3\xA8me-br\xC3\xBBl\xC3\xA9"
                      "e!";
  assert(ChatHashtag::span(tags, 0, &start, &end));
  assert(std::string(tags + start, end - start) == "#caf\xC3\xA8");
  assert(ChatHashtag::span(tags, end, &start, &end));
  assert(std::string(tags + start, end - start) == "#cr\xC3\xA8me-br\xC3\xBBl\xC3\xA9"
                                                   "e");
  assert(!ChatHashtag::span(tags, end, &start, &end));
  const char decomposed[] = "#cafe\xCC\x80";
  assert(ChatHashtag::span(decomposed, 0, &start, &end));
  assert(end == (int)strlen(decomposed));
  assert(!ChatHashtag::span("@[#caf\xC3\xA8]", 0, &start, &end));
  assert(!ChatHashtag::span("https://example.org/#caf\xC3\xA8", 0, &start, &end));
  assert(!ChatHashtag::span("caf\xC3\xA8#tag", 0, &start, &end));
  assert(!ChatHashtag::span("#caf\xC3", 0, &start, &end));
  assert(!ChatHashtag::span("#1", 0, &start, &end));
  assert(!ChatHashtag::span("#1channel", 0, &start, &end));
  assert(ChatHashtag::span("#channel1", 0, &start, &end));
  const std::string trailing_hyphen = "#mesh-";
  assert(ChatHashtag::span(trailing_hyphen.c_str(), 0, &start, &end));
  assert(trailing_hyphen.substr(start, end - start) == "#mesh");
  const std::string trailing_underscore = "#mesh_";
  assert(ChatHashtag::span(trailing_underscore.c_str(), 0, &start, &end));
  assert(trailing_underscore.substr(start, end - start) == "#mesh");
  const std::string hyphenated = "#mesh-channel";
  assert(ChatHashtag::span(hyphenated.c_str(), 0, &start, &end));
  assert(hyphenated.substr(start, end - start) == "#mesh-channel");
  const std::string underscored = "#mesh_channel";
  assert(ChatHashtag::span(underscored.c_str(), 0, &start, &end));
  assert(underscored.substr(start, end - start) == "#mesh_channel");
  assert(!ChatHashtag::span("#FFF", 0, &start, &end));
  assert(!ChatHashtag::span("#FFFF", 0, &start, &end));
  assert(!ChatHashtag::span("#FFFFFF", 0, &start, &end));
  assert(!ChatHashtag::span("#FF0000", 0, &start, &end));
  assert(!ChatHashtag::span("#FFFFFFFF", 0, &start, &end));
  assert(ChatHashtag::span("#cable", 0, &start, &end));
  std::string long_tag = "#";
  for (int count = 0; count < 15; ++count)
    long_tag += "\xC3\xA8";
  assert(ChatHashtag::span(long_tag.c_str(), 0, &start, &end));
  assert(end == 31);
  long_tag += "\xC3\xA8";
  assert(!ChatHashtag::span(long_tag.c_str(), 0, &start, &end));

  const char full[] = "Ouderkerk\xE2\x98\x80\xEF\xB8\x8F";
  assert(strlen(full) == 15);
  assert(Utf8Text::valid(full, strlen(full)));

  std::string broken(full, 14); // Wardrive's former name:sub(1, 14)
  assert(!Utf8Text::valid(broken.data(), broken.size()));

  std::string scratch;
  const char *clean = Utf8Text::sanitize(broken.data(), broken.size(), scratch);
  assert(scratch == "Ouderkerk\xE2\x98\x80?");
  assert(Utf8Text::valid(clean, scratch.size()));

  scratch = "allocated";
  const char *unchanged = Utf8Text::sanitize(full, strlen(full), scratch);
  assert(unchanged == full);
  assert(scratch.empty());

  const char overlong[] = {(char)0xC0, (char)0xAF};
  assert(!Utf8Text::valid(overlong, sizeof overlong));
  Utf8Text::sanitize(overlong, sizeof overlong, scratch);
  assert(scratch == "?");
  return 0;
}