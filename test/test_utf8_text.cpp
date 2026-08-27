// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <string.h>
#include <string>

#include "ui-touch/Utf8Text.h"

int main() {
  const char full[] = "Ouderkerk\xE2\x98\x80\xEF\xB8\x8F";
  assert(strlen(full) == 15);
  assert(Utf8Text::valid(full, strlen(full)));

  std::string broken(full, 14);  // Wardrive's former name:sub(1, 14)
  assert(!Utf8Text::valid(broken.data(), broken.size()));

  std::string scratch;
  const char* clean = Utf8Text::sanitize(broken.data(), broken.size(), scratch);
  assert(scratch == "Ouderkerk\xE2\x98\x80?");
  assert(Utf8Text::valid(clean, scratch.size()));

  scratch = "allocated";
  const char* unchanged = Utf8Text::sanitize(full, strlen(full), scratch);
  assert(unchanged == full);
  assert(scratch.empty());

  const char overlong[] = { (char)0xC0, (char)0xAF };
  assert(!Utf8Text::valid(overlong, sizeof overlong));
  Utf8Text::sanitize(overlong, sizeof overlong, scratch);
  assert(scratch == "?");
  return 0;
}