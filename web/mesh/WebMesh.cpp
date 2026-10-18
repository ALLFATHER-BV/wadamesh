// SPDX-License-Identifier: GPL-3.0-or-later
// The MyMesh stand-in (see WebMesh.h). The table semantics below mirror BaseChatMesh /
// src/MyMesh.h one for one (same return values, same UI callbacks in the same order),
// so the UI cannot tell this mirror from a device's. Network work goes to the backend.
#include "WebMesh.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <helpers/TxtDataHelpers.h>
#include "helpers/esp32/TouchPrefsStore.h"
#include <math.h>

namespace {
const char* k_prefs_path = "/web_node_prefs";   // NodePrefs blob (versioned by size)

int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}
int b64decode(const char* in, uint8_t* out, int cap) {
  int n = 0, bits = 0; uint32_t acc = 0;
  for (; *in; ++in) {
    int v = b64val(*in);
    if (v < 0) continue;
    acc = (acc << 6) | (uint32_t)v; bits += 6;
    if (bits >= 8) { bits -= 8; if (n < cap) out[n] = (uint8_t)(acc >> bits); n++; }
  }
  return n;
}
// The channel hash as BaseChatMesh::setChannel derives it (128- or 256-bit key).
void channelHash(ChannelDetails& cd) {
  static const uint8_t zeroes[16] = {0};
  const int klen = memcmp(&cd.channel.secret[16], zeroes, 16) == 0 ? 16 : 32;
  mesh::Utils::sha256(cd.channel.hash, sizeof(cd.channel.hash), cd.channel.secret, klen);
}
void (*s_terminal_sink)(const char*) = nullptr;

// The device's "repeats heard" fingerprint (src/MyMesh.cpp fnv1a32): FNV-1a over a
// flood's payload, which repeaters leave unchanged.
uint32_t fnv1a32(const uint8_t* d, int n) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
  return h ? h : 1;
}
bool isMsgFloodType(uint8_t t) { return t == PAYLOAD_TYPE_TXT_MSG || t == PAYLOAD_TYPE_GRP_TXT; }

// Where a raw packet's path and payload start (header, optional transport codes, the
// packed path-length byte: low 6 bits hop count, top 2 bits hash size - 1).
struct RawView {
  uint8_t ptype = 0, route = 0, hops = 0, hash_sz = 1;
  int path_off = 0, payload_off = 0;
  bool ok = false;
};
RawView rawView(const uint8_t* raw, size_t len) {
  RawView v;
  if (!raw || len < 2) return v;
  v.route = raw[0] & 0x03;
  v.ptype = (raw[0] >> 2) & 0x0F;
  const bool xp = (v.route == ROUTE_TYPE_TRANSPORT_FLOOD || v.route == ROUTE_TYPE_TRANSPORT_DIRECT);
  const int ps = 1 + (xp ? 4 : 0);
  if ((size_t)ps >= len) return v;
  const uint8_t plb = raw[ps];
  v.hops = plb & 0x3F;
  v.hash_sz = (uint8_t)(((plb >> 6) & 0x03) + 1);
  v.path_off = ps + 1;
  v.payload_off = v.path_off + (int)v.hops * (int)v.hash_sz;
  v.ok = (size_t)v.payload_off <= len;
  return v;
}
}  // namespace

MyMesh::MyMesh(mesh::RTCClock& rtc, AbstractUITask* ui) : _rtc(&rtc), _ui(ui) {
  memset(_contacts, 0, sizeof _contacts);
  memset(_channels, 0, sizeof _channels);
  memset(_heard, 0, sizeof _heard);
}

void MyMesh::loadPrefs() {
  // Defaults: src/MyMesh.cpp's constructor values.
  _prefs = NodePrefs{};
  _prefs.airtime_factor = 1.0f;
  StrHelper::strncpy(_prefs.node_name, "wadamesh web", sizeof(_prefs.node_name));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.rx_boosted_gain = 1;
  _prefs.autoadd_max_hops = 64;
  File f = SPIFFS.open(k_prefs_path, "r");
  if (f) {
    NodePrefs p{};
    if (f.size() == sizeof p && f.read((uint8_t*)&p, sizeof p) == (int)sizeof p) _prefs = p;
    f.close();
  }
}

bool MyMesh::savePrefs() {
  File f = SPIFFS.open(k_prefs_path, "w");
  bool ok = false;
  if (f) { ok = f.write((const uint8_t*)&_prefs, sizeof _prefs) == sizeof _prefs; f.close(); }
  if (_backend) _backend->prefsChanged(_prefs);
  return ok;
}

void MyMesh::begin(MeshBackend* backend) {
  _backend = backend;
  loadPrefs();
  _region_reg.begin(&SPIFFS);
  // Identity: whatever the backend says this node is (a companion reports its key;
  // the demo derives a fixed one), kept in a LocalIdentity like the device does.
  uint8_t pub[PUB_KEY_SIZE] = {0};
  NodePrefs from_backend = _prefs;
  if (_backend) _backend->begin(*this, from_backend, pub);
  // A node name the user set here wins over the backend's default.
  File f = SPIFFS.open(k_prefs_path, "r");
  const bool have_saved = (bool)f;
  if (f) f.close();
  if (!have_saved) _prefs = from_backend;
  memcpy(self_id.pub_key, pub, PUB_KEY_SIZE);
  Serial.printf("[web] mesh backend: %s, %d contacts\n", _backend ? _backend->name() : "none", _num_contacts);
  // (A companion backend reports the node's identity and settings once its link is
  // up: beSelfInfo, which overrides the placeholders above.)
}

void MyMesh::loop() {
  const uint32_t now = millis();
  if (_backend) _backend->loop(now);
  if (now - _pos_sync_ms >= 5000) { _pos_sync_ms = now; syncAdvertPosition(now); }
}

