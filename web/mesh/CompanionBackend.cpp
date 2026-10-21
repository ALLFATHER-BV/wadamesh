// SPDX-License-Identifier: GPL-3.0-or-later
// CompanionBackend (CompanionBackend.h): MeshBackend over the MeshCore companion protocol.
//
// Shape of the link:
//   * One command in flight at a time (the protocol answers each command with one reply
//     frame, except the contact list, which streams). Pushes (codes >= 0x80) arrive any
//     time, and wadamesh also pushes message frames (16/17) live, so those are only taken
//     as the reply when CMD_SYNC_NEXT_MESSAGE is the command in flight.
//   * On link up: DEVICE_QUERY, APP_START, the device clock, every contact, every
//     channel slot, then the waiting messages; after that MSG_WAITING drives the sync.
//   * Calls from the UI return at once with what a device's MyMesh would return; the
//     node's answer is matched later (RESP_SENT's ack/tag, the PUSH_* replies) and
//     delivered as MeshBackendEvents, the way a radio reply reaches a device.
#include "CompanionBackend.h"
#include "CompanionTransport.h"
#include "MeshBackend.h"
#include "BackendSelect.h"
#include <Arduino.h>
#include <esp_random.h>
#include <Utils.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/TxtDataHelpers.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace companion;

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
// The browser's own sync id, kept in localStorage next to the page's other settings. A
// browser that already synced under the old shared id keeps it (its cursor on the node
// is there, so nothing is delivered twice); new ones get "wadaweb-" + 8 random characters.
EM_JS(int, wada_js_client_id, (char* out, int cap), {
  var id = null;
  try { id = localStorage.getItem('wadamesh.syncId'); } catch (e) {}
  if (!id || !/^[A-Za-z0-9-]{1,31}$/.test(id)) {
    var legacy = false;
    try { legacy = !!localStorage.getItem('wadamesh.nodes'); } catch (e) {}
    if (legacy) id = 'wadamesh-web';
    else {
      var a = new Uint8Array(8), abc = 'abcdefghijklmnopqrstuvwxyz0123456789';
      crypto.getRandomValues(a);
      id = 'wadaweb-';
      for (var i = 0; i < 8; i++) id += abc[a[i] % 36];
    }
    try { localStorage.setItem('wadamesh.syncId', id); } catch (e) {}
  }
  stringToUTF8(id, out, cap);
  return 1;
});
#endif

const char* companion::clientId() {
  static char id[32] = "";
#if defined(__EMSCRIPTEN__)
  if (!id[0]) wada_js_client_id(id, sizeof id);
#endif
  if (!id[0]) strcpy(id, "wadamesh-web");
  return id;
}

namespace {

typedef std::array<uint8_t, PUB_KEY_SIZE> Key;
Key toKey(const uint8_t* p) { Key k; memcpy(k.data(), p, PUB_KEY_SIZE); return k; }

uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
void wr32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
uint32_t fnv1a32(const uint8_t* d, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
  return h ? h : 1;
}
// Tags the UI stores with a message (the ack it waits for) outlive the session in the
// chat history, so they come from the OS CSPRNG, not a per-run sequence.
uint32_t randomTag() {
  uint32_t t = 0;
  while (!t) t = esp_random();
  return t;
}
std::string hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}

// A contact frame (RESP_CONTACT / PUSH_NEW_ADVERT): code, pub[32], type, flags,
// out_path_len, out_path[64], name[32], last_advert, lat, lon, lastmod (148 bytes).
const size_t CONTACT_FRAME_LEN = 1 + 32 + 3 + 64 + 32 + 16;
bool parseContact(const uint8_t* f, size_t len, ContactInfo& c) {
  if (len < CONTACT_FRAME_LEN - 4) return false;   // lastmod is the last, optional on old nodes
  c = ContactInfo{};
  memcpy(c.id.pub_key, f + 1, PUB_KEY_SIZE);
  c.type = f[33];
  c.flags = f[34];
  c.out_path_len = f[35];
  memcpy(c.out_path, f + 36, MAX_PATH_SIZE);
  memcpy(c.name, f + 100, 32);
  c.name[31] = 0;
  c.last_advert_timestamp = rd32(f + 132);
  c.gps_lat = (int32_t)rd32(f + 136);
  c.gps_lon = (int32_t)rd32(f + 140);
  c.lastmod = len >= CONTACT_FRAME_LEN ? rd32(f + 144) : 0;
  return true;
}
// wadamesh appends a local "Meshcomod" command contact (pub key "MESHCM" + zeros) to
// the contact list; it is the node's own console, not a mesh contact.
bool isMeshcomodPseudo(const ContactInfo& c) {
  static const uint8_t k[6] = {0x4D, 0x45, 0x53, 0x48, 0x43, 0x4D};
  if (memcmp(c.id.pub_key, k, 6) != 0) return false;
  for (int i = 6; i < PUB_KEY_SIZE; i++) if (c.id.pub_key[i]) return false;
  return true;
}
// Commands that change the node (settings, contacts, channels, clock): what --read-only
// keeps from being sent. Messages, adverts and requests still go out.
bool changesNode(uint8_t cmd) {
  switch (cmd) {
    case CMD_SET_DEVICE_TIME: case CMD_SET_ADVERT_NAME: case CMD_ADD_UPDATE_CONTACT: case CMD_SET_RADIO_PARAMS:
    case CMD_SET_RADIO_TX_POWER: case CMD_RESET_PATH: case CMD_SET_ADVERT_LATLON: case CMD_REMOVE_CONTACT:
    case CMD_IMPORT_CONTACT: case CMD_REBOOT: case CMD_SET_TUNING_PARAMS: case CMD_SET_CHANNEL:
    case CMD_SET_DEVICE_PIN: case CMD_SET_OTHER_PARAMS: case CMD_FACTORY_RESET: case CMD_SET_PATH_HASH_MODE:
    case CMD_SET_DEFAULT_FLOOD_SCOPE:
      return true;
    default:
      return false;
  }
}
// Is `code` an answer to `cmd`? A reply that arrives after its command timed out must
// not be taken as the answer to the next one.
bool replyFits(uint8_t cmd, uint8_t code) {
  if (code == RESP_ERR || code == RESP_DISABLED) return true;
  switch (cmd) {
    case CMD_DEVICE_QUERY: return code == RESP_DEVICE_INFO;
    case CMD_APP_START: return code == RESP_SELF_INFO;
    case CMD_GET_DEVICE_TIME: return code == RESP_CURR_TIME;
    case CMD_GET_CONTACTS: return code == RESP_END_OF_CONTACTS;
    case CMD_GET_CONTACT_BY_KEY: return code == RESP_CONTACT;
    case CMD_GET_CHANNEL: return code == RESP_CHANNEL_INFO;
    case CMD_SYNC_NEXT_MESSAGE:
      return code == RESP_NO_MORE_MESSAGES || code == RESP_CONTACT_MSG_RECV || code == RESP_CHANNEL_MSG_RECV ||
             code == RESP_CONTACT_MSG_RECV_V3 || code == RESP_CHANNEL_MSG_RECV_V3;
    case CMD_SEND_TXT_MSG: case CMD_SEND_LOGIN: case CMD_SEND_STATUS_REQ: case CMD_SEND_TELEMETRY_REQ:
    case CMD_SEND_TRACE_PATH: case CMD_SEND_ANON_REQ: case CMD_SEND_BINARY_REQ:
      return code == RESP_SENT;
    case CMD_GET_BATT_AND_STORAGE: return code == RESP_BATT_AND_STORAGE;
    case CMD_RUN_CLI_COMMAND: return code == RESP_CLI_REPLY;
    case CMD_GET_STATS: return code == RESP_STATS;
    default: return code == RESP_OK;
  }
}
const char* cmdName(uint8_t c) {
  switch (c) {
    case CMD_APP_START: return "APP_START"; case CMD_SEND_TXT_MSG: return "SEND_TXT_MSG";
    case CMD_SEND_CHANNEL_TXT_MSG: return "SEND_CHANNEL_TXT_MSG"; case CMD_GET_CONTACTS: return "GET_CONTACTS";
    case CMD_GET_DEVICE_TIME: return "GET_DEVICE_TIME"; case CMD_SET_DEVICE_TIME: return "SET_DEVICE_TIME";
    case CMD_SEND_SELF_ADVERT: return "SEND_SELF_ADVERT"; case CMD_SET_ADVERT_NAME: return "SET_ADVERT_NAME";
    case CMD_ADD_UPDATE_CONTACT: return "ADD_UPDATE_CONTACT"; case CMD_SYNC_NEXT_MESSAGE: return "SYNC_NEXT_MESSAGE";
    case CMD_SET_RADIO_PARAMS: return "SET_RADIO_PARAMS"; case CMD_SET_RADIO_TX_POWER: return "SET_RADIO_TX_POWER";
    case CMD_RESET_PATH: return "RESET_PATH"; case CMD_REMOVE_CONTACT: return "REMOVE_CONTACT";
    case CMD_SHARE_CONTACT: return "SHARE_CONTACT"; case CMD_DEVICE_QUERY: return "DEVICE_QUERY";
    case CMD_SEND_LOGIN: return "SEND_LOGIN"; case CMD_SEND_STATUS_REQ: return "SEND_STATUS_REQ";
    case CMD_HAS_CONNECTION: return "HAS_CONNECTION"; case CMD_GET_CONTACT_BY_KEY: return "GET_CONTACT_BY_KEY";
    case CMD_GET_CHANNEL: return "GET_CHANNEL"; case CMD_SET_CHANNEL: return "SET_CHANNEL";
    case CMD_SEND_TRACE_PATH: return "SEND_TRACE_PATH"; case CMD_SET_OTHER_PARAMS: return "SET_OTHER_PARAMS";
    case CMD_SEND_TELEMETRY_REQ: return "SEND_TELEMETRY_REQ"; case CMD_SEND_ANON_REQ: return "SEND_ANON_REQ";
    case CMD_SEND_BINARY_REQ: return "SEND_BINARY_REQ";
    case CMD_RUN_CLI_COMMAND: return "RUN_CLI_COMMAND"; case CMD_SET_DEVICE_PIN: return "SET_DEVICE_PIN";
    default: return "CMD";
  }
}

