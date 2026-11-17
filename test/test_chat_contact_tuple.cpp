// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/ChatContactTuple.h"

int main() {
  const char* key = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
  char tuple[128];
  snprintf(tuple, sizeof tuple, "<%s:1:Test Node>", key);
  ChatContactTuple::Contact contact{};
  size_t start = 0;
  assert(ChatContactTuple::span(tuple, strlen(tuple), 0, &start, &contact));
  assert(start == 0);
  assert(contact.end == strlen(tuple));
  assert(contact.type == 1);
  assert(strcmp(contact.name, "Test Node") == 0);
  assert(strcmp(contact.public_key_hex, key) == 0);
  const uint8_t expected_key[4] = {0x00, 0x01, 0x02, 0x03};
  assert(memcmp(contact.public_key, expected_key, sizeof expected_key) == 0);

  for (uint8_t type = 1; type <= 4; ++type) {
    snprintf(tuple, sizeof tuple, "before <%s:%u:Node> after", key, type);
    assert(ChatContactTuple::span(tuple, strlen(tuple), 0, &start, &contact));
    assert(start == 7);
    assert(contact.type == type);
    assert(strcmp(contact.name, "Node") == 0);
    assert(!ChatContactTuple::span(tuple, strlen(tuple), contact.end, &start, &contact));
  }

  snprintf(tuple, sizeof tuple, "<%s:1:>", key);
  assert(!ChatContactTuple::span(tuple, strlen(tuple), 0, &start, &contact));
  snprintf(tuple, sizeof tuple, "<%s:5:Node>", key);
  assert(!ChatContactTuple::span(tuple, strlen(tuple), 0, &start, &contact));
  snprintf(tuple, sizeof tuple, "<%s:10:Node>", key);
  assert(!ChatContactTuple::span(tuple, strlen(tuple), 0, &start, &contact));
  snprintf(tuple, sizeof tuple, "<%s:1:This name is much too long for ContactInfo>", key);
  assert(!ChatContactTuple::span(tuple, strlen(tuple), 0, &start, &contact));
  snprintf(tuple, sizeof tuple, "<%s:1:Node", key);
  assert(!ChatContactTuple::span(tuple, strlen(tuple), 0, &start, &contact));

  tuple[2] = 'z';
  assert(!ChatContactTuple::span(tuple, strlen(tuple), 0, &start, &contact));

  char display[256];
  snprintf(display, sizeof display, "Add <%s:1:Test Node> now", key);
  ChatContactTuple::Contact contacts[2]{};
  assert(ChatContactTuple::extractAndRemove(display, contacts, 2) == 1);
  assert(strcmp(display, "Add now") == 0);
  assert(contacts[0].type == 1);
  assert(strcmp(contacts[0].name, "Test Node") == 0);
  assert(strcmp(contacts[0].public_key_hex, key) == 0);

  snprintf(display, sizeof display, "<%s:2:Repeater> and <%s:4:Sensor>", key, key);
  assert(ChatContactTuple::extractAndRemove(display, contacts, 2) == 2);
  assert(strcmp(display, "and") == 0);
  assert(contacts[0].type == 2);
  assert(strcmp(contacts[0].name, "Repeater") == 0);
  assert(contacts[1].type == 4);
  assert(strcmp(contacts[1].name, "Sensor") == 0);

  snprintf(display, sizeof display, "literal <contact>");
  assert(ChatContactTuple::extractAndRemove(display, contacts, 2) == 0);
  assert(strcmp(display, "literal <contact>") == 0);
}