// ---- this computer's position and the node (WEB_BUILD.md "Location") --------------------
// The device's advertPosition() (src/MyMesh.cpp, #399), unchanged: the Location privacy
// radius moves the ADVERTISED point by a fixed offset derived from the node key and the
// radius; the local map keeps the real one.
static void advertDisplace(const uint8_t pub_key[PUB_KEY_SIZE], double& lat, double& lon) {
  const uint16_t r = touchPrefsGetGpsFuzzM();
  if (r == 0 || (lat == 0 && lon == 0)) return;
  uint32_t h = 2166136261u;
  for (int i = 0; i < PUB_KEY_SIZE; ++i) { h ^= pub_key[i]; h *= 16777619u; }
  h ^= (uint32_t)r;         h *= 16777619u;
  h ^= (uint32_t)(r >> 8);  h *= 16777619u;
  const double bearing = ((double)(h & 0xFFFFu) / 65536.0) * 2.0 * 3.14159265358979;
  const double dist    = (0.5 + ((double)((h >> 16) & 0xFFFFu) / 131072.0)) * (double)r;
  const double dlat    = (dist * cos(bearing)) / 111320.0;
  double coslat = cos(lat * 3.14159265358979 / 180.0);
  if (coslat < 0.01) coslat = 0.01;
  const double dlon    = (dist * sin(bearing)) / (111320.0 * coslat);
  lat += dlat;
  lon += dlon;
}

// A device advertises sensors.node_lat/lon (its GPS fix, or the Profile position) only
// while advert_loc_policy is ADVERT_LOC_SHARE. Here the advert is the node's, so the
// same rule decides whether the position the UI holds is handed to the node at all:
//   * share off: it stays in this browser (map, distances, Lua); nothing is sent
//   * share on, and the UI's position differs from the node's (this computer's GPS, a
//     Profile edit): CMD_SET_ADVERT_LATLON with the Location privacy displacement,
//     at most every 2 minutes (the device's own persist rate), the first one at once
// The demo has no node to tell (ownsPrefs() is false), and a read-only link refuses it.
void MyMesh::syncAdvertPosition(uint32_t now_ms) {
  if (!_backend || !_backend->ownsPrefs()) return;
  if (_prefs.advert_loc_policy != ADVERT_LOC_SHARE) return;   // the privacy rule
  const double la = sensors.node_lat, lo = sensors.node_lon;
  if (la == 0.0 && lo == 0.0) return;
  if (fabs(la - _node_pos_lat) < 1e-4 && fabs(lo - _node_pos_lon) < 1e-4) return;   // ~11 m
  if (_pos_pushed_ms && now_ms - _pos_pushed_ms < 120000) return;
  double alat = la, alon = lo;
  advertDisplace(self_id.pub_key, alat, alon);
  _backend->advertPositionChanged((int32_t)llround(alat * 1.0e6), (int32_t)llround(alon * 1.0e6));
  _node_pos_lat = la;
  _node_pos_lon = lo;
  _pos_pushed_ms = now_ms ? now_ms : 1;
}
void MyMesh::beSelfPosition(int32_t lat_e6, int32_t lon_e6) {
  sensors.node_lat = (double)lat_e6 / 1.0e6;
  sensors.node_lon = (double)lon_e6 / 1.0e6;
  if (_ui) _ui->onThreadsChanged();
}

bool MyMesh::setBLEPin(uint32_t pin) {
  if (!_backend || !_backend->setBlePin(pin)) return false;
  _prefs.ble_pin = pin;
  return true;
}

void MyMesh::applyRadioFromPrefs() {
  if (_backend) _backend->prefsChanged(_prefs);
}