class CompanionBackend : public MeshBackend, private CompanionTransportListener {
public:
  CompanionBackend(CompanionTransport* t, bool read_only) : _t(t), _read_only(read_only) {
    snprintf(_name, sizeof _name, "companion-%s", t ? t->name() : "none");
    _debug = getenv("WADA_COMPANION_DEBUG") && getenv("WADA_COMPANION_DEBUG")[0] == '1';
  }
  const char* name() const override { return _name; }

  void begin(MeshBackendEvents& ev, NodePrefs& prefs, uint8_t self_pub_key[32]) override {
    (void)prefs; (void)self_pub_key;   // placeholders until the node's SELF_INFO (beSelfInfo)
    _ev = &ev;
    if (!_t) return;
    _t->setListener(this);
    _t->open();
    fprintf(stderr, "[companion] %s %s%s\n", _t->name(), _t->describe(), _read_only ? " (read-only: no node settings are changed)" : "");
    linkHook(0);
  }

  void loop(uint32_t now_ms) override {
    _now = now_ms;
    if (!_t) return;
    _t->poll(now_ms);
    if (_inflight && (int32_t)(now_ms - _deadline) >= 0) {
      Cmd c = _q.front();
      _q.pop_front();
      _inflight = false;
      if (_debug || c.code != CMD_DEVICE_QUERY) fprintf(stderr, "[companion] %s: no reply\n", cmdName(c.code));
      if (c.done) c.done(nullptr, 0);
    }
    pump();
    if (_handshake_retry_at && (int32_t)(now_ms - _handshake_retry_at) >= 0) {
      _handshake_retry_at = 0;
      if (_t->isConnected() && !_ready) startHandshake();
    }
    serviceRequest(now_ms);
  }

  // ---- sending ----------------------------------------------------------------------
  int sendDirect(const ContactInfo& to, uint32_t ts, const char* text, uint32_t& expected_ack,
                 uint32_t& est_timeout_ms, uint32_t& sent_fp) override {
    if (!_ready) return MSG_SEND_FAILED;
    const ContactInfo* known = findKnown(to.id.pub_key);
    const bool flood = !known || known->out_path_len == OUT_PATH_UNKNOWN;
    expected_ack = randomTag();
    est_timeout_ms = flood ? 12000 : 6000;   // replaced by the node's estimate when RESP_SENT lands
    sent_fp = flood ? randomTag() : 0;
    std::vector<uint8_t> f = {CMD_SEND_TXT_MSG, TXT_TYPE_PLAIN, 0};
    wr32(f, ts);
    f.insert(f.end(), to.id.pub_key, to.id.pub_key + 6);
    f.insert(f.end(), text, text + strnlen(text, MAX_TEXT_LEN));
    const uint32_t local_ack = expected_ack, local_fp = sent_fp;
    const uint8_t dest0 = to.id.pub_key[0];
    send(f, [this, local_ack, local_fp, dest0](const uint8_t* r, size_t n) {
      if (!r || r[0] != RESP_SENT || n < 10) {
        fprintf(stderr, "[companion] DM not sent (%s)\n", r ? "node refused" : "no reply");
        return;
      }
      const uint32_t node_ack = rd32(r + 2);
      if (node_ack) { _acks[node_ack] = local_ack; if (_acks.size() > 64) _acks.erase(_acks.begin()); }
      if (r[1] == 1 && local_fp) {   // flood: watch for repeaters re-flooding it
        DmEcho e{dest0, local_fp, 0, _now + 30000};
        _dm_echo.push_back(e);
        if (_dm_echo.size() > 8) _dm_echo.erase(_dm_echo.begin());
      }
    });
    return flood ? MSG_SEND_SENT_FLOOD : MSG_SEND_SENT_DIRECT;
  }

  bool sendChannel(int channel_idx, const ChannelDetails& ch, uint32_t ts, const char* sender_name,
                   const char* text, uint32_t& sent_fp) override {
    (void)sender_name;   // the node signs with its own name
    if (!_ready || channel_idx < 0 || channel_idx > 255) return false;
    const size_t tlen = strnlen(text, MAX_TEXT_LEN);
    std::vector<uint8_t> f = {CMD_SEND_CHANNEL_TXT_MSG, TXT_TYPE_PLAIN, (uint8_t)channel_idx};
    wr32(f, ts);
    f.insert(f.end(), text, text + tlen);
    sent_fp = channelPayloadFp(ch, ts, text, tlen);
    send(f, [](const uint8_t* r, size_t) {
      if (!r || r[0] != RESP_OK) fprintf(stderr, "[companion] channel post not sent (%s)\n", r ? "node refused" : "no reply");
    });
    return true;
  }

