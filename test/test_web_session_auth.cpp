// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <stdint.h>
#include <string.h>

static uint32_t g_now = 1000;
static uint32_t millis() { return g_now; }

#include "helpers/esp32/WebSessionAuth.h"

static void key(uint8_t k[16], uint8_t v) { memset(k, v, 16); }

int main() {
  WebSessionAuth a;
  uint8_t A[16], B[16];
  key(A, 0xA1);
  key(B, 0xB2);
  char label[32];
  uint32_t seq = 0;

  // A new browser waits; a second one is told the device is busy asking about the first.
  assert(a.hello(A, "Chrome on Mac", 0x1701A8C0) == WebSessionAuth::WAIT);   // 192.168.1.23
  assert(a.hello(A, "Chrome on Mac") == WebSessionAuth::WAIT);   // its other tabs join the same request
  assert(a.hello(B, "Safari") == WebSessionAuth::BUSY);
  uint32_t ip = 0;
  assert(a.pending(label, sizeof label, &seq, &ip) && strcmp(label, "Chrome on Mac") == 0);
  assert(ip == 0x1701A8C0);   // the card shows where the request came from

  // Allow once: in for this console session, not remembered.
  assert(!a.resolve(seq, WebSessionAuth::ALLOW_ONCE));
  assert(a.status(A) == WebSessionAuth::OK);
  assert(!a.pending(nullptr, 0, nullptr));
  assert(a.resolve(seq, WebSessionAuth::DENY) == false && a.approved(A));   // a stale answer is ignored
  assert(a.trustedCount() == 0);

  // Deny: refused, and its retries are refused for a while instead of re-asking.
  assert(a.hello(B, "Safari\x01") == WebSessionAuth::WAIT);
  assert(a.pending(label, sizeof label, &seq) && strcmp(label, "Safari?") == 0);
  a.resolve(seq, WebSessionAuth::DENY);
  assert(a.status(B) == WebSessionAuth::DENIED);
  assert(a.hello(B, "Safari") == WebSessionAuth::DENIED);
  g_now += WebSessionAuth::DENY_HOLD_MS;
  assert(a.hello(B, "Safari") == WebSessionAuth::WAIT);

  // No answer counts as Deny.
  g_now += WebSessionAuth::REQUEST_MS;
  assert(a.status(B) == WebSessionAuth::DENIED);
  assert(!a.pending(nullptr, 0, nullptr));

  // Always allow survives the end of the session; Allow once does not.
  g_now += WebSessionAuth::DENY_HOLD_MS;
  assert(a.hello(B, "Safari") == WebSessionAuth::WAIT);
  a.pending(nullptr, 0, &seq);
  assert(a.resolve(seq, WebSessionAuth::ALLOW_ALWAYS));
  const uint32_t gen = a.generation();
  a.endSessions();
  assert(a.generation() != gen);
  assert(a.hello(B, "Safari") == WebSessionAuth::OK);
  assert(a.hello(A, "Chrome on Mac") == WebSessionAuth::WAIT);   // its Allow-once is gone

  // The code fallback lets the waiting browser in and clears its request.
  a.allowSession(A);
  assert(a.status(A) == WebSessionAuth::OK && !a.pending(nullptr, 0, nullptr));

  // The trusted list round-trips through export/load, and revoking ends access.
  WebTrustedEntry saved[WebSessionAuth::MAX_TRUSTED];
  assert(a.exportTrusted(saved, WebSessionAuth::MAX_TRUSTED) == 1);
  WebSessionAuth b;
  b.loadTrusted(saved, 1);
  WebTrustedEntry e;
  assert(b.trustedAt(0, &e) && strcmp(e.label, "Safari") == 0);
  assert(b.hello(B, "") == WebSessionAuth::OK);
  assert(b.revoke(0) && b.trustedCount() == 0);
  assert(b.hello(B, "") == WebSessionAuth::WAIT);
  assert(b.pending(label, sizeof label, nullptr) && strcmp(label, "Browser") == 0);

  // A full trusted list drops its oldest entry.
  WebSessionAuth c;
  for (int i = 0; i <= WebSessionAuth::MAX_TRUSTED; ++i) {
    uint8_t k[16];
    key(k, (uint8_t)(i + 1));
    assert(c.hello(k, "x") == WebSessionAuth::WAIT);
    c.pending(nullptr, 0, &seq);
    c.resolve(seq, WebSessionAuth::ALLOW_ALWAYS);
  }
  c.endSessions();
  uint8_t first[16];
  key(first, 1);
  assert(c.trustedCount() == WebSessionAuth::MAX_TRUSTED);
  assert(c.hello(first, "x") == WebSessionAuth::WAIT);
  return 0;
}