// ---- contacts -------------------------------------------------------------------------
bool MyMesh::getContactByIdx(uint32_t idx, ContactInfo& contact) {
  if (idx >= (uint32_t)_num_contacts) return false;
  contact = _contacts[idx];
  return true;
}
ContactInfo* MyMesh::lookupContactByPubKey(const uint8_t* pub_key, int prefix_len) {
  if (!pub_key || prefix_len <= 0) return nullptr;
  for (int i = 0; i < _num_contacts; i++)
    if (memcmp(_contacts[i].id.pub_key, pub_key, prefix_len) == 0) return &_contacts[i];
  return nullptr;
}
bool MyMesh::addContact(const ContactInfo& contact) {
  if (_num_contacts >= MAX_CONTACTS) return false;
  _contacts[_num_contacts] = contact;
  _contacts[_num_contacts].shared_secret_valid = false;
  _num_contacts++;
  return true;
}
void MyMesh::resetPathTo(ContactInfo& recipient) { recipient.out_path_len = OUT_PATH_UNKNOWN; }
int MyMesh::uiContactIdxByPubKey(const uint8_t pub_key[32]) {
  for (int i = 0; i < _num_contacts; i++)
    if (memcmp(_contacts[i].id.pub_key, pub_key, PUB_KEY_SIZE) == 0) return i;
  return -1;
}
bool MyMesh::uiResetContactPath(const uint8_t pub_key[32]) {
  ContactInfo* slot = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!slot) return false;
  slot->out_path_len = OUT_PATH_UNKNOWN;
  memset(slot->out_path, 0, sizeof(slot->out_path));
  if (_backend) _backend->contactPathReset(pub_key);
  return true;
}
bool MyMesh::uiShareContact(const uint8_t pub_key[32]) {
  if (!_backend || !lookupContactByPubKey(pub_key, PUB_KEY_SIZE)) return false;
  return _backend->shareContact(pub_key);
}
bool MyMesh::uiSetContactGps(const uint8_t pub_key[32], int32_t lat_e6, int32_t lon_e6) {
  ContactInfo* slot = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!slot) return false;
  slot->gps_lat = lat_e6;
  slot->gps_lon = lon_e6;
  slot->lastmod = getRTCClock()->getCurrentTime();
  if (_backend) _backend->contactChanged(*slot);
  return true;
}
bool MyMesh::uiAddManualContact(const uint8_t pub_key[32], const char* name) {
  if (!name || !name[0]) return false;
  if (lookupContactByPubKey(pub_key, PUB_KEY_SIZE) != nullptr) return false;
  ContactInfo ci{};
  memcpy(ci.id.pub_key, pub_key, PUB_KEY_SIZE);
  ci.type = ADV_TYPE_CHAT;
  ci.out_path_len = OUT_PATH_UNKNOWN;
  ci.lastmod = getRTCClock()->getCurrentTime();
  StrHelper::strncpy(ci.name, name, sizeof(ci.name));
  if (!addContact(ci)) return false;
  if (_backend) _backend->contactChanged(ci);
  if (_ui) _ui->onThreadsChanged();
  return true;
}
bool MyMesh::uiAddDiscoveredContact(const uint8_t pub_key[32], uint8_t type, const char* name) {
  if (!name || !name[0]) return false;
  if (lookupContactByPubKey(pub_key, PUB_KEY_SIZE) != nullptr) return false;
  ContactInfo ci{};
  memcpy(ci.id.pub_key, pub_key, PUB_KEY_SIZE);
  ci.type = type;
  ci.out_path_len = OUT_PATH_UNKNOWN;
  ci.lastmod = getRTCClock()->getCurrentTime();
  StrHelper::strncpy(ci.name, name, sizeof(ci.name));
  if (!addContact(ci)) return false;
  if (_backend) _backend->contactChanged(ci);
  if (_ui) _ui->onThreadsChanged();
  return true;
}
static uint8_t contactTelemBit(uint8_t telem_perm) { return (uint8_t)(telem_perm << 1); }
bool MyMesh::uiGetContactTelemetryPerm(const uint8_t pub_key[32], uint8_t telem_perm) {
  ContactInfo* slot = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  return slot && (slot->flags & contactTelemBit(telem_perm)) != 0;
}
bool MyMesh::uiSetContactTelemetryPerm(const uint8_t pub_key[32], uint8_t telem_perm, bool on) {
  ContactInfo* slot = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!slot) return false;
  const uint8_t bit = contactTelemBit(telem_perm);
  const uint8_t before = slot->flags;
  slot->flags = on ? (uint8_t)(slot->flags | bit) : (uint8_t)(slot->flags & ~bit);
  if (_backend && slot->flags != before) _backend->contactChanged(*slot);
  return true;
}
bool MyMesh::uiSetContactFavorite(const uint8_t pub_key[32], bool fav) {
  ContactInfo* slot = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!slot) return false;
  const uint8_t before = slot->flags;
  slot->flags = fav ? (uint8_t)(slot->flags | 0x01) : (uint8_t)(slot->flags & ~0x01);
  if (_backend && slot->flags != before) _backend->contactChanged(*slot);
  return true;
}
static bool removeAt(ContactInfo* table, int& n, const uint8_t* pub_key) {
  int idx = 0;
  while (idx < n && memcmp(table[idx].id.pub_key, pub_key, PUB_KEY_SIZE) != 0) idx++;
  if (idx >= n) return false;
  n--;
  for (; idx < n; idx++) table[idx] = table[idx + 1];
  return true;
}
bool MyMesh::uiRemoveContact(const ContactInfo& c) {
  if (!removeAt(_contacts, _num_contacts, c.id.pub_key)) return false;
  if (_backend) _backend->contactRemoved(c.id.pub_key);
  if (_ui) _ui->onThreadsChanged();
  return true;
}
bool MyMesh::uiRemoveContactNoSave(const uint8_t pub_key[PUB_KEY_SIZE]) {
  if (!removeAt(_contacts, _num_contacts, pub_key)) return false;
  if (_backend) _backend->contactRemoved(pub_key);
  return true;
}
bool MyMesh::removeTransientContact(const uint8_t pub_key[PUB_KEY_SIZE]) {
  ContactInfo* slot = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!slot || slot->type != ADV_TYPE_NONE) return false;
  return removeAt(_contacts, _num_contacts, pub_key);
}
bool MyMesh::uiHopName(const uint8_t* hash, int prefix_len, char* out, size_t out_sz) {
  if (!out || out_sz == 0) return false;
  out[0] = '\0';
  if (!hash || prefix_len <= 0) return false;
  ContactInfo* c = lookupContactByPubKey(hash, prefix_len);
  if (!c || c->name[0] == '\0') return false;
  strncpy(out, c->name, out_sz - 1);
  out[out_sz - 1] = '\0';
  return true;
}
bool MyMesh::uiHopPos(const uint8_t* hash, int prefix_len, double* out_lat, double* out_lon) {
  if (!hash || prefix_len <= 0) return false;
  ContactInfo* c = lookupContactByPubKey(hash, prefix_len);
  if (!c || (c->gps_lat == 0 && c->gps_lon == 0)) return false;
  if (out_lat) *out_lat = (double)c->gps_lat / 1.0e6;
  if (out_lon) *out_lon = (double)c->gps_lon / 1.0e6;
  return true;
}
void MyMesh::noteHeard(const ContactInfo& c) {
  if (!c.name[0]) return;
  int slot = -1;
  for (int i = 0; i < _heard_n; i++)
    if (memcmp(_heard[i].pubkey_prefix, c.id.pub_key, sizeof(_heard[i].pubkey_prefix)) == 0) { slot = i; break; }
  if (slot < 0) {
    if (_heard_n < HEARD_MAX) slot = _heard_n++;
    else {   // evict the oldest
      slot = 0;
      for (int i = 1; i < _heard_n; i++) if (_heard[i].recv_seq < _heard[slot].recv_seq) slot = i;
    }
  }
  RecentlyHeardName& h = _heard[slot];
  memcpy(h.pubkey_prefix, c.id.pub_key, sizeof(h.pubkey_prefix));
  StrHelper::strncpy(h.name, c.name, sizeof(h.name));
  h.recv_seq = ++_heard_seq;
  h.type = c.type;
}
int MyMesh::getRecentlyHeard(RecentlyHeardName dest[], int max_num) {
  if (!dest || max_num <= 0) return 0;
  RecentlyHeardName snap[HEARD_MAX];
  int n = _heard_n;
  memcpy(snap, _heard, sizeof(snap[0]) * (size_t)n);
  // newest first, like MyMesh::getRecentlyHeard's sort_by_recent
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && snap[j].recv_seq > snap[j - 1].recv_seq; j--) { RecentlyHeardName t = snap[j]; snap[j] = snap[j - 1]; snap[j - 1] = t; }
  if (max_num > n) max_num = n;
  memcpy(dest, snap, (size_t)max_num * sizeof(dest[0]));
  return max_num;
}
bool MyMesh::getAdvertPath(const uint8_t* pubkey_prefix, int prefix_len, uint8_t& path_len,
                           uint8_t* path, size_t path_cap) {
  // The route of the last advert heard from this node, from the raw RX pushes (beRxRaw);
  // the same table and answer as src/MyMesh.cpp's. The demo reports none: "unknown".
  if (!pubkey_prefix || prefix_len <= 0) return false;
  if (prefix_len > (int)sizeof(AdvertPath::pubkey_prefix)) prefix_len = sizeof(AdvertPath::pubkey_prefix);
  for (int i = 0; i < ADVERT_PATHS; i++) {
    const AdvertPath& p = _advert_paths[i];
    if (!p.used || memcmp(p.pubkey_prefix, pubkey_prefix, (size_t)prefix_len) != 0) continue;
    const size_t bytes = (size_t)(p.path_len & 63) * (size_t)((p.path_len >> 6) + 1);
    if (bytes > path_cap || bytes > sizeof(p.path) || (bytes && !path)) return false;
    path_len = p.path_len;
    if (bytes) memcpy(path, p.path, bytes);
    return true;
  }
  return false;
}
void MyMesh::uiExportBackup(Print& out, double node_lat, double node_lon) {
  (void)node_lat; (void)node_lon;
  out.print("{}");
}
bool MyMesh::uiImportBackup(Stream& in, uint8_t sections, bool replace_channels, bool replace_contacts,
                            int* out_channels, int* out_contacts) {
  (void)in; (void)sections; (void)replace_channels; (void)replace_contacts;
  if (out_channels) *out_channels = 0;
  if (out_contacts) *out_contacts = 0;
  return false;
}
bool MyMesh::uiFactoryReset() { return SPIFFS.format(); }