  bool sendAdvert(bool flood) override {
    if (!_ready) return false;
    send({CMD_SEND_SELF_ADVERT, (uint8_t)(flood ? 1 : 0)}, nullptr);
    return true;
  }

  // ---- requests ----------------------------------------------------------------------
  int requestStatus(const ContactInfo& c) override { return startRequest(c, false); }
  int requestTelemetry(const ContactInfo& c) override { return startRequest(c, true); }

  uint32_t tracePing(const uint8_t pub_key[32]) override {
    if (!_ready) return 0;
    const uint32_t tag = randomTag();
    std::vector<uint8_t> f = {CMD_SEND_TRACE_PATH};
    wr32(f, tag);
    wr32(f, 0);         // auth code
    f.push_back(0);     // flags: 1-byte hashes, understood by every trace-capable node
    f.push_back(pub_key[0]);
    send(f, nullptr);
    return tag;
  }
  uint32_t traceRoute(const ContactInfo& c) override {
    const ContactInfo* k = findKnown(c.id.pub_key);
    if (!_ready || !k || k->out_path_len == OUT_PATH_UNKNOWN || (k->out_path_len & 0x3F) == 0) return tracePing(c.id.pub_key);
    const uint8_t hops = k->out_path_len & 0x3F, hsz = (uint8_t)((k->out_path_len >> 6) + 1);
    const uint8_t flags = hsz == 1 ? 0 : hsz == 2 ? 1 : hsz == 4 ? 2 : 0xFF;   // trace sizes are 1 << flags
    if (flags == 0xFF || (size_t)hops * hsz > MAX_PATH_SIZE) return tracePing(c.id.pub_key);
    const uint32_t tag = randomTag();
    std::vector<uint8_t> f = {CMD_SEND_TRACE_PATH};
    wr32(f, tag);
    wr32(f, 0);
    f.push_back(flags);
    f.insert(f.end(), k->out_path, k->out_path + hops * hsz);
    send(f, nullptr);
    return tag;
  }
  void cancelRequests() override { _req = Req{}; }

  int login(const ContactInfo& c, const char* password) override {
    if (!_ready) return MSG_SEND_FAILED;
    _admin_login = toKey(c.id.pub_key);
    _admin_login_pending = true;
    sendLogin(c.id.pub_key, password);
    return routeGuess(c);
  }
  int sendCli(const ContactInfo& c, const char* text) override {
    if (!_ready) return MSG_SEND_FAILED;
    std::vector<uint8_t> f = {CMD_SEND_TXT_MSG, TXT_TYPE_CLI_DATA, 0};
    wr32(f, (uint32_t)time(nullptr));   // the node replaces it with its own clock
    f.insert(f.end(), c.id.pub_key, c.id.pub_key + 6);
    f.insert(f.end(), text, text + strnlen(text, MAX_TEXT_LEN));
    send(f, nullptr);
    return routeGuess(c);
  }
  bool hasConnectionTo(const uint8_t pub_key[32]) override {
    const Key k = toKey(pub_key);
    auto it = _sessions.find(k);
    if (it == _sessions.end()) return false;
    if (_ready && (uint32_t)(_now - it->second) > 30000) {   // refresh in the background
      it->second = _now;
      std::vector<uint8_t> f = {CMD_HAS_CONNECTION};
      f.insert(f.end(), pub_key, pub_key + PUB_KEY_SIZE);
      send(f, [this, k](const uint8_t* r, size_t) { if (r && r[0] == RESP_ERR) _sessions.erase(k); });
    }
    return true;
  }
  int binaryRequest(const ContactInfo& c, const uint8_t* payload, uint8_t len) override {
    if (!_ready || !payload || !len) return MSG_SEND_FAILED;
    std::vector<uint8_t> f = {CMD_SEND_BINARY_REQ};
    f.insert(f.end(), c.id.pub_key, c.id.pub_key + PUB_KEY_SIZE);
    f.insert(f.end(), payload, payload + len);
    const Key k = toKey(c.id.pub_key);
    _bin_tag = 0;
    send(f, [this, k](const uint8_t* r, size_t n) {
      if (r && r[0] == RESP_SENT && n >= 10) { _bin_tag = rd32(r + 2); _bin_key = k; }
    });
    return c.out_path_len == OUT_PATH_UNKNOWN ? MSG_SEND_SENT_FLOOD : MSG_SEND_SENT_DIRECT;
  }
  int requestRegions(const uint8_t pub_key[32], uint32_t& est_timeout) override {
    est_timeout = 0;
    if (!_ready) return MSG_SEND_FAILED;
    std::vector<uint8_t> f = {CMD_SEND_ANON_REQ};
    f.insert(f.end(), pub_key, pub_key + PUB_KEY_SIZE);
    f.push_back(0x01);   // REGIONS
    f.push_back(0x00);   // reply zero-hop
    const Key k = toKey(pub_key);
    _regions_tag = 0;
    send(f, [this, k](const uint8_t* r, size_t n) {
      if (r && r[0] == RESP_SENT && n >= 10) { _regions_tag = rd32(r + 2); _regions_key = k; }
    });
    est_timeout = 8000;
    return MSG_SEND_SENT_DIRECT;
  }
  bool runNodeCli(const char* cmd) override {
    if (!_ready || !cmd || !cmd[0]) return false;
    // --read-only: only commands that read. Anything else could change the node.
    if (_read_only && strncmp(cmd, "get ", 4) != 0 && strcmp(cmd, "ver") != 0 && strcmp(cmd, "board") != 0 &&
        strcmp(cmd, "clock") != 0) {
      if (_ev) _ev->beTerminalLine("read-only link: only get / ver / board / clock run on the node");
      return true;
    }
    std::vector<uint8_t> f = {CMD_RUN_CLI_COMMAND};
    f.insert(f.end(), cmd, cmd + strnlen(cmd, 160));
    send(f, [this](const uint8_t* r, size_t n) {
      if (!_ev) return;
      if (r && r[0] == RESP_CLI_REPLY) {
        std::string text((const char*)r + 1, n - 1);
        size_t a = 0;   // one terminal line per reply line
        while (a < text.size()) {
          size_t b = text.find('\n', a);
          if (b == std::string::npos) b = text.size();
          if (b > a) _ev->beTerminalLine(text.substr(a, b - a).c_str());
          a = b + 1;
        }
      } else {
        _ev->beTerminalLine(r ? "this node has no CLI over the companion link (MeshCore 1.18 or later)"
                              : "the node did not answer");
      }
    });
    return true;
  }
  bool setBlePin(uint32_t pin) override {
    if (!_ready || _read_only || (pin != 0 && (pin < 100000 || pin > 999999))) return false;
    std::vector<uint8_t> f = {CMD_SET_DEVICE_PIN};
    wr32(f, pin);
    send(f, nullptr);
    _node_prefs.ble_pin = pin;
    return true;
  }
  bool shareContact(const uint8_t pub_key[32]) override {
    if (!_ready) return false;
    std::vector<uint8_t> f = {CMD_SHARE_CONTACT};
    f.insert(f.end(), pub_key, pub_key + PUB_KEY_SIZE);
    send(f, nullptr);
    return true;
  }

