// Host test for the T-Display P4 legacy ESP-AT link-health state machine (#685).
// Build + run from the repo root:
//   c++ -std=c++17 -Wall -Wextra -I tdisplay_p4/components/c6_at test/test_c6_link_health.cpp \
//       -o /tmp/test_c6_link_health && /tmp/test_c6_link_health
#include <assert.h>
#include <stdio.h>

#include "c6_link_health.h"

static const uint32_t S = 1000;   // ms per second

int main() {
  // ---- backoff ladder: 30 s, 60 s, 120 s, 240 s, then capped at 5 min ----
  assert(c6h_backoff_ms(0) == 30 * S);
  assert(c6h_backoff_ms(1) == 60 * S);
  assert(c6h_backoff_ms(2) == 120 * S);
  assert(c6h_backoff_ms(3) == 240 * S);
  assert(c6h_backoff_ms(4) == 300 * S);
  assert(c6h_backoff_ms(200) == 300 * S);

  c6h_t h;
  c6h_init(&h);

  // ---- ERROR is an answer: never counts toward a stall ----
  for (int i = 0; i < 10; i++) assert(c6h_note(&h, C6H_RES_ERR, C6H_KIND_CMD, 0) == C6H_ACT_NONE);
  assert(h.streak == 0);

  // ---- timeouts: ping after each, stall on the third in a row ----
  assert(c6h_note(&h, C6H_RES_TIMEOUT, C6H_KIND_CMD, 0) == C6H_ACT_PING);
  assert(c6h_note(&h, C6H_RES_OK, C6H_KIND_PING, 0) == C6H_ACT_NONE);   // parser alive...
  assert(h.streak == 1);                                                // ...streak NOT cleared
  assert(c6h_note(&h, C6H_RES_BUSY, C6H_KIND_CMD, 0) == C6H_ACT_PING);  // busy counts too
  assert(c6h_note(&h, C6H_RES_TIMEOUT, C6H_KIND_CMD, 0) == C6H_ACT_STALL);

  // ---- an answer in between resets the streak ----
  c6h_init(&h);
  c6h_note(&h, C6H_RES_TIMEOUT, C6H_KIND_CMD, 0);
  c6h_note(&h, C6H_RES_TIMEOUT, C6H_KIND_CMD, 0);
  assert(c6h_note(&h, C6H_RES_ERR, C6H_KIND_CMD, 0) == C6H_ACT_NONE);
  assert(h.streak == 0);
  assert(c6h_note(&h, C6H_RES_TIMEOUT, C6H_KIND_CMD, 0) == C6H_ACT_PING);

  // ---- a ping that times out is an immediate stall; busy/ERR pings are alive ----
  c6h_init(&h);
  assert(c6h_note(&h, C6H_RES_BUSY, C6H_KIND_PING, 0) == C6H_ACT_NONE);
  assert(c6h_note(&h, C6H_RES_ERR, C6H_KIND_PING, 0) == C6H_ACT_NONE);
  assert(c6h_note(&h, C6H_RES_TIMEOUT, C6H_KIND_PING, 0) == C6H_ACT_STALL);

  // ---- SOFT timeouts (connect to a dead host) only ask for a ping, never stall ----
  c6h_init(&h);
  for (int i = 0; i < 10; i++)
    assert(c6h_note(&h, C6H_RES_TIMEOUT, C6H_KIND_SOFT, 0) == C6H_ACT_PING);
  assert(h.streak == 0);
  // ...and a SOFT answer does not clear a CMD streak (it says nothing about the AT stack's state)
  c6h_note(&h, C6H_RES_TIMEOUT, C6H_KIND_CMD, 0);
  c6h_note(&h, C6H_RES_OK, C6H_KIND_SOFT, 0);
  assert(h.streak == 1);

  // ---- reset scheduling: first reset immediate, then 30/60/120/240/300/300 s apart ----
  c6h_init(&h);
  uint32_t t = 5000 * S;
  assert(c6h_reset_due(&h, t));
  c6h_on_reset(&h, t);                       // reset #1 at t
  assert(h.streak == 0);
  assert(!c6h_reset_due(&h, t + 29 * S));
  assert(c6h_reset_due(&h, t + 30 * S));
  const uint32_t gaps[] = {30, 60, 120, 240, 300, 300, 300};
  for (unsigned i = 0; i < sizeof(gaps) / sizeof(gaps[0]); i++) {
    assert(!c6h_reset_due(&h, t + gaps[i] * S - 1));
    t += gaps[i] * S;
    assert(c6h_reset_due(&h, t));
    c6h_on_reset(&h, t);
  }

  // ---- stability: 10 min of answers after a reset drops the ladder back to "immediate" ----
  c6h_init(&h);
  t = 100 * S;
  c6h_on_reset(&h, t);
  c6h_on_reset(&h, t + 30 * S);              // two resets: next gap would be 60 s
  t += 30 * S;
  c6h_note(&h, C6H_RES_OK, C6H_KIND_CMD, t + 9 * 60 * S);
  assert(h.resets == 2);                     // 9 min: not stable yet
  assert(!c6h_reset_due(&h, t + 59 * S));
  c6h_note(&h, C6H_RES_OK, C6H_KIND_CMD, t + 10 * 60 * S);
  assert(h.resets == 0);                     // 10 min: ladder reset
  assert(c6h_reset_due(&h, t + 10 * 60 * S + 1));

  // ---- a re-stall soon after a successful reset still honours the backoff ----
  c6h_init(&h);
  t = 1000 * S;
  c6h_on_reset(&h, t);
  c6h_note(&h, C6H_RES_OK, C6H_KIND_CMD, t + 5 * S);       // link came back
  c6h_note(&h, C6H_RES_TIMEOUT, C6H_KIND_CMD, t + 6 * S);
  c6h_note(&h, C6H_RES_TIMEOUT, C6H_KIND_CMD, t + 7 * S);
  assert(c6h_note(&h, C6H_RES_TIMEOUT, C6H_KIND_CMD, t + 8 * S) == C6H_ACT_STALL);
  assert(!c6h_reset_due(&h, t + 8 * S));                   // wait for t + 30 s
  assert(c6h_reset_due(&h, t + 30 * S));

  // ---- millisecond clock wrap (esp_timer ms in a uint32_t wraps after ~49.7 days) ----
  c6h_init(&h);
  t = 0xFFFFFFFFu - 10 * S;
  c6h_on_reset(&h, t);
  assert(!c6h_reset_due(&h, t + 29 * S));    // wrapped past zero, still inside the window
  assert(c6h_reset_due(&h, t + 30 * S));

  printf("c6_link_health: all tests passed\n");
  return 0;
}
