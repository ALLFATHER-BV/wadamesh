// SPDX-License-Identifier: GPL-3.0-or-later
// Host test for the Store's Apps search and sort (src/ui-touch/StoreFilter.h, #863):
// case folding, description matches, an empty query, A -> Z / Z -> A, Newest with
// missing dates last, and apps with an update pinned first in every mode.
// Build + run from the repo root (scripts/run-host-tests.sh does this):
//   c++ -std=c++17 -Wall -Wextra -I src test/test_store_filter.cpp -o /tmp/t && /tmp/t
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "ui-touch/StoreFilter.h"

using namespace storefilter;

namespace {

std::string order(const SortKey* keys, int n, uint8_t mode) {
  int view[16];
  for (int i = 0; i < n; ++i) view[i] = i;
  sortView(view, n, keys, mode);
  std::string s;
  for (int i = 0; i < n; ++i) { if (i) s += ","; s += keys[view[i]].name; }
  return s;
}

void testMatch() {
  assert(appMatches("RF Monitor", "monitor", "RSSI and noise", ""));
  assert(appMatches("RF Monitor", "monitor", "RSSI and noise", nullptr));
  assert(appMatches("RF Monitor", "monitor", "RSSI and noise", "rf mon"));
  assert(appMatches("RF Monitor", "monitor", "RSSI and noise", "MONITOR"));
  assert(appMatches("Snake", "snake", "The classic. Swipe to steer.", "STEER"));   // desc
  assert(appMatches("Tetris", "tetris", "", "etri"));
  assert(!appMatches("Snake", "snake", "The classic", "tetris"));
  assert(appMatches("Space Invaders", "spaceinvaders", nullptr, "spaceinv"));     // id
  assert(!appMatches("Mine", "mine", nullptr, "classic"));   // sideload: no desc
  // UTF-8 bytes compare exactly; only ASCII folds.
  assert(appMatches("ProTreck", "protreck", "déniv", "déniv"));
  assert(!appMatches("ProTreck", "protreck", "déniv", "DÉNIV"));
  char q[8];
  trimQuery(q, sizeof q, "   ");
  assert(q[0] == 0);
  trimQuery(q, sizeof q, "  snake  ");
  assert(strcmp(q, "snake") == 0);
  trimQuery(q, sizeof q, "abcdefghijk");
  assert(strcmp(q, "abcdefg") == 0);
}

void testDates() {
  assert(validDate("2026-11-23"));
  assert(!validDate(""));
  assert(!validDate(nullptr));
  assert(!validDate("2026-1-23"));
  assert(!validDate("2026/11/23"));
  assert(!validDate("2026-11-233"));
}

void testSort() {
  const SortKey k[] = {
    { "snake",   "2026-08-04", false },
    { "Airtime", "2026-10-15", false },
    { "Breakout","2026-08-04", false },
    { "ping",    "",           false },
    { "Music",   "bad",        false },
    { "ebook",   "2026-11-02", false },
  };
  const int n = 6;
  assert(order(k, n, SORT_AZ) == "Airtime,Breakout,ebook,Music,ping,snake");
  assert(order(k, n, SORT_ZA) == "snake,ping,Music,ebook,Breakout,Airtime");
  // Newest first; same-day ties by name; missing/bad dates last, by name.
  assert(order(k, n, SORT_NEWEST) == "ebook,Airtime,Breakout,snake,Music,ping");
  assert(nextSort(SORT_AZ) == SORT_ZA && nextSort(SORT_ZA) == SORT_NEWEST && nextSort(SORT_NEWEST) == SORT_AZ);
}

void testUpdatesPinned() {
  const SortKey k[] = {
    { "Airtime", "2026-10-15", false },
    { "Zork",    "2026-01-01", true  },
    { "Monitor", "",           true  },
    { "Blackjack","2026-11-20", false },
    { "Holdem",  "2026-11-14", true  },
  };
  const int n = 5;
  assert(order(k, n, SORT_AZ) == "Holdem,Monitor,Zork,Airtime,Blackjack");
  assert(order(k, n, SORT_ZA) == "Zork,Monitor,Holdem,Blackjack,Airtime");
  assert(order(k, n, SORT_NEWEST) == "Holdem,Zork,Monitor,Blackjack,Airtime");
  // A filtered view (a search) keeps the matching updates pinned.
  int view[] = { 0, 3, 2 };
  sortView(view, 3, k, SORT_AZ);
  assert(view[0] == 2 && view[1] == 0 && view[2] == 3);
}

void testStableOnEqualNames() {
  const SortKey k[] = { { "Same", "", false }, { "same", "", false }, { "SAME", "", false } };
  int view[] = { 0, 1, 2 };
  sortView(view, 3, k, SORT_ZA);
  assert(view[0] == 0 && view[1] == 1 && view[2] == 2);   // catalog order kept
}

}  // namespace

int main() {
  testMatch();
  testDates();
  testSort();
  testUpdatesPinned();
  testStableOnEqualNames();
  printf("test_store_filter: all passed\n");
  return 0;
}