  // ---- configuration written back to the node -----------------------------------------
  bool ownsPrefs() const override { return _have_self; }
  bool busy() const override { return _t && _t->isConnected() && (_inflight || !_q.empty()); }
  void prefsChanged(const NodePrefs& p) override {
    if (!_have_self || !_ready) return;   // nothing to compare against yet: never write placeholders
    NodePrefs& b = _node_prefs;
    if (strncmp(p.node_name, b.node_name, sizeof b.node_name) != 0 && p.node_name[0]) {
      std::vector<uint8_t> f = {CMD_SET_ADVERT_NAME};
      f.insert(f.end(), p.node_name, p.node_name + strnlen(p.node_name, sizeof p.node_name - 1));
      send(f, nullptr);
      StrHelper::strncpy(b.node_name, p.node_name, sizeof b.node_name);
    }
    if (fabsf(p.freq - b.freq) > 0.0005f || fabsf(p.bw - b.bw) > 0.0005f || p.sf != b.sf || p.cr != b.cr) {
      std::vector<uint8_t> f = {CMD_SET_RADIO_PARAMS};
      wr32(f, (uint32_t)lroundf(p.freq * 1000.0f));
      wr32(f, (uint32_t)lroundf(p.bw * 1000.0f));
      f.push_back(p.sf);
      f.push_back(p.cr);
      if (_proto_ver >= 9) f.push_back(b.client_repeat);
      send(f, nullptr);
      b.freq = p.freq; b.bw = p.bw; b.sf = p.sf; b.cr = p.cr;
    }
    if (p.tx_power_dbm != b.tx_power_dbm) {
      send({CMD_SET_RADIO_TX_POWER, (uint8_t)p.tx_power_dbm}, nullptr);
      b.tx_power_dbm = p.tx_power_dbm;
    }
    if (p.manual_add_contacts != b.manual_add_contacts || p.advert_loc_policy != b.advert_loc_policy ||
        p.multi_acks != b.multi_acks || p.telemetry_mode_base != b.telemetry_mode_base ||
        p.telemetry_mode_loc != b.telemetry_mode_loc || p.telemetry_mode_env != b.telemetry_mode_env) {
      const uint8_t telem = (uint8_t)((p.telemetry_mode_env << 4) | (p.telemetry_mode_loc << 2) | p.telemetry_mode_base);
      send({CMD_SET_OTHER_PARAMS, p.manual_add_contacts, telem, p.advert_loc_policy, p.multi_acks}, nullptr);
      b.manual_add_contacts = p.manual_add_contacts; b.advert_loc_policy = p.advert_loc_policy;
      b.multi_acks = p.multi_acks; b.telemetry_mode_base = p.telemetry_mode_base;
      b.telemetry_mode_loc = p.telemetry_mode_loc; b.telemetry_mode_env = p.telemetry_mode_env;
    }
  }
  void channelChanged(int idx, const ChannelDetails& ch) override {
    if (!_ready || idx < 0 || idx > 255) return;
    static const uint8_t zero[16] = {0};
    if (memcmp(&ch.channel.secret[16], zero, 16) != 0) {
      fprintf(stderr, "[companion] channel %d: 256-bit keys are not in the companion protocol, not written\n", idx);
      return;
    }
    std::vector<uint8_t> f = {CMD_SET_CHANNEL, (uint8_t)idx};
    char name[32] = {0};
    StrHelper::strncpy(name, ch.name, sizeof name);
    f.insert(f.end(), (const uint8_t*)name, (const uint8_t*)name + 32);
    f.insert(f.end(), ch.channel.secret, ch.channel.secret + 16);
    send(f, nullptr);
  }
  void contactRemoved(const uint8_t pub_key[32]) override {
    _known.erase(toKey(pub_key));
    if (!_ready) return;
    std::vector<uint8_t> f = {CMD_REMOVE_CONTACT};
    f.insert(f.end(), pub_key, pub_key + PUB_KEY_SIZE);
    send(f, nullptr);
  }
  void contactChanged(const ContactInfo& c) override {
    _known[toKey(c.id.pub_key)] = c;
    if (!_ready) return;
    std::vector<uint8_t> f = {CMD_ADD_UPDATE_CONTACT};
    f.insert(f.end(), c.id.pub_key, c.id.pub_key + PUB_KEY_SIZE);
    f.push_back(c.type);
    f.push_back(c.flags);
    f.push_back(c.out_path_len);
    f.insert(f.end(), c.out_path, c.out_path + MAX_PATH_SIZE);
    char name[32] = {0};
    StrHelper::strncpy(name, c.name, sizeof name);
    f.insert(f.end(), (const uint8_t*)name, (const uint8_t*)name + 32);
    wr32(f, c.last_advert_timestamp);
    wr32(f, (uint32_t)c.gps_lat);
    wr32(f, (uint32_t)c.gps_lon);
    wr32(f, c.lastmod);
    send(f, nullptr);
  }
  // Only ever called while the node's advert location is shared (WebMesh.cpp
  // syncAdvertPosition): the node then advertises it as a phone app's position.
  void advertPositionChanged(int32_t lat_e6, int32_t lon_e6) override {
    if (!_ready) return;
    std::vector<uint8_t> f = {CMD_SET_ADVERT_LATLON};
    wr32(f, (uint32_t)lat_e6);
    wr32(f, (uint32_t)lon_e6);
    send(f, nullptr);
  }
  void contactPathReset(const uint8_t pub_key[32]) override {
    auto it = _known.find(toKey(pub_key));
    if (it != _known.end()) it->second.out_path_len = OUT_PATH_UNKNOWN;
    if (!_ready) return;
    std::vector<uint8_t> f = {CMD_RESET_PATH};
    f.insert(f.end(), pub_key, pub_key + PUB_KEY_SIZE);
    send(f, nullptr);
  }

private:
  // ---- the command queue ---------------------------------------------------------------
  struct Cmd {
    std::vector<uint8_t> frame;
    uint8_t code;
    std::function<void(const uint8_t*, size_t)> done;   // (nullptr, 0) = no reply / not sent
    uint32_t timeout_ms;
  };
  void send(const std::vector<uint8_t>& f, std::function<void(const uint8_t*, size_t)> done, uint32_t timeout_ms = 6000) {
    if (f.empty()) return;
    if (_read_only && changesNode(f[0])) {
      fprintf(stderr, "[companion] read-only: %s not sent\n", cmdName(f[0]));
      if (done) { const uint8_t err[2] = {RESP_ERR, 0}; done(err, 2); }
      return;
    }
    _q.push_back(Cmd{f, f[0], std::move(done), timeout_ms});
    pump();
  }
  void pump() {
    if (_inflight || _q.empty() || !_t || !_t->isConnected()) return;
    Cmd& c = _q.front();
    if (_debug) fprintf(stderr, "[companion] -> %s (%u bytes)\n", cmdName(c.code), (unsigned)c.frame.size());
    if (!_t->sendFrame(c.frame.data(), c.frame.size())) {
      Cmd dead = c;
      _q.pop_front();
      if (dead.done) dead.done(nullptr, 0);
      return;
    }
    _inflight = true;
    _deadline = _now + c.timeout_ms;
  }
  void complete(const uint8_t* f, size_t n) {
    Cmd c = _q.front();
    _q.pop_front();
    _inflight = false;
    if (c.done) c.done(f, n);
    pump();
  }
  void failAll() {
    std::deque<Cmd> q;
    q.swap(_q);
    _inflight = false;
    for (Cmd& c : q) if (c.done) c.done(nullptr, 0);
  }

