// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for the repeater admin app's wire parsing (RepeaterAdminProto.h):
// MeshCore 1.18 RepeaterStats / room ServerStats, GET_NEIGHBOURS, GET_ACCESS_LIST,
// owner info and the CLI reply shapes.
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_repeater_admin_proto.cpp -o /tmp/rap && /tmp/rap

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui-touch/RepeaterAdminProto.h"

using namespace RepeaterAdminProto;

static void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF; }

int main() {
  // RepeaterStats, 56 bytes, as simple_repeater packs it.
  uint8_t d[56] = {0};
  put16(d + 0, 4012); put16(d + 2, 3); put16(d + 4, (uint16_t)-112); put16(d + 6, (uint16_t)-87);
  put32(d + 8, 1000); put32(d + 12, 500); put32(d + 16, 77); put32(d + 20, 90061);
  put32(d + 24, 400); put32(d + 28, 100); put32(d + 32, 800); put32(d + 36, 200);
  put16(d + 40, 2); put16(d + 42, (uint16_t)(int16_t)-26); put16(d + 44, 5); put16(d + 46, 9);
  put32(d + 48, 1234); put32(d + 52, 17);
  Status s;
  assert(parseStatus(d, sizeof d, false, s));
  assert(s.batt_mv == 4012 && s.queue_len == 3 && s.noise_floor == -112 && s.last_rssi == -87);
  assert(s.air_secs == 77 && s.uptime_secs == 90061);   // uptime is at 20, not 16
  assert(s.recv_flood == 800 && s.recv_direct == 200 && s.last_snr_q4 == -26);
  assert(s.has_rx && s.rx_air_secs == 1234 && s.recv_errors == 17 && !s.has_posts);
  // Room ServerStats: 52 bytes, the post counters where the repeater keeps rx airtime.
  put16(d + 48, 42); put16(d + 50, 40);
  assert(parseStatus(d, 52, true, s));
  assert(s.has_posts && s.posted == 42 && s.post_push == 40 && !s.has_rx);
  assert(!parseStatus(d, 47, false, s));

  // GET_NEIGHBOURS request + reply.
  uint8_t req[12];
  assert(buildNeighboursReq(req, 11, 300, NB_STRONGEST, 6, 0xAABBCCDD) == 11);
  assert(req[0] == 0x06 && req[1] == 0 && req[2] == 11 && req[3] == 44 && req[4] == 1 && req[5] == 2 && req[6] == 6);
  uint8_t nb[4 + 2 * 11] = {0};
  put16(nb, 5); put16(nb + 2, 2);
  memcpy(nb + 4, "\x01\x02\x03\x04\x05\x06", 6); put32(nb + 10, 342); nb[14] = 38;
  memcpy(nb + 15, "\xA1\xA2\xA3\xA4\xA5\xA6", 6); put32(nb + 21, 7); nb[25] = (uint8_t)(int8_t)-20;
  Neighbour out[4]; uint16_t total = 0;
  assert(parseNeighbours(nb, sizeof nb, 6, out, 4, &total) == 2 && total == 5);
  assert(out[0].heard_ago == 342 && out[0].snr_q4 == 38 && out[0].prefix[5] == 6);
  assert(out[1].snr_q4 == -20 && out[1].prefix[0] == 0xA1);
  assert(parseNeighbours(nb, 20, 6, out, 4, &total) == 1);   // a cut-off second entry is dropped

  // ACL: 7-byte entries.
  uint8_t acl[14] = { 1, 2, 3, 4, 5, 6, 3, 9, 9, 9, 9, 9, 9, 2 };
  AclEntry ae[4];
  assert(parseAcl(acl, sizeof acl, ae, 4) == 2 && aclRole(ae[0].perms) == ROLE_ADMIN && aclRole(ae[1].perms) == ROLE_READ_WRITE);

  // Owner info.
  char own[] = "v1.18.0 (Build: 15 Oct 2026)\nYagi\nKaj\nline two";
  const char *v, *n, *o;
  assert(splitOwnerInfo(own, &v, &n, &o));
  assert(!strcmp(v, "v1.18.0 (Build: 15 Oct 2026)") && !strcmp(n, "Yagi") && !strcmp(o, "Kaj\nline two"));

  // CLI replies.
  assert(!strcmp(cliGetValue("> 869.525,250.000,11,5"), "869.525,250.000,11,5"));
  assert(cliGetValue("OK") == nullptr);
  assert(!strcmp(cliGetValue(">  x"), "x"));
  assert(cliClassify("OK - reboot to apply") == CliResult::Ok);
  assert(cliNeedsReboot("OK - reboot to apply"));
  assert(cliClassify("Error, invalid radio params") == CliResult::Error);
  assert(cliClassify("ERROR: af must be 0-9") == CliResult::Error);
  assert(cliClassify("Err - not found") == CliResult::Error);
  assert(cliClassify("unknown config: foo") == CliResult::Error);
  assert(cliClassify("??: foo") == CliResult::Other);
  assert(cliClassify("password now: abc") == CliResult::Other);
  assert(cliBool("on") == 1 && cliBool("off") == 0 && cliBool("maybe") == -1);
  Radio r;
  assert(parseRadio("869.525,250.000,11,5", r) && fabsf(r.freq - 869.525f) < 0.001f && r.sf == 11 && r.cr == 5);
  assert(validBandwidth(r.bw) && validBandwidth(62.5f) && !validBandwidth(100.0f));
  assert(!parseRadio("869.5,250,13,5", r));
  char b[16];
  fmtDuration(90061, b, sizeof b); assert(!strcmp(b, "1d 1h"));
  fmtDuration(3725, b, sizeof b);  assert(!strcmp(b, "1h 2m"));
  fmtDuration(42, b, sizeof b);    assert(!strcmp(b, "42s"));
  printf("test_repeater_admin_proto: all assertions passed\n");
  return 0;
}
