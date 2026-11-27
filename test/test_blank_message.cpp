// SPDX-License-Identifier: GPL-3.0-or-later
//
// g++ -std=c++17 -I src test/test_blank_message.cpp -o /tmp/test_blank_message && /tmp/test_blank_message

#include <assert.h>
#include <stdio.h>

#include "ui-touch/BlankMessage.h"

int main() {
  // Nothing to send.
  assert(BlankMessage::isBlank(nullptr));
  assert(BlankMessage::isBlank(""));
  assert(BlankMessage::isBlank(" "));
  assert(BlankMessage::isBlank("  "));
  assert(BlankMessage::isBlank("\t \n\r "));
  assert(BlankMessage::isBlank("\v\f"));          // every ASCII whitespace character
  assert(BlankMessage::isBlank(" \t\n\v\f\r"));

  // A real message, however short or padded, still goes out.
  assert(!BlankMessage::isBlank("k"));
  assert(!BlankMessage::isBlank("  hi  "));
  assert(!BlankMessage::isBlank(" ."));
  assert(!BlankMessage::isBlank("\xf0\x9f\x91\x8d"));   // a lone emoji is a message
  assert(!BlankMessage::isBlank(" \xf0\x9f\x91\x8d "));

  puts("test_blank_message: ok");
  return 0;
}