  // ---- transport events -------------------------------------------------------------
  void onTransportConnected() override {
    _link_up = true;
    _ready = false;
    startHandshake();
  }
  void onTransportDisconnected(const char* why) override {
    _link_up = false;
    _ready = false;
    _syncing = false;
    failAll();
    if (_ev) _ev->beLinkState(false, why);
    linkHook(0);
  }
  void onTransportFrame(const uint8_t* f, size_t n) override {
    if (!n) return;
    const uint8_t code = f[0];
    if (_debug && code != PUSH_LOG_RX_DATA) fprintf(stderr, "[companion] <- 0x%02x (%u bytes)\n", code, (unsigned)n);
    if (code >= 0x80) { onPush(f, n); return; }
    const bool msg = code == RESP_CONTACT_MSG_RECV || code == RESP_CONTACT_MSG_RECV_V3 ||
                     code == RESP_CHANNEL_MSG_RECV || code == RESP_CHANNEL_MSG_RECV_V3;
    const uint8_t want = _inflight ? _q.front().code : 0;
    if (msg && want != CMD_SYNC_NEXT_MESSAGE) { onMessageFrame(f, n); return; }   // a live push (wadamesh)
    if (!_inflight) {
      if (_debug) fprintf(stderr, "[companion] stray reply 0x%02x\n", code);
      return;
    }
    if (want == CMD_GET_CONTACTS && (code == RESP_CONTACTS_START || code == RESP_CONTACT)) {
      _deadline = _now + 6000;   // the stream is alive
      if (code == RESP_CONTACT) onContactListEntry(f, n);
      return;
    }
    if (!replyFits(want, code)) {
      if (_debug) fprintf(stderr, "[companion] late reply 0x%02x ignored (waiting on %s)\n", code, cmdName(want));
      return;
    }
    complete(f, n);
  }

  // ---- handshake + initial sync -----------------------------------------------------------
  void startHandshake() {
    _ready = false;
    _handshake_retry_at = 0;
    // A node that just reset (port open, USB re-enumeration) takes a few seconds to
    // answer; DEVICE_QUERY is retried until it does.
    // Every step checks the link is still up: a link lost mid-handshake fails the queued
    // commands (no reply), which must not walk the chain on to "ready".
    send({CMD_DEVICE_QUERY, APP_TARGET_VER}, [this](const uint8_t* r, size_t n) {
      if (!_link_up) return;
      if (!r || r[0] != RESP_DEVICE_INFO || n < 4) { _handshake_retry_at = _now + 1500; return; }
      onDeviceInfo(r, n);
      const char* cid = clientId();
      std::vector<uint8_t> f = {CMD_APP_START, (uint8_t)strlen(cid)};
      f.insert(f.end(), cid, cid + strlen(cid));
      while (f.size() < 8) f.push_back(0);
      f.insert(f.end(), APP_NAME, APP_NAME + strlen(APP_NAME));
      send(f, [this](const uint8_t* r2, size_t n2) {
        if (!_link_up) return;
        if (!r2 || r2[0] != RESP_SELF_INFO || n2 < 58) { _handshake_retry_at = _now + 1500; return; }
        onSelfInfo(r2, n2);
        send({CMD_GET_DEVICE_TIME}, [this](const uint8_t* r3, size_t n3) { onDeviceTime(r3, n3); });
        fetchContacts();
        fetchChannels();
        send({CMD_GET_BATT_AND_STORAGE}, [this](const uint8_t* r4, size_t n4) {
          if (!_link_up) return;
          if (r4 && r4[0] == RESP_BATT_AND_STORAGE && n4 >= 3) _batt_mv = (uint16_t)(r4[1] | (r4[2] << 8));
          // Everything above is in: the link is ready, then the waiting messages.
          _ready = true;
          char line[160];
          snprintf(line, sizeof line, "%s over %s %s, %s, %d contacts, %d channels%s", _node_prefs.node_name,
                   _t->name(), _t->describe(), _fw_version, (int)_known.size(), _n_channels,
                   _read_only ? ", read-only" : "");
          if (_ev) _ev->beLinkState(true, line);
          linkHook(1);
          syncMessages();
        });
      });
    }, 2500);
  }
  void onDeviceInfo(const uint8_t* r, size_t n) {
    _proto_ver = r[1];
    _max_channels = n > 3 ? r[3] : 8;
    if (_max_channels == 0 || _max_channels > MAX_GROUP_CHANNELS) _max_channels = MAX_GROUP_CHANNELS;
    if (n >= 8) _node_prefs.ble_pin = rd32(r + 4);
    _fw_version[0] = 0;
    if (n >= 80) { memcpy(_fw_version, r + 60, 20); _fw_version[20] = 0; }
    char build[13] = {0}, model[41] = {0};
    if (n >= 20) memcpy(build, r + 8, 12);
    if (n >= 60) memcpy(model, r + 20, 40);
    _node_prefs.client_repeat = n > 80 ? r[80] : 0;
    _node_prefs.path_hash_mode = n > 81 ? r[81] : 0;
    fprintf(stderr, "[companion] node: %s, firmware %s (%s), protocol v%u, %u channel slots\n", model,
            _fw_version[0] ? _fw_version : "?", build, (unsigned)_proto_ver, (unsigned)_max_channels);
  }
  void onSelfInfo(const uint8_t* r, size_t n) {
    NodePrefs& p = _node_prefs;
    p.tx_power_dbm = (int8_t)r[2];
    memcpy(_self_key, r + 4, PUB_KEY_SIZE);
    const int32_t lat = (int32_t)rd32(r + 36), lon = (int32_t)rd32(r + 40);
    p.multi_acks = r[44];
    p.advert_loc_policy = r[45];
    p.telemetry_mode_base = r[46] & 0x03;
    p.telemetry_mode_loc = (r[46] >> 2) & 0x03;
    p.telemetry_mode_env = (r[46] >> 4) & 0x03;
    p.manual_add_contacts = r[47];
    p.freq = (float)rd32(r + 48) / 1000.0f;
    p.bw = (float)rd32(r + 52) / 1000.0f;
    p.sf = r[56];
    p.cr = r[57];
    memset(p.node_name, 0, sizeof p.node_name);
    memcpy(p.node_name, r + 58, std::min(n - 58, sizeof p.node_name - 1));
    _have_self = true;
    fprintf(stderr, "[companion] self: %s %s, %.3f MHz BW %.1f SF%u CR%u, %d dBm\n", p.node_name,
            hex(_self_key, 4).c_str(), p.freq, p.bw, p.sf, p.cr, p.tx_power_dbm);
    if (_ev) _ev->beSelfInfo(p, _self_key, lat, lon);
  }
  void onDeviceTime(const uint8_t* r, size_t n) {
    if (!r || r[0] != RESP_CURR_TIME || n < 5) return;
    const uint32_t node_t = rd32(r + 1), host_t = (uint32_t)time(nullptr);
    fprintf(stderr, "[companion] node clock %+lds from this computer's\n", (long)((int64_t)node_t - host_t));
    // The phone apps set an unset node clock; a set one is left alone (the node rejects
    // moving it backwards anyway).
    if (node_t < 1700000000u && host_t > 1700000000u) {
      std::vector<uint8_t> f = {CMD_SET_DEVICE_TIME};
      wr32(f, host_t);
      send(f, nullptr);
    }
  }
  void fetchContacts() {
    _list_seen.clear();
    send({CMD_GET_CONTACTS}, [this](const uint8_t* r, size_t) {
      if (!_link_up) return;
      if (!r || r[0] != RESP_END_OF_CONTACTS) {
        fprintf(stderr, "[companion] contact list incomplete (%s)\n", r ? "error" : "no reply");
        return;
      }
      // Contacts the node no longer has (a reconnect after changes elsewhere).
      std::vector<Key> gone;
      for (auto& kv : _known) if (!_list_seen.count(kv.first)) gone.push_back(kv.first);
      for (const Key& k : gone) { _known.erase(k); if (_ev) _ev->beContactRemoved(k.data()); }
      fprintf(stderr, "[companion] %d contacts\n", (int)_known.size());
    }, 10000);
  }
  void onContactListEntry(const uint8_t* f, size_t n) {
    ContactInfo c;
    if (!parseContact(f, n, c) || isMeshcomodPseudo(c)) return;
    const Key k = toKey(c.id.pub_key);
    _list_seen.insert(k);
    _known[k] = c;
    if (_ev) _ev->beContact(c, false, 0);
  }
  void fetchChannels() {
    _n_channels = 0;
    for (int i = 0; i < _max_channels; i++) {
      send({CMD_GET_CHANNEL, (uint8_t)i}, [this, i](const uint8_t* r, size_t n) {
        if (!r || r[0] != RESP_CHANNEL_INFO || n < 2 + 32 + 16) return;
        ChannelDetails cd{};
        memcpy(cd.name, r + 2, 32);
        cd.name[31] = 0;
        memcpy(cd.channel.secret, r + 34, 16);
        if (cd.name[0]) _n_channels++;
        if (_ev) _ev->beChannel(i, cd);
        _chan_secret[i] = std::vector<uint8_t>(r + 34, r + 50);
      });
    }
  }