// ---- channels -------------------------------------------------------------------------
bool MyMesh::getChannel(int idx, ChannelDetails& dest) {
  if (idx < 0 || idx >= MAX_GROUP_CHANNELS) return false;
  dest = _channels[idx];
  return true;
}
int MyMesh::findFirstEmptyChannelSlot() {
  for (int i = 0; i < MAX_GROUP_CHANNELS; ++i) if (_channels[i].name[0] == '\0') return i;
  return -1;
}
bool MyMesh::uiAddOrUpdateChannel(int idx, const char* name, const uint8_t secret16[16]) {
  if (idx < 0 || idx >= MAX_GROUP_CHANNELS) return false;
  ChannelDetails ch{};
  StrHelper::strncpy(ch.name, name, sizeof(ch.name));
  memcpy(ch.channel.secret, secret16, 16);
  channelHash(ch);
  _channels[idx] = ch;
  if (_backend) _backend->channelChanged(idx, ch);
  if (_ui) _ui->onThreadsChanged();
  return true;
}
bool MyMesh::uiDeleteChannel(int idx) {
  if (idx < 0 || idx >= MAX_GROUP_CHANNELS) return false;
  ChannelDetails empty{};
  channelHash(empty);
  _channels[idx] = empty;
  if (_backend) _backend->channelChanged(idx, empty);
  if (_ui) _ui->onThreadsChanged();
  return true;
}
bool MyMesh::uiJoinPublicChannel() {
  for (int i = 0; i < MAX_GROUP_CHANNELS; ++i)
    if (_channels[i].name[0] && strncasecmp(_channels[i].name, "Public", 6) == 0) return true;
  const int idx = findFirstEmptyChannelSlot();
  if (idx < 0) return false;
  uint8_t secret[32] = {0};
  if (b64decode("izOH6cXN6mrJ5e26oRXNcg==", secret, sizeof secret) != 16) return false;   // MyMesh.cpp PUBLIC_GROUP_PSK
  return uiAddOrUpdateChannel(idx, "Public", secret);
}
void MyMesh::setDefaultFloodScope(const char* region_name) {
  StrHelper::strncpy(_prefs.default_scope_name, region_name ? region_name : "", sizeof(_prefs.default_scope_name));
  savePrefs();
}
bool MyMesh::pushChannelScope(const char* region_name) { return region_name && region_name[0]; }
int MyMesh::channelIdxOf(const mesh::GroupChannel& ch) const {
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++)
    if (_channels[i].name[0] && memcmp(ch.secret, _channels[i].channel.secret, sizeof(ch.secret)) == 0) return i;
  return -1;
}

