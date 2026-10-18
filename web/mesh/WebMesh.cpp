// SPDX-License-Identifier: GPL-3.0-or-later
// The MyMesh stand-in (see WebMesh.h). The table semantics below mirror BaseChatMesh /
// src/MyMesh.h one for one (same return values, same UI callbacks in the same order),
// so the UI cannot tell this mirror from a device's. Network work goes to the backend.
#include "WebMesh.h"
#include <SPIFFS.h>
#include <Utils.h>
#include <helpers/TxtDataHelpers.h>

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
}

void MyMesh::loop() {
  if (_backend) _backend->loop(millis());
}

bool MyMesh::setBLEPin(uint32_t pin) { (void)pin; return false; }

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
  return true;
}
bool MyMesh::uiShareContact(const uint8_t pub_key[32]) { (void)pub_key; return false; }
bool MyMesh::uiSetContactGps(const uint8_t pub_key[32], int32_t lat_e6, int32_t lon_e6) {
  ContactInfo* slot = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!slot) return false;
  slot->gps_lat = lat_e6;
  slot->gps_lon = lon_e6;
  slot->lastmod = getRTCClock()->getCurrentTime();
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
  slot->flags = on ? (uint8_t)(slot->flags | bit) : (uint8_t)(slot->flags & ~bit);
  return true;
}
bool MyMesh::uiSetContactFavorite(const uint8_t pub_key[32], bool fav) {
  ContactInfo* slot = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
  if (!slot) return false;
  slot->flags = fav ? (uint8_t)(slot->flags | 0x01) : (uint8_t)(slot->flags & ~0x01);
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
  (void)pubkey_prefix; (void)prefix_len; (void)path_len; (void)path; (void)path_cap;
  return false;   // advert routes are not reported by the backend: "unknown"
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
    if (fp && r == MSG_SEND_SENT_FLOOD) {
      _last_sent_fp = fp;
      _echo_fp[_echo_idx] = fp; _echo_rep[_echo_idx] = 0;
      _echo_idx = (uint8_t)((_echo_idx + 1) % UI_ECHO_SLOTS);
    }
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
    if (fp) {
      _last_sent_fp = fp;
      _echo_fp[_echo_idx] = fp; _echo_rep[_echo_idx] = 0;
      _echo_idx = (uint8_t)((_echo_idx + 1) % UI_ECHO_SLOTS);
    }
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
int MyMesh::uiSendAdminLogin(ContactInfo& recipient, const char* password) { (void)recipient; (void)password; return MSG_SEND_FAILED; }
int MyMesh::uiSendAdminCommand(ContactInfo& recipient, const char* text) { (void)recipient; (void)text; return MSG_SEND_FAILED; }
int MyMesh::uiRoomRelogin(const uint8_t pub_key[32]) { (void)pub_key; return MSG_SEND_FAILED; }
int MyMesh::sendRegionsRequestForUI(const uint8_t pub_key[PUB_KEY_SIZE], uint32_t& est_timeout) {
  (void)pub_key; est_timeout = 0; return MSG_SEND_FAILED;
}
uint32_t MyMesh::uiSendTracePing(const uint8_t pub_key[32]) {
  if (!_backend || !pub_key) return 0;
  _ui_trace_tag = _backend->tracePing(pub_key);
  return _ui_trace_tag;
}
uint32_t MyMesh::uiSendTraceRoute(const ContactInfo& c) { return uiSendTracePing(c.id.pub_key); }

// ---- terminal -----------------------------------------------------------------------------
void MyMesh::setTerminalSink(void (*cb)(const char* line)) { s_terminal_sink = cb; }
void MyMesh::runLocalCli(const char* cmd) {
  if (!s_terminal_sink || !cmd) return;
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
  _last_rx_path_n = 0;
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
  _last_rx_path_n = 0;
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
  _last_rx_path_n = 0;
  _ui->notify(UIEventType::roomMessage);
  _last_sender_ts = sender_ts;
  _ui->newRoomMsgFromPubWithMeta(path_len, true, room->id.pub_key, room->name, author, text,
                                 ++_msgcount, snr_q4, rssi);
}
void MyMesh::beAck(uint32_t expected_ack) { if (_ui) _ui->onMessageAcked(expected_ack); }
void MyMesh::beRepeatHeard(uint32_t sent_fp) {
  for (int i = 0; i < UI_ECHO_SLOTS; i++)
    if (_echo_fp[i] == sent_fp) { if (_echo_rep[i] < 255) _echo_rep[i]++; _echo_dirty = true; return; }
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
void MyMesh::beTraceResult(uint32_t tag, int8_t their_snr, int8_t our_snr) {
  if (!_ui || tag == 0 || tag != _ui_trace_tag) return;
  _ui_trace_tag = 0;
  _ui->onTracePingResult(tag, their_snr, our_snr, 0, nullptr);
}
void MyMesh::beSignal(int8_t snr_q4, int8_t rssi) {
  _ui_sig_snr_q4 = snr_q4;
  _ui_sig_rssi = rssi;
  _ui_sig_ms = millis();
  if (_ui_sig_ms == 0) _ui_sig_ms = 1;
}