  // ---- messages ---------------------------------------------------------------------
  void syncMessages() {
    if (!_ready) return;
    if (_syncing) { _sync_again = true; return; }
    _syncing = true;
    _sync_again = false;
    syncNext();
  }
  void syncNext() {
    send({CMD_SYNC_NEXT_MESSAGE}, [this](const uint8_t* r, size_t n) {
      if (r && r[0] != RESP_NO_MORE_MESSAGES && r[0] != RESP_ERR) {
        onMessageFrame(r, n);
        _synced++;
        syncNext();
        return;
      }
      _syncing = false;
      if (_synced) fprintf(stderr, "[companion] %u waiting messages synced\n", (unsigned)_synced);
      _synced = 0;
      if (_sync_again) syncMessages();
    });
  }
  // A received message, whichever way it came (sync reply or wadamesh's live push).
  void onMessageFrame(const uint8_t* f, size_t n) {
    // The same message can arrive both ways: drop repeats (snr byte excluded).
    {
      uint32_t h = 2166136261u;
      for (size_t i = 0; i < n; i++) { if (i == 1 && (f[0] == RESP_CONTACT_MSG_RECV_V3 || f[0] == RESP_CHANNEL_MSG_RECV_V3)) continue; h ^= f[i]; h *= 16777619u; }
      for (uint32_t s : _seen_msgs) if (s == h) return;
      _seen_msgs.push_back(h);
      if (_seen_msgs.size() > 256) _seen_msgs.pop_front();
    }
    int8_t snr_q4 = 0;
    const uint8_t* p = f + 1;
    size_t left = n - 1;
    if (f[0] == RESP_CONTACT_MSG_RECV_V3 || f[0] == RESP_CHANNEL_MSG_RECV_V3) {
      if (left < 3) return;
      snr_q4 = (int8_t)p[0];
      p += 3; left -= 3;   // snr, reserved x2
    }
    const int8_t rssi = (_now - _last_rx_ms < 3000) ? _last_rx_rssi : 0;
    if (f[0] == RESP_CONTACT_MSG_RECV || f[0] == RESP_CONTACT_MSG_RECV_V3) {
      if (left < 12) return;
      const uint8_t* prefix = p;
      const uint8_t path_len = p[6], txt_type = p[7];
      const uint32_t ts = rd32(p + 8);
      p += 12; left -= 12;
      const uint8_t* author = nullptr;
      if (txt_type == TXT_TYPE_SIGNED_PLAIN) {
        if (left < 4) return;
        author = p; p += 4; left -= 4;
      }
      std::string text((const char*)p, strnlen((const char*)p, left));
      const ContactInfo* from = findKnownPrefix(prefix, 6);
      if (!from) {
        fprintf(stderr, "[companion] message from unknown contact %s dropped\n", hex(prefix, 6).c_str());
        return;
      }
      if (!_ev) return;
      if (txt_type == TXT_TYPE_PLAIN) {
        _ev->beDirectMessage(from->id.pub_key, ts, text.c_str(), path_len, path_len != 0xFF, snr_q4, rssi);
      } else if (txt_type == TXT_TYPE_CLI_DATA) {
        _ev->beCliReply(from->id.pub_key, text.c_str());
      } else if (txt_type == TXT_TYPE_SIGNED_PLAIN) {
        char author_buf[16];
        const char* author_name;
        const ContactInfo* a = findKnownPrefix(author, 4);
        if (a && a->name[0]) author_name = a->name;
        else if (memcmp(author, _self_key, 4) == 0) author_name = _node_prefs.node_name;
        else { mesh::Utils::toHex(author_buf, (uint8_t*)author, 4); author_name = author_buf; }
        _ev->beRoomMessage(from->id.pub_key, author_name, ts, text.c_str(), path_len, snr_q4, rssi);
      }
      return;
    }
    // channel
    if (left < 7) return;
    const uint8_t idx = p[0], path_len = p[1], txt_type = p[2];
    const uint32_t ts = rd32(p + 3);
    p += 7; left -= 7;
    if (txt_type != TXT_TYPE_PLAIN || !_ev) return;
    std::string text((const char*)p, strnlen((const char*)p, left));
    _ev->beChannelMessage(idx, ts, text.c_str(), path_len, snr_q4, rssi);
  }