// ---- sending --------------------------------------------------------------------------
int MyMesh::sendMessage(const ContactInfo& recipient, uint32_t timestamp, uint8_t attempt,
                        const char* text, uint32_t& expected_ack, uint32_t& est_timeout,
                        uint32_t* out_packet_hash4, TxtTxDebugInfo* out_dbg) {
  (void)attempt;
  if (!_backend) return MSG_SEND_FAILED;
  uint32_t fp = 0;
  const int r = _backend->sendDirect(recipient, timestamp, text, expected_ack, est_timeout, fp);
  if (r == MSG_SEND_SENT_FLOOD) _n_sent_flood++; else if (r == MSG_SEND_SENT_DIRECT) _n_sent_direct++;
  if (r != MSG_SEND_FAILED) {
    _last_txt_hash4 = expected_ack ^ 0x5741u;   // stands in for the packet hash: unique per send
    if (out_packet_hash4) *out_packet_hash4 = _last_txt_hash4;
    if (out_dbg) { *out_dbg = TxtTxDebugInfo{}; out_dbg->uniq_ts = timestamp; out_dbg->packet_hash4 = _last_txt_hash4; }
    if (fp && r == MSG_SEND_SENT_FLOOD) trackSentFp(fp);
  }
  return r;
}
bool MyMesh::sendGroupMessage(uint32_t timestamp, mesh::GroupChannel& channel, const char* sender_name,
                              const char* text, int text_len) {
  if (!_backend) return false;
  const int idx = channelIdxOf(channel);
  if (idx < 0) return false;
  char body[MAX_TEXT_LEN + 1];
  int n = text_len < (int)sizeof(body) - 1 ? text_len : (int)sizeof(body) - 1;
  memcpy(body, text, n); body[n] = 0;
  uint32_t fp = 0;
  const bool ok = _backend->sendChannel(idx, _channels[idx], timestamp, sender_name, body, fp);
  if (ok) {
    _n_sent_flood++;
    if (fp) trackSentFp(fp);
  }
  return ok;
}
bool MyMesh::sendAdvert(bool flood) {
  if (!_backend) return false;
  const bool ok = _backend->sendAdvert(flood);
  if (ok) { if (flood) _n_sent_flood++; else _n_sent_direct++; }
  return ok;
}
void MyMesh::uiRegisterExpectedAck(uint32_t expected_ack, const uint8_t pub_key[32]) {
  (void)expected_ack; (void)pub_key;   // the backend matches its own ACKs (beAck)
}
uint8_t MyMesh::uiRepeatsForFp(uint32_t fp) const {
  if (fp == 0) return 0;
  for (int i = 0; i < UI_ECHO_SLOTS; i++) if (_echo_fp[i] == fp) return _echo_rep[i];
  return 0;
}
uint8_t MyMesh::uiRepeatHopCount(uint32_t fp) const {
  if (fp == 0) return 0;
  for (int i = 0; i < UI_ECHO_SLOTS; i++) if (_echo_fp[i] == fp) return _echo_hop_n[i];
  return 0;
}
uint8_t MyMesh::uiRepeatHop(uint32_t fp, uint8_t idx, uint8_t* out, uint8_t out_sz) const {
  if (fp == 0 || !out) return 0;
  for (int i = 0; i < UI_ECHO_SLOTS; i++) if (_echo_fp[i] == fp) {
    if (idx >= _echo_hop_n[i]) return 0;
    uint8_t sz = _echo_hop_sz[i]; if (sz > out_sz) sz = out_sz; if (sz > 4) sz = 4;
    memcpy(out, _echo_hop[i][idx], sz);
    return sz;
  }
  return 0;
}
// src/MyMesh.h uiTrackSentFp.
void MyMesh::trackSentFp(uint32_t fp) {
  if (fp == 0) return;
  _last_sent_fp = fp;
  for (int i = 0; i < UI_ECHO_SLOTS; i++) if (_echo_fp[i] == fp) { _echo_rep[i] = 0; _echo_hop_n[i] = 0; return; }
  _echo_fp[_echo_idx] = fp; _echo_rep[_echo_idx] = 0; _echo_hop_n[_echo_idx] = 0;
  _echo_idx = (uint8_t)((_echo_idx + 1) % UI_ECHO_SLOTS);
}

// ---- UI requests ------------------------------------------------------------------------
int MyMesh::sendTelemetryRequestWithGuestLoginForUI(ContactInfo& recipient) {
  return uiSendRequestAfterGuestLogin(recipient, UiReqKind::Telemetry);
}
int MyMesh::uiSendRequestAfterGuestLogin(ContactInfo& recipient, UiReqKind kind) {
  if (!_backend || hasUIRequestPending()) return MSG_SEND_FAILED;
  const int r = (kind == UiReqKind::Telemetry) ? _backend->requestTelemetry(recipient)
                                               : _backend->requestStatus(recipient);
  if (r == MSG_SEND_SENT_FLOOD || r == MSG_SEND_SENT_DIRECT) {
    _ui_pending_kind = kind;
    memcpy(_ui_pending_key, recipient.id.pub_key, PUB_KEY_SIZE);
  }
  return r;
}
int MyMesh::uiSendAdminLogin(ContactInfo& recipient, const char* password) {
  if (!_backend) return MSG_SEND_FAILED;
  return _backend->login(recipient, password ? password : "");
}
int MyMesh::uiSendAdminCommand(ContactInfo& recipient, const char* text) {
  if (!_backend || !text || !text[0]) return MSG_SEND_FAILED;
  return _backend->sendCli(recipient, text);
}
int MyMesh::uiRoomRelogin(const uint8_t pub_key[32]) {
  ContactInfo* c = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!c || c->type != ADV_TYPE_ROOM) return MSG_SEND_FAILED;
  return uiSendAdminLogin(*c, "");
}
int MyMesh::sendRegionsRequestForUI(const uint8_t pub_key[PUB_KEY_SIZE], uint32_t& est_timeout) {
  est_timeout = 0;
  if (!_backend || !pub_key || hasUIRequestPending()) return MSG_SEND_FAILED;
  const int r = _backend->requestRegions(pub_key, est_timeout);
  if (r == MSG_SEND_SENT_FLOOD || r == MSG_SEND_SENT_DIRECT) {
    _ui_pending_kind = UiReqKind::Regions;
    memcpy(_ui_pending_key, pub_key, PUB_KEY_SIZE);
  }
  return r;
}
uint32_t MyMesh::uiSendTracePing(const uint8_t pub_key[32]) {
  if (!_backend || !pub_key) return 0;
  _ui_trace_tag = _backend->tracePing(pub_key);
  return _ui_trace_tag;
}
uint32_t MyMesh::uiSendTraceRoute(const ContactInfo& c) {
  if (!_backend) return 0;
  _ui_trace_tag = _backend->traceRoute(c);
  return _ui_trace_tag;
}

