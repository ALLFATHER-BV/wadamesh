// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Wire formats the repeater admin app reads (MeshCore 1.18 simple_repeater /
// simple_room_server). Pure functions, no LVGL and no mesh, so the parsing is
// host-tested (test/test_repeater_admin_proto.cpp). See dev-docs/REPEATER_ADMIN.md
// for where each layout comes from.
namespace RepeaterAdminProto {

// Binary request types (first payload byte of a PAYLOAD_TYPE_REQ).
enum : uint8_t {
  REQ_GET_STATUS      = 0x01,   // guests too
  REQ_GET_TELEMETRY   = 0x03,
  REQ_GET_ACCESS_LIST = 0x05,   // admin only
  REQ_GET_NEIGHBOURS  = 0x06,   // repeaters only, guests too
  REQ_GET_OWNER_INFO  = 0x07,   // repeaters only (FIRMWARE_VER_LEVEL >= 2), guests too
};

static inline uint16_t rdU16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline int16_t  rdI16(const uint8_t* p) { return (int16_t)rdU16(p); }
static inline uint32_t rdU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// REQ_TYPE_GET_STATUS reply (after the 4-byte tag). Both servers send the same
// first 48 bytes; a repeater adds rx airtime + receive errors (56 bytes), a room
// server adds its post counters (52 bytes).
struct Status {
  uint16_t batt_mv;
  uint16_t queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t recv, sent;
  uint32_t air_secs, uptime_secs;
  uint32_t sent_flood, sent_direct, recv_flood, recv_direct;
  uint16_t err_events;
  int16_t  last_snr_q4;
  uint16_t direct_dups, flood_dups;
  // repeater extras (has_rx = true)
  bool     has_rx;
  uint32_t rx_air_secs, recv_errors;
  // room server extras (has_posts = true)
  bool     has_posts;
  uint16_t posted, post_push;
};

inline bool parseStatus(const uint8_t* d, size_t len, bool is_room, Status& s) {
  memset(&s, 0, sizeof s);
  if (!d || len < 48) return false;
  s.batt_mv     = rdU16(d + 0);
  s.queue_len   = rdU16(d + 2);
  s.noise_floor = rdI16(d + 4);
  s.last_rssi   = rdI16(d + 6);
  s.recv        = rdU32(d + 8);
  s.sent        = rdU32(d + 12);
  s.air_secs    = rdU32(d + 16);
  s.uptime_secs = rdU32(d + 20);
  s.sent_flood  = rdU32(d + 24);
  s.sent_direct = rdU32(d + 28);
  s.recv_flood  = rdU32(d + 32);
  s.recv_direct = rdU32(d + 36);
  s.err_events  = rdU16(d + 40);
  s.last_snr_q4 = rdI16(d + 42);
  s.direct_dups = rdU16(d + 44);
  s.flood_dups  = rdU16(d + 46);
  if (is_room) {
    if (len >= 52) { s.has_posts = true; s.posted = rdU16(d + 48); s.post_push = rdU16(d + 50); }
  } else if (len >= 56) {
    s.has_rx = true; s.rx_air_secs = rdU32(d + 48); s.recv_errors = rdU32(d + 52);
  }
  return true;
}

// REQ_TYPE_GET_NEIGHBOURS request (version 0): count, offset, order, prefix
// length, then 4 random bytes so two identical requests hash differently.
enum : uint8_t { NB_NEWEST = 0, NB_OLDEST = 1, NB_STRONGEST = 2, NB_WEAKEST = 3 };
inline uint8_t buildNeighboursReq(uint8_t* out, uint8_t count, uint16_t offset, uint8_t order,
                                  uint8_t prefix_len, uint32_t nonce) {
  out[0] = REQ_GET_NEIGHBOURS;
  out[1] = 0;                       // request version
  out[2] = count;
  out[3] = (uint8_t)(offset & 0xFF);
  out[4] = (uint8_t)(offset >> 8);
  out[5] = order;
  out[6] = prefix_len;
  memcpy(out + 7, &nonce, 4);
  return 11;
}

// Reply: u16 total neighbours, u16 entries here, then per entry
// prefix_len bytes of public key, u32 heard-seconds-ago, i8 SNR x4.
struct Neighbour { uint8_t prefix[8]; uint32_t heard_ago; int8_t snr_q4; };
inline int parseNeighbours(const uint8_t* d, size_t len, uint8_t prefix_len,
                           Neighbour* out, int max_out, uint16_t* total) {
  if (total) *total = 0;
  if (!d || len < 4 || prefix_len == 0 || prefix_len > 8) return -1;
  if (total) *total = rdU16(d);
  const int n = rdU16(d + 2);
  const size_t entry = (size_t)prefix_len + 5;
  int got = 0;
  size_t o = 4;
  for (int i = 0; i < n && o + entry <= len && got < max_out; ++i, o += entry) {
    Neighbour& nb = out[got++];
    memset(&nb, 0, sizeof nb);
    memcpy(nb.prefix, d + o, prefix_len);
    nb.heard_ago = rdU32(d + o + prefix_len);
    nb.snr_q4 = (int8_t)d[o + prefix_len + 4];
  }
  return got;
}

// REQ_TYPE_GET_ACCESS_LIST reply: 7-byte entries (6-byte key prefix + permission
// byte). Request payload is { 0x05, 0, 0 } (two reserved bytes must be zero).
struct AclEntry { uint8_t prefix[6]; uint8_t perms; };
inline int parseAcl(const uint8_t* d, size_t len, AclEntry* out, int max_out) {
  if (!d) return -1;
  int got = 0;
  for (size_t o = 0; o + 7 <= len && got < max_out; o += 7) {
    memcpy(out[got].prefix, d + o, 6);
    out[got].perms = d[o + 6];
    ++got;
  }
  return got;
}
// PERM_ACL_* role in the low two bits.
enum : uint8_t { ROLE_GUEST = 0, ROLE_READ_ONLY = 1, ROLE_READ_WRITE = 2, ROLE_ADMIN = 3 };
static inline uint8_t aclRole(uint8_t perms) { return perms & 3; }

// REQ_TYPE_GET_OWNER_INFO reply: "<firmware version>\n<node name>\n<owner info>".
// Splits in place into three pointers (owner may itself hold newlines: the rest).
inline bool splitOwnerInfo(char* text, const char** ver, const char** name, const char** owner) {
  *ver = *name = *owner = "";
  if (!text) return false;
  char* a = strchr(text, '\n');
  if (!a) { *ver = text; return false; }
  *a = 0; *ver = text;
  char* b = strchr(a + 1, '\n');
  if (!b) { *name = a + 1; return true; }
  *b = 0; *name = a + 1; *owner = b + 1;
  return true;
}

// ---- CLI replies --------------------------------------------------------------
// "get <key>" answers "> <value>"; a "set" answers "OK" (some add text after it,
// e.g. "OK - reboot to apply"); failures start "Error" / "ERR" or say "unknown".
// Returns the value part of a get reply, or nullptr when the reply is not one.
inline const char* cliGetValue(const char* reply) {
  if (!reply) return nullptr;
  while (*reply == ' ' || *reply == '\r' || *reply == '\n') ++reply;
  if (reply[0] != '>') return nullptr;
  ++reply;
  while (*reply == ' ') ++reply;
  return reply;
}
inline bool startsWithNoCase(const char* s, const char* pfx) {
  for (; *pfx; ++s, ++pfx) {
    char a = *s, b = *pfx;
    if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}
enum class CliResult : uint8_t { Ok, Error, Other };
inline CliResult cliClassify(const char* reply) {
  if (!reply) return CliResult::Error;
  while (*reply == ' ' || *reply == '\r' || *reply == '\n') ++reply;
  if (startsWithNoCase(reply, "ok")) return CliResult::Ok;
  if (startsWithNoCase(reply, "err") || startsWithNoCase(reply, "unknown") ||
      startsWithNoCase(reply, "invalid") || strstr(reply, "rror") != nullptr ||
      strstr(reply, "not supported") != nullptr || strstr(reply, "not found") != nullptr)
    return CliResult::Error;
  return CliResult::Other;
}
// Did an OK reply say the change waits for a reboot?
inline bool cliNeedsReboot(const char* reply) {
  if (!reply) return false;
  return strstr(reply, "reboot") != nullptr || strstr(reply, "Reboot") != nullptr;
}

// "on"/"off" style values, as the CLI prints them.
inline int cliBool(const char* v) {
  if (!v) return -1;
  if (startsWithNoCase(v, "on") || startsWithNoCase(v, "1") || startsWithNoCase(v, "true") ||
      startsWithNoCase(v, "yes") || startsWithNoCase(v, "enabled")) return 1;
  if (startsWithNoCase(v, "off") || startsWithNoCase(v, "0") || startsWithNoCase(v, "false") ||
      startsWithNoCase(v, "no") || startsWithNoCase(v, "disabled")) return 0;
  return -1;
}

// "get radio" -> "freq,bw,sf,cr" (e.g. "869.525,250.0,11,5").
struct Radio { float freq, bw; int sf, cr; };
inline bool parseRadio(const char* v, Radio& r) {
  if (!v) return false;
  float f = 0, bw = 0; int sf = 0, cr = 0;
  if (sscanf(v, "%f,%f,%d,%d", &f, &bw, &sf, &cr) != 4) return false;
  if (f < 100.0f || f > 3000.0f || bw <= 0 || sf < 5 || sf > 12 || cr < 5 || cr > 8) return false;
  r.freq = f; r.bw = bw; r.sf = sf; r.cr = cr;
  return true;
}
// The bandwidths the SX126x/LR11xx accept, in kHz (what "set radio" validates against).
static const float kBandwidths[] = { 7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f };
inline bool validBandwidth(float bw) {
  for (float b : kBandwidths) if (bw > b - 0.05f && bw < b + 0.05f) return true;
  return false;
}

// Seconds as "3d 4h", "5h 12m", "7m", "42s".
inline void fmtDuration(uint32_t s, char* out, size_t cap) {
  if (s >= 86400u)     snprintf(out, cap, "%lud %luh", (unsigned long)(s / 86400u), (unsigned long)((s % 86400u) / 3600u));
  else if (s >= 3600u) snprintf(out, cap, "%luh %lum", (unsigned long)(s / 3600u), (unsigned long)((s % 3600u) / 60u));
  else if (s >= 60u)   snprintf(out, cap, "%lum", (unsigned long)(s / 60u));
  else                 snprintf(out, cap, "%lus", (unsigned long)s);
}

}  // namespace RepeaterAdminProto