  // ---- pushes -----------------------------------------------------------------------
  void onPush(const uint8_t* f, size_t n) {
    switch (f[0]) {
      case PUSH_LOG_RX_DATA:
        if (n < 4) return;
        _last_rx_rssi = (int8_t)f[2];
        _last_rx_ms = _now;
        if (_ev) _ev->beRxRaw((int8_t)f[1], (int8_t)f[2], f + 3, n - 3);
        noteRawForDmEcho(f + 3, n - 3);
        noteRawAdvert(f + 3, n - 3);
        return;
      case PUSH_MSG_WAITING:
        syncMessages();
        return;
      case PUSH_SEND_CONFIRMED: {
        if (n < 5) return;
        auto it = _acks.find(rd32(f + 1));
        if (it == _acks.end()) return;
        const uint32_t local = it->second;
        _acks.erase(it);
        if (_ev) _ev->beAck(local);
        return;
      }
      case PUSH_ADVERT:
      case PUSH_PATH_UPDATED: {
        if (n < 1 + PUB_KEY_SIZE || !_ready) return;
        const bool advert = f[0] == PUSH_ADVERT;
        std::vector<uint8_t> q = {CMD_GET_CONTACT_BY_KEY};
        q.insert(q.end(), f + 1, f + 1 + PUB_KEY_SIZE);
        send(q, [this, advert](const uint8_t* r, size_t rn) {
          ContactInfo c;
          if (!r || r[0] != RESP_CONTACT || !parseContact(r, rn, c) || isMeshcomodPseudo(c)) return;
          _known[toKey(c.id.pub_key)] = c;
          if (_ev) _ev->beContact(c, advert, advert ? advertHops(c.id.pub_key, c) : 0);
        });
        return;
      }
      case PUSH_NEW_ADVERT: {
        ContactInfo c;
        if (!parseContact(f, n, c) || isMeshcomodPseudo(c)) return;
        if (_known.count(toKey(c.id.pub_key))) {   // an older node: NEW_ADVERT for a table entry
          _known[toKey(c.id.pub_key)] = c;
          if (_ev) _ev->beContact(c, true, advertHops(c.id.pub_key, c));
        } else if (_ev) {
          _ev->beDiscovered(c, advertHops(c.id.pub_key, c));
        }
        return;
      }
      case PUSH_CONTACT_DELETED:
        if (n < 1 + PUB_KEY_SIZE) return;
        _known.erase(toKey(f + 1));
        if (_ev) _ev->beContactRemoved(f + 1);
        return;
      case PUSH_CONTACTS_FULL:
        fprintf(stderr, "[companion] the node's contact table is full\n");
        return;
      case PUSH_LOGIN_SUCCESS:
      case PUSH_LOGIN_FAIL:
        // A modern LOGIN_OK: code, perms, key prefix (6), server clock (4), ACL, firmware level
        if (f[0] == PUSH_LOGIN_SUCCESS && n >= 14 && _ev)
          if (const ContactInfo* c = findKnownPrefix(f + 2, 6)) _ev->beLoginInfo(c->id.pub_key, f[12], f[13]);
        if (n >= 8) onLogin(f[0] == PUSH_LOGIN_SUCCESS, f[1], f + 2, n >= 12 ? rd32(f + 8) : 0);
        return;
      case PUSH_STATUS_RESPONSE:
      case PUSH_TELEMETRY_RESPONSE: {
        if (n < 8) return;
        const bool telem = f[0] == PUSH_TELEMETRY_RESPONSE;
        const ContactInfo* c = findKnownPrefix(f + 2, 6);
        if (!c) return;
        if (_req.active && memcmp(_req.key.data(), c->id.pub_key, PUB_KEY_SIZE) == 0 && _req.telemetry == telem)
          _req = Req{};   // answered: no fallback
        if (!_ev) return;
        if (telem) _ev->beTelemetryReply(c->id.pub_key, f + 8, n - 8);
        else _ev->beStatusReply(c->id.pub_key, f + 8, n - 8);
        return;
      }
      case PUSH_TRACE_DATA: {
        if (n < 12) return;
        const uint8_t path_len = f[2], flags = f[3];
        const uint32_t tag = rd32(f + 4);
        const uint8_t path_sz = flags & 0x03;
        const size_t nsnr = path_sz >= 4 ? 0 : (size_t)(path_len >> path_sz);
        if (n < 12 + (size_t)path_len + nsnr + 1) return;
        const int8_t* snrs = (const int8_t*)(f + 12 + path_len);
        const int8_t final_snr = (int8_t)f[12 + path_len + nsnr];
        if (_ev) _ev->beTraceResult(tag, nsnr ? snrs[0] : 0, final_snr,
                                    nsnr > 1 ? (uint8_t)(nsnr - 1) : 0, nsnr > 1 ? snrs + 1 : nullptr);
        return;
      }
      case PUSH_BINARY_RESPONSE: {
        if (n < 6) return;
        const uint32_t tag = rd32(f + 2);
        if (_bin_tag && tag == _bin_tag) {   // the admin app's request (CMD_SEND_BINARY_REQ)
          _bin_tag = 0;
          if (_ev) _ev->beBinaryReply(_bin_key.data(), f + 6, n - 6);
          return;
        }
        if (!_regions_tag || tag != _regions_tag) return;
        _regions_tag = 0;
        if (_ev) _ev->beRegionsReply(_regions_key.data(), f + 2, n - 2);   // tag, clock, CSV: as on a device
        return;
      }
      default:
        return;
    }
  }

  // ---- status / telemetry, the way the device asks (src/MyMesh.h uiSendRequestAfterGuestLogin):
  // the request goes straight out; a repeater / room / sensor that does not answer gets a
  // guest login, and the request again 2 s after the login is accepted.
  struct Req {
    bool active = false, telemetry = false, logged_in = false;
    Key key{};
    uint32_t fallback_at = 0, resend_at = 0;
  };
  int startRequest(const ContactInfo& c, bool telemetry) {
    if (!_ready) return MSG_SEND_FAILED;
    _req = Req{};
    _req.active = true;
    _req.telemetry = telemetry;
    _req.key = toKey(c.id.pub_key);
    const bool needs_login = c.type == ADV_TYPE_REPEATER || c.type == ADV_TYPE_ROOM || c.type == ADV_TYPE_SENSOR;
    const int r = routeGuess(c);
    if (needs_login) _req.fallback_at = (_now + (r == MSG_SEND_SENT_DIRECT ? 6000u : 12000u)) | 1u;
    sendRequestFrame();
    return r;
  }
  void sendRequestFrame() {
    std::vector<uint8_t> f;
    if (_req.telemetry) f = {CMD_SEND_TELEMETRY_REQ, 0, 0, 0};
    else f = {CMD_SEND_STATUS_REQ};
    f.insert(f.end(), _req.key.begin(), _req.key.end());
    send(f, [](const uint8_t* r, size_t) {
      if (!r || r[0] != RESP_SENT) fprintf(stderr, "[companion] request not sent (%s)\n", r ? "node refused" : "no reply");
    });
  }
  void serviceRequest(uint32_t now) {
    if (!_req.active || !_ready) return;
    if (_req.fallback_at && (int32_t)(now - _req.fallback_at) >= 0) {
      _req.fallback_at = 0;
      fprintf(stderr, "[companion] no answer: guest login, then ask again\n");
      sendLogin(_req.key.data(), "");
    }
    if (_req.resend_at && (int32_t)(now - _req.resend_at) >= 0) {
      _req.resend_at = 0;
      sendRequestFrame();
    }
  }
  void sendLogin(const uint8_t* pub_key, const char* password) {
    std::vector<uint8_t> f = {CMD_SEND_LOGIN};
    f.insert(f.end(), pub_key, pub_key + PUB_KEY_SIZE);
    f.insert(f.end(), password, password + strnlen(password, 15));
    send(f, nullptr);
  }
  void onLogin(bool ok, uint8_t perms, const uint8_t* prefix6, uint32_t server_clock) {
    const ContactInfo* c = findKnownPrefix(prefix6, 6);
    if (!c) return;
    const Key k = toKey(c->id.pub_key);
    if (ok) _sessions[k] = _now; else _sessions.erase(k);
    if (_admin_login_pending && _admin_login == k) {
      _admin_login_pending = false;
      if (_ev) _ev->beLoginResult(c->id.pub_key, ok, perms, server_clock);
      return;
    }
    if (_req.active && _req.key == k && ok && !_req.logged_in) {
      _req.logged_in = true;
      _req.resend_at = (_now + 2000) | 1u;
    }
  }