// ---- terminal -----------------------------------------------------------------------------
void MyMesh::setTerminalSink(void (*cb)(const char* line)) { s_terminal_sink = cb; }
void MyMesh::runLocalCli(const char* cmd) {
  if (!s_terminal_sink || !cmd) return;
  if (_backend && _backend->runNodeCli(cmd)) return;   // a companion node runs it (beTerminalLine)
  char line[160];
  if (!strcmp(cmd, "ver") || !strcmp(cmd, "version")) {
    snprintf(line, sizeof line, "%s (%s), web build, backend %s", FIRMWARE_VERSION, FIRMWARE_BUILD_DATE,
             _backend ? _backend->name() : "none");
  } else if (!strcmp(cmd, "name")) {
    snprintf(line, sizeof line, "name: %s", _prefs.node_name);
  } else if (!strcmp(cmd, "contacts")) {
    snprintf(line, sizeof line, "contacts: %d", _num_contacts);
  } else {
    snprintf(line, sizeof line, "'%s' is not available in the web build", cmd);
  }
  s_terminal_sink(line);
}

// ---- events from the backend (same UI callback order as src/MyMesh.cpp) -------------------
void MyMesh::beContact(const ContactInfo& c, bool heard_advert, uint8_t path_len) {
  ContactInfo* slot = lookupContactByPubKey(c.id.pub_key, PUB_KEY_SIZE);
  const bool is_new = (slot == nullptr);
  if (slot) *slot = c; else addContact(c);
  if (heard_advert) {
    noteHeard(c);
    if (path_len == 0 || path_len == OUT_PATH_UNKNOWN) _n_recv_direct++; else _n_recv_flood++;
    if (_ui) _ui->notify(UIEventType::newContactMessage);
    if (_ui) _ui->discoveredContact(c, false /* it is in contacts[] now */, path_len);
  } else if (is_new && _ui) {
    _ui->onThreadsChanged();
  }
}
void MyMesh::beDiscovered(const ContactInfo& c, uint8_t path_len) {
  noteHeard(c);
  _n_recv_flood++;
  if (_ui) _ui->notify(UIEventType::newContactMessage);
  if (_ui) _ui->discoveredContact(c, true /* not in contacts[] */, path_len);
}
void MyMesh::beOutgoingDirect(const uint8_t pub_key[32], const char* text, uint32_t expected_ack) {
  ContactInfo* to = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (to && _ui) _ui->appSentMsgToContact(to->id.pub_key, to->name, text, expected_ack);
}
void MyMesh::beOutgoingChannel(int channel_idx, const char* text) {
  if (channel_idx < 0 || channel_idx >= MAX_GROUP_CHANNELS || !_channels[channel_idx].name[0] || !_ui) return;
  _ui->appSentMsgToChannel(_channels[channel_idx].name, text);
}
void MyMesh::beChannel(int idx, const ChannelDetails& ch) {
  if (idx < 0 || idx >= MAX_GROUP_CHANNELS) return;
  _channels[idx] = ch;
  channelHash(_channels[idx]);
  if (_ui) _ui->onThreadsChanged();
}
void MyMesh::beDirectMessage(const uint8_t pub_key[32], uint32_t sender_ts, const char* text,
                             uint8_t path_len, bool flood, int8_t snr_q4, int8_t rssi) {
  ContactInfo* from = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!from || !_ui) return;
  if (flood) _n_recv_flood++; else _n_recv_direct++;
  beSignal(snr_q4, rssi);
  stashRxMeta(false, from->id.pub_key, flood ? path_len : 0xFF);
  _ui->notify(UIEventType::contactMessage);
  _last_sender_ts = sender_ts;
  _ui->newMsgFromPubWithMeta(flood ? path_len : 0xFF, flood, from->id.pub_key, from->name, text,
                             ++_msgcount, snr_q4, rssi);
}
void MyMesh::beChannelMessage(int channel_idx, uint32_t sender_ts, const char* text,
                              uint8_t path_len, int8_t snr_q4, int8_t rssi) {
  if (!_ui) return;
  const char* channel_name = "Unknown channel";
  if (channel_idx >= 0 && channel_idx < MAX_GROUP_CHANNELS && _channels[channel_idx].name[0])
    channel_name = _channels[channel_idx].name;
  _n_recv_flood++;
  beSignal(snr_q4, rssi);
  stashRxMeta(true, (channel_idx >= 0 && channel_idx < MAX_GROUP_CHANNELS) ? _channels[channel_idx].channel.hash : nullptr,
              path_len);
  _ui->notify(UIEventType::channelMessage);
  _last_sender_ts = sender_ts;
  _ui->newMsgFromPubWithMeta(path_len, true, nullptr, channel_name, text, ++_msgcount, snr_q4, rssi);
}
void MyMesh::beRoomMessage(const uint8_t room_pub_key[32], const char* author, uint32_t sender_ts,
                           const char* text, uint8_t path_len, int8_t snr_q4, int8_t rssi) {
  ContactInfo* room = lookupContactByPubKey(room_pub_key, PUB_KEY_SIZE);
  if (!room || !_ui) return;
  _n_recv_flood++;
  beSignal(snr_q4, rssi);
  stashRxMeta(false, room->id.pub_key, path_len);
  _ui->notify(UIEventType::roomMessage);
  _last_sender_ts = sender_ts;
  _ui->newRoomMsgFromPubWithMeta(path_len, true, room->id.pub_key, room->name, author, text,
                                 ++_msgcount, snr_q4, rssi);
}
void MyMesh::beAck(uint32_t expected_ack) { if (_ui) _ui->onMessageAcked(expected_ack); }
// src/MyMesh.h uiCountEcho: count one echo, remember which repeater re-flooded it.
void MyMesh::beRepeatHeard(uint32_t sent_fp, const uint8_t* hop, uint8_t hop_sz) {
  for (int i = 0; i < UI_ECHO_SLOTS; i++) {
    if (_echo_fp[i] != sent_fp) continue;
    if (_echo_rep[i] < 255) _echo_rep[i]++;
    _echo_dirty = true;
    if (hop && hop_sz > 0 && hop_sz <= 4 && _echo_hop_n[i] < ECHO_MAX_HOPS) {
      bool seen = false;
      for (uint8_t k = 0; k < _echo_hop_n[i]; k++)
        if (_echo_hop_sz[i] == hop_sz && memcmp(_echo_hop[i][k], hop, hop_sz) == 0) { seen = true; break; }
      if (!seen) {
        memcpy(_echo_hop[i][_echo_hop_n[i]], hop, hop_sz);
        _echo_hop_sz[i] = hop_sz;
        _echo_hop_n[i]++;
      }
    }
    return;
  }
}
void MyMesh::beStatusReply(const uint8_t pub_key[32], const uint8_t* data, size_t len) {
  ContactInfo* c = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (_ui_pending_kind == UiReqKind::None || memcmp(_ui_pending_key, pub_key, PUB_KEY_SIZE) != 0) return;
  _ui_pending_kind = UiReqKind::None;
  if (c && _ui) _ui->onPingReply(*c, data, len);
}
void MyMesh::beTelemetryReply(const uint8_t pub_key[32], const uint8_t* lpp, size_t len) {
  ContactInfo* c = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (_ui_pending_kind == UiReqKind::None || memcmp(_ui_pending_key, pub_key, PUB_KEY_SIZE) != 0) return;
  _ui_pending_kind = UiReqKind::None;
  if (c && _ui) _ui->onTelemetryReply(*c, lpp, len);
}
void MyMesh::beTraceResult(uint32_t tag, int8_t their_snr, int8_t our_snr,
                           uint8_t extra_hops, const int8_t* extra_snrs) {
  if (!_ui || tag == 0 || tag != _ui_trace_tag) return;
  _ui_trace_tag = 0;
  _ui->onTracePingResult(tag, their_snr, our_snr, extra_snrs ? extra_hops : 0, extra_snrs);
}
void MyMesh::beSignal(int8_t snr_q4, int8_t rssi) {
  _ui_sig_snr_q4 = snr_q4;
  _ui_sig_rssi = rssi;
  _ui_sig_ms = millis();
  if (_ui_sig_ms == 0) _ui_sig_ms = 1;
}