  // ---- helpers ------------------------------------------------------------------------
  void linkHook(int state) {
    if (!g_wada_link_hook || !_t) return;
    const std::string k = state ? hex(_self_key, 4) : std::string();
    const std::string n = _t->name();
    const char* via = (n == "serial" || n == "web-serial") ? "USB" : n == "ble" ? "Bluetooth" : "Wi-Fi";
    g_wada_link_hook(state, state ? _node_prefs.node_name : "", via, k.c_str());
  }
  const ContactInfo* findKnown(const uint8_t* pub_key) const {
    auto it = _known.find(toKey(pub_key));
    return it == _known.end() ? nullptr : &it->second;
  }
  const ContactInfo* findKnownPrefix(const uint8_t* prefix, size_t n) const {
    if (!prefix) return nullptr;
    for (auto& kv : _known) if (memcmp(kv.first.data(), prefix, n) == 0) return &kv.second;
    return nullptr;
  }
  int routeGuess(const ContactInfo& c) const {
    const ContactInfo* k = findKnown(c.id.pub_key);
    const uint8_t opl = k ? k->out_path_len : c.out_path_len;
    return opl == OUT_PATH_UNKNOWN ? MSG_SEND_SENT_FLOOD : MSG_SEND_SENT_DIRECT;
  }
  // The exact payload the node will flood for this channel post (BaseChatMesh::
  // sendGroupMessage + Mesh::createGroupDatagram: deterministic), fingerprinted the way
  // the device fingerprints its own floods, so echoes heard back are counted as repeats.
  uint32_t channelPayloadFp(const ChannelDetails& ch, uint32_t ts, const char* text, size_t tlen) const {
    if (!_have_self) return randomTag();
    uint8_t temp[5 + MAX_TEXT_LEN + 32];
    memcpy(temp, &ts, 4);
    temp[4] = TXT_TYPE_PLAIN;
    int prefix_len = snprintf((char*)&temp[5], 40, "%s: ", _node_prefs.node_name);
    if (prefix_len < 0) prefix_len = 0;
    if ((int)tlen + prefix_len > MAX_TEXT_LEN) tlen = (size_t)(MAX_TEXT_LEN - prefix_len);
    memcpy(&temp[5 + prefix_len], text, tlen);
    uint8_t payload[MAX_PACKET_PAYLOAD + 16];
    payload[0] = ch.channel.hash[0];
    const int enc = mesh::Utils::encryptThenMAC(ch.channel.secret, &payload[PATH_HASH_SIZE], temp, 5 + prefix_len + (int)tlen);
    return fnv1a32(payload, (size_t)(PATH_HASH_SIZE + enc));
  }
  // DM "repeats heard": a DM's payload is encrypted with a key only the node has, so its
  // echo cannot be fingerprinted in advance. The first flooded TXT_MSG from us to that
  // contact heard after the send is taken as ours; later copies of that payload count.
  struct DmEcho { uint8_t dest0; uint32_t local_fp; uint32_t payload_fp; uint32_t until; };
  void noteRawForDmEcho(const uint8_t* raw, size_t len) {
    if (_dm_echo.empty() || len < 4 || !_have_self) return;
    const uint8_t rt = raw[0] & 0x03, ptype = (raw[0] >> 2) & 0x0F;
    if (ptype != PAYLOAD_TYPE_TXT_MSG || !(rt == ROUTE_TYPE_FLOOD || rt == ROUTE_TYPE_TRANSPORT_FLOOD)) return;
    const int ps = 1 + (rt == ROUTE_TYPE_TRANSPORT_FLOOD ? 4 : 0);
    const uint8_t hops = raw[ps] & 0x3F, hsz = (uint8_t)(((raw[ps] >> 6) & 3) + 1);
    const size_t pl = (size_t)ps + 1 + (size_t)hops * hsz;
    if (pl + 2 > len) return;
    const uint32_t fp = fnv1a32(raw + pl, len - pl);
    for (DmEcho& e : _dm_echo) {
      if ((int32_t)(_now - e.until) > 0) continue;
      if (e.payload_fp ? e.payload_fp != fp : (raw[pl] != e.dest0 || raw[pl + 1] != _self_key[0])) continue;
      e.payload_fp = fp;
      if (_ev) _ev->beRepeatHeard(e.local_fp, hops ? raw + pl - hsz : nullptr, hops ? hsz : 0);
      return;
    }
  }
  // Hop counts of recent adverts (raw RX), for the "heard N hops away" the UI shows.
  void noteRawAdvert(const uint8_t* raw, size_t len) {
    if (len < 3 || ((raw[0] >> 2) & 0x0F) != PAYLOAD_TYPE_ADVERT) return;
    const uint8_t rt = raw[0] & 0x03;
    const int ps = 1 + ((rt == ROUTE_TYPE_TRANSPORT_FLOOD || rt == ROUTE_TYPE_TRANSPORT_DIRECT) ? 4 : 0);
    const uint8_t plb = raw[ps];
    const size_t pl = (size_t)ps + 1 + (size_t)(plb & 0x3F) * (((plb >> 6) & 3) + 1);
    if (pl + 4 > len) return;
    AdvertHop h;
    memcpy(h.prefix, raw + pl, 4);
    h.path_len = plb;
    h.ms = _now;
    _adv_hops.push_back(h);
    if (_adv_hops.size() > 16) _adv_hops.pop_front();
  }
  uint8_t advertHops(const uint8_t* pub_key, const ContactInfo& c) const {
    for (auto it = _adv_hops.rbegin(); it != _adv_hops.rend(); ++it)
      if (memcmp(it->prefix, pub_key, 4) == 0 && _now - it->ms < 5000) return it->path_len;
    return c.out_path_len == OUT_PATH_UNKNOWN ? 0 : c.out_path_len;
  }
  struct AdvertHop { uint8_t prefix[4]; uint8_t path_len; uint32_t ms; };

  CompanionTransport* _t;
  bool _read_only;
  bool _debug = false;
  char _name[32];
  MeshBackendEvents* _ev = nullptr;
  uint32_t _now = 0;

  std::deque<Cmd> _q;
  bool _inflight = false;
  uint32_t _deadline = 0;

  bool _link_up = false, _ready = false, _have_self = false;
  uint32_t _handshake_retry_at = 0;
  uint8_t _proto_ver = 0, _max_channels = 8;
  int _n_channels = 0;
  char _fw_version[24] = {0};
  uint16_t _batt_mv = 0;
  NodePrefs _node_prefs{};
  uint8_t _self_key[PUB_KEY_SIZE] = {0};

  std::map<Key, ContactInfo> _known;   // the node's contact table, as last reported
  std::set<Key> _list_seen;
  std::map<int, std::vector<uint8_t>> _chan_secret;

  bool _syncing = false, _sync_again = false;
  unsigned _synced = 0;
  std::deque<uint32_t> _seen_msgs;

  std::map<uint32_t, uint32_t> _acks;  // node's expected ack -> the tag the UI holds
  std::vector<DmEcho> _dm_echo;
  std::deque<AdvertHop> _adv_hops;
  int8_t _last_rx_rssi = 0;
  uint32_t _last_rx_ms = 0;

  Req _req;
  Key _admin_login{};
  bool _admin_login_pending = false;
  std::map<Key, uint32_t> _sessions;   // logged in this session -> last checked
  uint32_t _regions_tag = 0;
  Key _regions_key{};
  uint32_t _bin_tag = 0;   // the admin app's binary request in flight
  Key _bin_key{};
};

}  // namespace

MeshBackend* wadaCreateCompanionBackend(CompanionTransport* transport, bool read_only) {
  return new CompanionBackend(transport, read_only);
}