// ---- events only a link to a real node produces ----------------------------------------
void MyMesh::beSelfInfo(const NodePrefs& np, const uint8_t pub_key[32], int32_t lat_e6, int32_t lon_e6) {
  // The node owns these; everything else in NodePrefs is this UI's own.
  StrHelper::strncpy(_prefs.node_name, np.node_name, sizeof(_prefs.node_name));
  _prefs.freq = np.freq;
  _prefs.bw = np.bw;
  _prefs.sf = np.sf;
  _prefs.cr = np.cr;
  _prefs.tx_power_dbm = np.tx_power_dbm;
  _prefs.multi_acks = np.multi_acks;
  _prefs.manual_add_contacts = np.manual_add_contacts;
  _prefs.advert_loc_policy = np.advert_loc_policy;
  _prefs.telemetry_mode_base = np.telemetry_mode_base;
  _prefs.telemetry_mode_loc = np.telemetry_mode_loc;
  _prefs.telemetry_mode_env = np.telemetry_mode_env;
  _prefs.client_repeat = np.client_repeat;
  _prefs.path_hash_mode = np.path_hash_mode;
  _prefs.ble_pin = np.ble_pin;
  memcpy(self_id.pub_key, pub_key, PUB_KEY_SIZE);
  _node_pos_lat = (double)lat_e6 / 1.0e6;
  _node_pos_lon = (double)lon_e6 / 1.0e6;
  // The node's position is the UI's, unless this computer's GPS has one (it wins, as a
  // device's fix wins over its stored position; syncAdvertPosition decides if it is sent).
  LocationProvider* lp = sensors.getLocationProvider();
  const bool own_fix = _prefs.gps_enabled && lp && lp->isValid();
  if ((lat_e6 != 0 || lon_e6 != 0) && !own_fix) {
    sensors.node_lat = _node_pos_lat;
    sensors.node_lon = _node_pos_lon;
  }
  // Keep the local copy in step without echoing it back to the node.
  File f = SPIFFS.open(k_prefs_path, "w");
  if (f) { f.write((const uint8_t*)&_prefs, sizeof _prefs); f.close(); }
  if (_ui) _ui->onThreadsChanged();
}

void MyMesh::beContactRemoved(const uint8_t pub_key[32]) {
  if (removeAt(_contacts, _num_contacts, pub_key) && _ui) _ui->onThreadsChanged();
}

// src/MyMesh.cpp logRxRaw, on the node's copy of the frame: the RF monitor ring, the
// live signal (0-hop only), advert routes, and "repeats heard" for our own floods.
void MyMesh::beRxRaw(int8_t snr_q4, int8_t rssi, const uint8_t* raw, size_t len) {
  const RawView v = rawView(raw, len);
  if (!v.ok) return;
  const uint32_t now_ms = millis();
  _last_rx_snr_q4 = snr_q4;
  _last_rx_rssi = rssi;
  uint8_t org_kind = 0, org[4] = {0, 0, 0, 0};
  const int pl = v.payload_off;
  switch (v.ptype) {
    case PAYLOAD_TYPE_ADVERT:
      if ((size_t)pl + 32 <= len) { org_kind = 1; memcpy(org, &raw[pl], 4); }
      break;
    case PAYLOAD_TYPE_REQ: case PAYLOAD_TYPE_RESPONSE: case PAYLOAD_TYPE_TXT_MSG: case PAYLOAD_TYPE_PATH:
      if ((size_t)pl + 2 <= len) { org_kind = 2; org[0] = raw[pl]; org[1] = raw[pl + 1]; }
      break;
    default: break;
  }
  if (v.hops == 0) beSignal(snr_q4, rssi);
  UiRxRec& r = _rxlog[_rxlog_head];
  r.ms = now_ms; r.rssi = rssi; r.snr_q4 = snr_q4; r.ptype = v.ptype; r.route = v.route; r.hops = v.hops;
  r.len = (uint8_t)(len > 255 ? 255 : len);
  r.org_kind = org_kind;
  memcpy(r.org, org, 4);
  _rxlog_head = (uint8_t)((_rxlog_head + 1) % UI_RXLOG_MAX);
  if (_rxlog_cnt < UI_RXLOG_MAX) _rxlog_cnt++;

  // Advert routes (getAdvertPath).
  if (v.ptype == PAYLOAD_TYPE_ADVERT && org_kind == 1) {
    int slot = -1, oldest = 0;
    for (int i = 0; i < ADVERT_PATHS; i++) {
      if (_advert_paths[i].used && memcmp(_advert_paths[i].pubkey_prefix, &raw[pl], sizeof(AdvertPath::pubkey_prefix)) == 0) { slot = i; break; }
      if (!_advert_paths[i].used) { if (slot < 0) slot = i; continue; }
      if (_advert_paths[i].recv_seq < _advert_paths[oldest].recv_seq) oldest = i;
    }
    if (slot < 0) slot = oldest;
    AdvertPath& a = _advert_paths[slot];
    memcpy(a.pubkey_prefix, &raw[pl], sizeof(a.pubkey_prefix));
    const size_t bytes = (size_t)v.hops * v.hash_sz;
    a.path_len = (uint8_t)(raw[v.path_off - 1]);
    if (bytes <= sizeof(a.path)) memcpy(a.path, &raw[v.path_off], bytes);
    else a.path_len = 0;
    a.recv_timestamp = getRTCClock()->getCurrentTime();
    a.recv_seq = ++_advert_seq;
    a.used = true;
  }

  // A message packet: kept for the Info popup of the message event that follows it.
  if ((v.ptype == PAYLOAD_TYPE_TXT_MSG || v.ptype == PAYLOAD_TYPE_GRP_TXT) && len <= sizeof(_last_msg_raw)) {
    memcpy(_last_msg_raw, raw, len);
    _last_msg_raw_len = (uint16_t)len;
    _last_msg_raw_ms = now_ms ? now_ms : 1;
  }

  // Repeats heard: our own flood coming back (same payload, longer path).
  if (isMsgFloodType(v.ptype) && (size_t)pl < len) {
    const uint32_t fp = fnv1a32(raw + pl, (int)(len - pl));
    const uint8_t* lasthop = v.hops >= 1 ? raw + pl - v.hash_sz : nullptr;
    beRepeatHeard(fp, lasthop, lasthop ? v.hash_sz : 0);
  }
}

// src/MyMesh.h uiStashRxMeta: route + scope of the packet that carried the message the
// UI is about to be told about. Over a companion link that is the raw RX push that came
// just before it, when it is plausibly the same packet (type, sender/channel hash, hops).
void MyMesh::stashRxMeta(bool for_channel, const uint8_t* key_or_hash, uint8_t path_len) {
  _last_rx_path_n = 0;
  _last_rx_has_scope = false;
  _last_rx_scope = 0;
  _last_rx_scope_slot = REGION_SLOT_NONE;
  if (!_last_msg_raw_ms || !key_or_hash || (uint32_t)(millis() - _last_msg_raw_ms) > 3000) return;
  const RawView v = rawView(_last_msg_raw, _last_msg_raw_len);
  _last_msg_raw_ms = 0;
  if (!v.ok || (size_t)v.payload_off + 2 > _last_msg_raw_len) return;
  if (for_channel) {
    if (v.ptype != PAYLOAD_TYPE_GRP_TXT || _last_msg_raw[v.payload_off] != key_or_hash[0]) return;
  } else {
    if (v.ptype != PAYLOAD_TYPE_TXT_MSG || _last_msg_raw[v.payload_off + 1] != key_or_hash[0]) return;
  }
  const bool flood = (v.route == ROUTE_TYPE_FLOOD || v.route == ROUTE_TYPE_TRANSPORT_FLOOD);
  if (flood && path_len != 0xFF && (path_len & 0x3F) != v.hops) return;
  if (flood) {
    int nb = (int)v.hops * (int)v.hash_sz;
    if (nb > (int)sizeof(_last_rx_path)) nb = (int)sizeof(_last_rx_path);
    if (nb > 0) { memcpy(_last_rx_path, &_last_msg_raw[v.path_off], (size_t)nb); _last_rx_path_n = (uint8_t)nb; }
  }
  if (v.route == ROUTE_TYPE_TRANSPORT_FLOOD || v.route == ROUTE_TYPE_TRANSPORT_DIRECT) {
    mesh::Packet pkt;
    if (pkt.readFrom(_last_msg_raw, (uint8_t)_last_msg_raw_len)) {
      _last_rx_has_scope = true;
      _last_rx_scope = pkt.transport_codes[0] ? pkt.transport_codes[0] : pkt.transport_codes[1];
      _last_rx_scope_slot = pkt.transport_codes[0] ? _region_reg.matchPacket(&pkt, pkt.transport_codes[0])
                                                   : REGION_SLOT_NONE;
    }
  }
}

void MyMesh::beLoginResult(const uint8_t pub_key[32], bool ok, uint8_t perms, uint32_t server_clock) {
  ContactInfo* c = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!c || !_ui) return;
  if (ok) _ui->onServerClock(*c, server_clock);
  _ui->onAdminLoginResult(*c, ok, perms);
}

void MyMesh::beCliReply(const uint8_t pub_key[32], const char* text) {
  ContactInfo* c = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (c && _ui) _ui->onAdminCommandReply(*c, text);
}

void MyMesh::beRegionsReply(const uint8_t pub_key[32], const uint8_t* data, size_t len) {
  if (_ui_pending_kind != UiReqKind::Regions || memcmp(_ui_pending_key, pub_key, PUB_KEY_SIZE) != 0) return;
  _ui_pending_kind = UiReqKind::None;
  ContactInfo* c = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (c && _ui) _ui->onRegionListReply(*c, data, len);
}

void MyMesh::beLinkState(bool up, const char* detail) {
  char line[160];
  snprintf(line, sizeof line, "%s: %s", up ? "Connected" : "Disconnected", detail ? detail : "");
  Serial.printf("[web] companion %s\n", line);
  if (s_terminal_sink) s_terminal_sink(line);
  if (_ui) _ui->onThreadsChanged();
}

void MyMesh::beTerminalLine(const char* text) {
  if (s_terminal_sink && text) s_terminal_sink(text);
}
