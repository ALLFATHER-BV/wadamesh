// SPDX-License-Identifier: GPL-3.0-or-later
// DemoBackend: a small believable neighbourhood for the website demo and for
// developing the web build without a radio. The cast, places and conversations
// are scripts/doc/web-demo/demo.py's, so the browser demo and the documentation
// screenshots tell the same story.
//
// What it does:
//   * reports a node identity, radio settings, 8 contacts (chat, repeater, room),
//     2 channels and 4 heard-but-not-added nodes;
//   * on the FIRST boot of a data directory, plays the demo conversations into the UI
//     (a marker file keeps them from being added again on every start);
//   * DMs you send are ACKed after a short, varying delay, sometimes answered;
//     channel posts are "heard repeated"; status pings to the repeater answer;
//   * every so often someone says something on Public, and a node adverts.
// Everything is deterministic per run except the timings' small jitter.
#include "MeshBackend.h"
#include <Arduino.h>
#include <SPIFFS.h>
#include <Utils.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/TxtDataHelpers.h>
#include <vector>
#include <array>
#include <string>
#include <math.h>
#include <stdlib.h>
#include "WebLocation.h"

namespace {

// demo.py: self node (Amsterdam centre) + radio. With the visitor's location (the web
// board's GPS, once allowed) the whole neighbourhood moves there, same offsets, so the
// map and the distances look like a real mesh around them (kept for the next visit).
const int32_t SELF_LAT = 52372800, SELF_LON = 4893400;
const char* NODE_NAME = "Kaj T-Deck";

struct DemoContact { const char* name; uint8_t type; bool fav; bool direct; uint32_t age_s; double dlat, dlon; };
const DemoContact k_contacts[] = {
  {"Sanne",            ADV_TYPE_CHAT,     true,  true,  95,        0.021, -0.015},
  {"Diego",            ADV_TYPE_CHAT,     false, false, 1500,     -0.034,  0.052},
  {"Marta",            ADV_TYPE_CHAT,     false, true,  320,       0.0,    0.0},
  {"Hilltop Repeater", ADV_TYPE_REPEATER, false, false, 210,       0.145,  0.088},
  {"Harbor Room",      ADV_TYPE_ROOM,     false, false, 640,      -0.011,  0.031},
  {"Noah",             ADV_TYPE_CHAT,     false, false, 18000,     0.0,    0.0},
  {"Femke",            ADV_TYPE_CHAT,     false, true,  86400 * 3, -0.062, -0.044},
  {"Old Node",         ADV_TYPE_CHAT,     false, false, 86400 * 9, 0.0,    0.0},
};
struct DemoDiscovered { const char* name; uint8_t type; uint8_t path_len; uint32_t age_s; double dlat, dlon; };
const DemoDiscovered k_discovered[] = {
  {"Lars",           ADV_TYPE_CHAT,     2, 140,  0.012,  0.077},
  {"Ridge Repeater", ADV_TYPE_REPEATER, 1, 380,  0.191, -0.02},
  {"Cafe Beacon",    ADV_TYPE_CHAT,     3, 900, -0.008,  0.006},
  {"Yuki",           ADV_TYPE_CHAT,     2, 2600, 0.0,    0.0},
};
// demo.py _MESSAGES, oldest first. out=true: sent by this node (from the "phone app").
struct DemoMsg { const char* thread; bool out; const char* sender; const char* text; uint8_t path; int8_t snr; int8_t rssi; };
const DemoMsg k_history[] = {
  {"Sanne", false, nullptr, "Morning! You around for the hike this weekend?", 0, 8, -64},
  {"Sanne", true,  nullptr, "Yeah! Saturday works. Meshing the whole way up.", 0, 0, 0},
  {"Sanne", false, nullptr, "Perfect. I'll bring the spare T-Deck as a relay.", 0, 7, -66},
  {"Sanne", true,  nullptr, "Just tested range from the ridge: 6 km line of sight.", 0, 0, 0},
  {"Sanne", false, nullptr, "Nice, that covers the valley. See you at 8.", 0, 9, -61},
  {"Diego", false, nullptr, "Did the firmware update land on your node?", 2, 2, -98},
  {"Diego", true,  nullptr, "Flashing now. beta looks solid so far.", 2, 0, 0},
  {"Diego", false, nullptr, "Great. Ping me once the repeater picks you up.", 2, 1, -101},
  {"Diego", true,  nullptr, "Will do \xF0\x9F\x91\x8D", 2, 0, 0},
  {"Marta", true,  nullptr, "Map tiles are caching fine on the new build.", 0, 0, 0},
  {"Marta", false, nullptr, "Awesome, offline maps at the campsite then.", 0, 6, -70},
  {"Marta", true,  nullptr, "Exactly. Sending you my GPS pin now.", 0, 0, 0},
  {"Marta", false, nullptr, "Didn't come through, try again?", 0, 5, -72},
  {"Harbor Room", false, "Diego",  "Anyone near the north dock?", 1, 3, -92},
  {"Harbor Room", false, "Sanne",  "I'm 300 m out, what's up?", 1, 7, -68},
  {"Harbor Room", false, "Femke",  "Copy. Battery at 82% on the solar node.", 2, 1, -104},
  {"Harbor Room", false, "Diego",  "Meeting at the boathouse in 10.", 1, 4, -88},
  {"#Public", false, nullptr, "Hilltop Repeater: back online after the storm, all good.", 1, 4, -90},
  {"#Public", false, nullptr, "Lars: anyone else hearing the Ridge repeater today?", 2, 2, -97},
};
// Live chatter on Public, one line every minute or so.
const char* k_public_chatter[] = {
  "Diego: testing from the tram, 2 hops",
  "Femke: solar node at 85% and climbing",
  "Lars: is there a meetup this month?",
  "Sanne: the ridge link is solid today",
  "Noah: new antenna on the roof, let me know how I sound",
};
// What a DM partner might say back.
const char* k_replies[] = {
  "Got it, thanks!",
  "Copy that.",
  "Sounds good \xF0\x9F\x91\x8C",
  "Received loud and clear.",
};

// A stable public key per name: sha256("wadamesh-demo:" + name). Not a real keypair,
// which is fine: the demo never encrypts.
void demoKey(const char* name, uint8_t out[PUB_KEY_SIZE]) {
  std::string s = std::string("wadamesh-demo:") + name;
  mesh::Utils::sha256(out, PUB_KEY_SIZE, (const uint8_t*)s.data(), (int)s.size());
}
int32_t absCoord(int32_t base, double d) { return d == 0.0 ? 0 : (int32_t)(base + d * 1000000.0); }

struct Pending {
  uint32_t due_ms;
  enum Kind { Ack, Reply, Repeat, Status, Trace, Login, Binary, Cli } kind;
  uint8_t key[PUB_KEY_SIZE];
  uint32_t value;
  std::string text;   // Cli: the command line; Binary: the request
};

class DemoBackend : public MeshBackend {
public:
  const char* name() const override { return "demo"; }

  void begin(MeshBackendEvents& ev, NodePrefs& prefs, uint8_t self_pub_key[32]) override {
    _ev = &ev;
    demoKey(NODE_NAME, self_pub_key);
    StrHelper::strncpy(prefs.node_name, NODE_NAME, sizeof(prefs.node_name));
    prefs.freq = 869.525f; prefs.bw = 250.0f; prefs.sf = 11; prefs.cr = 5; prefs.tx_power_dbm = 22;   // demo.py RADIO
    loadHome();
    ev.beSelfPosition(_home_lat, _home_lon);
    const uint32_t now = (uint32_t)time(nullptr);
    for (const DemoContact& d : k_contacts) {
      ContactInfo c{};
      demoKey(d.name, c.id.pub_key);
      StrHelper::strncpy(c.name, d.name, sizeof(c.name));
      c.type = d.type;
      c.flags = d.fav ? 0x01 : 0;
      c.out_path_len = d.direct ? 0 : OUT_PATH_UNKNOWN;
      c.last_advert_timestamp = now - d.age_s;
      c.lastmod = now - d.age_s;
      c.gps_lat = absCoord(_home_lat, d.dlat);
      c.gps_lon = absCoord(_home_lon, d.dlon);
      ev.beContact(c, false, 0);
    }
    // WADA_DEMO_CONTACTS="name|name|...": extra chat contacts for a scripted run, so a
    // headless test can show a name the cast does not have (accents, emoji).
    if (const char* extra = getenv("WADA_DEMO_CONTACTS")) {
      const std::string all(extra);
      size_t at = 0;
      while (at <= all.size()) {
        size_t bar = all.find('|', at);
        if (bar == std::string::npos) bar = all.size();
        const std::string nm = all.substr(at, bar - at);
        at = bar + 1;
        if (nm.empty()) continue;
        ContactInfo c{};
        demoKey(nm.c_str(), c.id.pub_key);
        StrHelper::strncpy(c.name, nm.c_str(), sizeof(c.name));
        c.type = ADV_TYPE_CHAT;
        c.out_path_len = 0;
        c.last_advert_timestamp = c.lastmod = now - 60;
        ev.beContact(c, false, 0);
      }
    }
    addChannel(0, "Public", nullptr);
    addChannel(1, "#emergency", "#emergency");
  }

  void loop(uint32_t now_ms) override {
    if (!_started) { _started = true; _t0 = now_ms; }
    followHost();
    // After the splash: the heard-not-added nodes, then (first boot only) the history.
    if (!_seeded && now_ms - _t0 > 1500) {
      _seeded = true;
      const uint32_t now = (uint32_t)time(nullptr);
      for (const DemoDiscovered& d : k_discovered) {
        ContactInfo c{};
        demoKey(d.name, c.id.pub_key);
        StrHelper::strncpy(c.name, d.name, sizeof(c.name));
        c.type = d.type;
        c.out_path_len = OUT_PATH_UNKNOWN;
        c.last_advert_timestamp = now - d.age_s;
        c.gps_lat = absCoord(_home_lat, d.dlat);
        c.gps_lon = absCoord(_home_lon, d.dlon);
        _ev->beDiscovered(c, d.path_len);
      }
      if (!SPIFFS.exists(k_seed_marker)) {
        playHistory();
        File f = SPIFFS.open(k_seed_marker, "w");
        if (f) { f.print("1"); f.close(); }
      }
      // WADA_DEMO_CHATTER_MS=N: a Public line every N ms (the first after N), so a
      // headless script can have messages arrive at known times (navigation tests).
      // WADA_DEMO_CHATTER_BURST=N: the first N lines 20 ms apart (a long channel history).
      if (const char* ev = getenv("WADA_DEMO_CHATTER_MS")) _chatter_every = (uint32_t)strtoul(ev, nullptr, 10);
      if (const char* ev = getenv("WADA_DEMO_CHATTER_BURST")) _chatter_burst = (uint32_t)strtoul(ev, nullptr, 10);
      _next_chatter = now_ms + (_chatter_burst ? 20u : _chatter_every ? _chatter_every : 45000);
      _next_advert = now_ms + 20000;
    }
    if (_seeded && (int32_t)(now_ms - _next_chatter) >= 0) {
      const char* line = k_public_chatter[_chatter_i++ % (sizeof k_public_chatter / sizeof *k_public_chatter)];
      _ev->beChannelMessage(0, 0, line, 1 + (uint8_t)random(3), (int8_t)(4 * (random(12) - 3)), (int8_t)(-60 - random(45)));
      _next_chatter = now_ms + (_chatter_burst ? (_chatter_burst--, 20u)
                                : _chatter_every ? _chatter_every : 60000 + (uint32_t)random(30000));
    }
    if (_seeded && (int32_t)(now_ms - _next_advert) >= 0) {
      const DemoContact& d = k_contacts[_advert_i++ % 5];   // the active five re-advert
      ContactInfo c{};
      demoKey(d.name, c.id.pub_key);
      StrHelper::strncpy(c.name, d.name, sizeof(c.name));
      c.type = d.type; c.flags = d.fav ? 0x01 : 0;
      c.out_path_len = d.direct ? 0 : OUT_PATH_UNKNOWN;
      c.last_advert_timestamp = c.lastmod = (uint32_t)time(nullptr);
      c.gps_lat = absCoord(_home_lat, d.dlat);
      c.gps_lon = absCoord(_home_lon, d.dlon);
      _ev->beContact(c, true, d.direct ? 0 : 2);
      _ev->beSignal((int8_t)(4 * (random(14) - 4)), (int8_t)(-55 - random(50)));
      _next_advert = now_ms + 40000 + (uint32_t)random(40000);
    }
    for (size_t i = 0; i < _pending.size();) {
      if ((int32_t)(now_ms - _pending[i].due_ms) < 0) { ++i; continue; }
      Pending p = _pending[i];
      _pending.erase(_pending.begin() + (long)i);
      fire(p);
    }
  }

  int sendDirect(const ContactInfo& to, uint32_t ts, const char* text, uint32_t& expected_ack,
                 uint32_t& est_timeout_ms, uint32_t& sent_fp) override {
    (void)ts; (void)text;
    expected_ack = nextTag();
    est_timeout_ms = 4000;
    const bool direct = to.out_path_len != OUT_PATH_UNKNOWN;
    sent_fp = direct ? 0 : nextTag();
    queue(Pending::Ack, to.id.pub_key, expected_ack, 700 + (uint32_t)random(1500));
    if (sent_fp) queue(Pending::Repeat, to.id.pub_key, sent_fp, 400 + (uint32_t)random(600));
    if (to.type == ADV_TYPE_CHAT && random(100) < 60)
      queue(Pending::Reply, to.id.pub_key, 0, 3500 + (uint32_t)random(4000));
    return direct ? MSG_SEND_SENT_DIRECT : MSG_SEND_SENT_FLOOD;
  }
  bool sendChannel(int channel_idx, const ChannelDetails& ch, uint32_t ts, const char* sender_name,
                   const char* text, uint32_t& sent_fp) override {
    (void)channel_idx; (void)ch; (void)ts; (void)sender_name; (void)text;
    sent_fp = nextTag();
    uint8_t none[PUB_KEY_SIZE] = {0};
    const int repeats = 1 + (int)random(3);
    for (int i = 0; i < repeats; ++i) queue(Pending::Repeat, none, sent_fp, 350 + (uint32_t)random(1800));
    return true;
  }
  bool sendAdvert(bool flood) override { (void)flood; return true; }

  int requestStatus(const ContactInfo& c) override {
    if (c.type != ADV_TYPE_REPEATER && c.type != ADV_TYPE_ROOM) return MSG_SEND_FAILED;
    if (silentTelemetry()) Serial.printf("[demo] status request to %s at %u ms\n", c.name, (unsigned)millis());
    queue(Pending::Status, c.id.pub_key, 0, 900 + (uint32_t)random(800));
    return c.out_path_len == OUT_PATH_UNKNOWN ? MSG_SEND_SENT_FLOOD : MSG_SEND_SENT_DIRECT;
  }
  // Telemetry is not part of the demo: the request fails at once (the default). For
  // tests, WADA_DEMO_SILENT_TELEMETRY=1 makes it a request that goes out and whose
  // reply is lost on the air, so the UI's single-flight request slot stays taken
  // until its expiry (src/helpers/UiRequestSlot.h; web/scripts/ui-slot-expiry.wada).
  static bool silentTelemetry() {
    const char* e = getenv("WADA_DEMO_SILENT_TELEMETRY");
    return e && e[0] == '1';
  }
  int requestTelemetry(const ContactInfo& c) override {
    if (!silentTelemetry() || (c.type != ADV_TYPE_REPEATER && c.type != ADV_TYPE_ROOM && c.type != ADV_TYPE_SENSOR))
      return MSG_SEND_FAILED;
    Serial.printf("[demo] telemetry request to %s at %u ms: no reply will come\n", c.name, (unsigned)millis());
    return c.out_path_len == OUT_PATH_UNKNOWN ? MSG_SEND_SENT_FLOOD : MSG_SEND_SENT_DIRECT;
  }
  // Repeater / room admin (the repeater admin app): any password is the admin's, an
  // empty one a guest's; the demo repeater answers status, neighbours, its access list,
  // owner info and a few CLI lines.
  int login(const ContactInfo& c, const char* password) override {
    if (c.type != ADV_TYPE_REPEATER && c.type != ADV_TYPE_ROOM) return MSG_SEND_FAILED;
    queue(Pending::Login, c.id.pub_key, (password && password[0]) ? 1u : 0u, 700 + (uint32_t)random(600));
    return c.out_path_len == OUT_PATH_UNKNOWN ? MSG_SEND_SENT_FLOOD : MSG_SEND_SENT_DIRECT;
  }
  bool hasConnectionTo(const uint8_t pub_key[32]) override {
    for (const auto& k : _logged_in) if (memcmp(k.data(), pub_key, PUB_KEY_SIZE) == 0) return true;
    return false;
  }
  int binaryRequest(const ContactInfo& c, const uint8_t* payload, uint8_t len) override {
    if ((c.type != ADV_TYPE_REPEATER && c.type != ADV_TYPE_ROOM) || !payload || !len) return MSG_SEND_FAILED;
    Pending p{};
    p.due_ms = millis() + 800 + (uint32_t)random(700); p.kind = Pending::Binary;
    memcpy(p.key, c.id.pub_key, PUB_KEY_SIZE);
    p.text.assign((const char*)payload, len);
    _pending.push_back(p);
    return c.out_path_len == OUT_PATH_UNKNOWN ? MSG_SEND_SENT_FLOOD : MSG_SEND_SENT_DIRECT;
  }
  int sendCli(const ContactInfo& c, const char* text) override {
    if ((c.type != ADV_TYPE_REPEATER && c.type != ADV_TYPE_ROOM) || !text) return MSG_SEND_FAILED;
    Pending p{};
    p.due_ms = millis() + 700 + (uint32_t)random(600); p.kind = Pending::Cli;
    memcpy(p.key, c.id.pub_key, PUB_KEY_SIZE);
    p.text = text;
    _pending.push_back(p);
    return c.out_path_len == OUT_PATH_UNKNOWN ? MSG_SEND_SENT_FLOOD : MSG_SEND_SENT_DIRECT;
  }
  uint32_t tracePing(const uint8_t pub_key[32]) override {
    const uint32_t tag = nextTag();
    queue(Pending::Trace, pub_key, tag, 600 + (uint32_t)random(700));
    return tag;
  }
  uint32_t packetsRecv() const override { return 0; }

private:
  static constexpr const char* k_seed_marker = "/web_demo_seeded";
  static constexpr const char* k_home_file = "/web_demo_home";   // "lat_e6,lon_e6": the last re-homing

  void loadHome() {
    _home_lat = SELF_LAT; _home_lon = SELF_LON;
    File f = SPIFFS.open(k_home_file, "r");
    if (!f) return;
    char b[40] = {0};
    f.read((uint8_t*)b, sizeof b - 1);
    f.close();
    long la = 0, lo = 0;
    if (sscanf(b, "%ld,%ld", &la, &lo) == 2 && (la || lo)) { _home_lat = (int32_t)la; _home_lon = (int32_t)lo; }
  }
  // The visitor's location arrived (a GPS fix on the web board) and is more than a
  // kilometre from where the cast stands: move everyone there, offsets unchanged.
  void followHost() {
    double la = 0, lo = 0;
    if (!wadaHostLocation(la, lo)) return;
    const int32_t lat = (int32_t)llround(la * 1e6), lon = (int32_t)llround(lo * 1e6);
    if (labs((long)lat - _home_lat) < 10000 && labs((long)lon - _home_lon) < 10000) return;   // ~1 km
    _home_lat = lat; _home_lon = lon;
    File f = SPIFFS.open(k_home_file, "w");
    if (f) { f.printf("%ld,%ld", (long)lat, (long)lon); f.close(); }
    const uint32_t now = (uint32_t)time(nullptr);
    for (const DemoContact& d : k_contacts) {   // the contact table: replaced in place, no "heard"
      ContactInfo c{};
      demoKey(d.name, c.id.pub_key);
      StrHelper::strncpy(c.name, d.name, sizeof(c.name));
      c.type = d.type;
      c.flags = d.fav ? 0x01 : 0;
      c.out_path_len = d.direct ? 0 : OUT_PATH_UNKNOWN;
      c.last_advert_timestamp = now - d.age_s;
      c.lastmod = now - d.age_s;
      c.gps_lat = absCoord(_home_lat, d.dlat);
      c.gps_lon = absCoord(_home_lon, d.dlon);
      _ev->beContact(c, false, 0);
    }
    if (_seeded) {   // the heard-not-added nodes were listed at their old places: hear them again
      for (const DemoDiscovered& d : k_discovered) {
        ContactInfo c{};
        demoKey(d.name, c.id.pub_key);
        StrHelper::strncpy(c.name, d.name, sizeof(c.name));
        c.type = d.type;
        c.out_path_len = OUT_PATH_UNKNOWN;
        c.last_advert_timestamp = now - d.age_s;
        c.gps_lat = absCoord(_home_lat, d.dlat);
        c.gps_lon = absCoord(_home_lon, d.dlon);
        _ev->beDiscovered(c, d.path_len);
      }
    }
    _ev->beSelfPosition(_home_lat, _home_lon);
    Serial.println("[web] demo: the neighbourhood moved to this device's location");
  }
  int32_t _home_lat = SELF_LAT, _home_lon = SELF_LON;

  void addChannel(int idx, const char* name, const char* hashtag_key) {
    ChannelDetails ch{};
    StrHelper::strncpy(ch.name, name, sizeof(ch.name));
    if (hashtag_key) {   // hashtag channels: the key is SHA256("#name") (16 bytes)
      mesh::Utils::sha256(ch.channel.secret, 16, (const uint8_t*)hashtag_key, (int)strlen(hashtag_key));
    } else {             // MyMesh.cpp PUBLIC_GROUP_PSK, izOH6cXN6mrJ5e26oRXNcg==
      static const uint8_t k_public[16] = {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                           0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};
      memcpy(ch.channel.secret, k_public, 16);
    }
    _ev->beChannel(idx, ch);
  }
  void playHistory() {
    for (const DemoMsg& m : k_history) {
      const bool is_channel = m.thread[0] == '#';
      if (is_channel) {
        _ev->beChannelMessage(0, 0, m.text, m.path, (int8_t)(m.snr * 4), m.rssi);
        continue;
      }
      uint8_t key[PUB_KEY_SIZE];
      demoKey(m.thread, key);
      if (m.out) {
        const uint32_t ack = nextTag();
        _ev->beOutgoingDirect(key, m.text, ack);
        _ev->beAck(ack);   // history: already delivered
      } else if (m.sender) {
        _ev->beRoomMessage(key, m.sender, 0, m.text, m.path, (int8_t)(m.snr * 4), m.rssi);
      } else {
        _ev->beDirectMessage(key, 0, m.text, m.path ? m.path : 0xFF, m.path != 0, (int8_t)(m.snr * 4), m.rssi);
      }
    }
  }
  void fire(const Pending& p) {
    switch (p.kind) {
      case Pending::Ack:    _ev->beAck(p.value); break;
      case Pending::Repeat: _ev->beRepeatHeard(p.value); _ev->beSignal((int8_t)(4 * (random(10) - 2)), (int8_t)(-70 - random(30))); break;
      case Pending::Reply: {
        const char* r = k_replies[_reply_i++ % (sizeof k_replies / sizeof *k_replies)];
        _ev->beDirectMessage(p.key, 0, r, 0xFF, false, (int8_t)(4 * (3 + random(6))), (int8_t)(-60 - random(30)));
        break;
      }
      case Pending::Status: {
        // RepeaterStats, packed little-endian like the repeater firmware sends it.
        uint8_t s[56] = {0};
        auto put16 = [&](int o, uint16_t v) { s[o] = (uint8_t)v; s[o + 1] = (uint8_t)(v >> 8); };
        auto put32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; ++i) s[o + i] = (uint8_t)(v >> (8 * i)); };
        put16(0, 4012);                     // batt_mv
        put16(2, 0);                        // queue_len
        put16(4, (uint16_t)(int16_t)-118);  // noise_floor
        put16(6, (uint16_t)(int16_t)-87);   // last_rssi
        put32(8, 18234); put32(12, 9120);   // n_recv, n_sent
        put32(16, 2 * 3600 + 13 * 60);              // TX airtime
        put32(20, 6 * 86400 + 4 * 3600 + 17 * 60);   // uptime_secs
        put32(24, 3120); put32(28, 2210);   // sent flood / direct
        put32(32, 11870); put32(36, 6364);  // recv flood / direct
        put16(42, (uint16_t)(int16_t)(6 * 4));   // last SNR x4
        put16(44, 41); put16(46, 377);      // direct / flood duplicates
        put32(48, 41 * 60); put32(52, 12);  // rx airtime, receive errors (a repeater's extras)
        _ev->beStatusReply(p.key, s, sizeof s);
        break;
      }
      case Pending::Trace:  _ev->beTraceResult(p.value, (int8_t)(5 + random(4)), (int8_t)(6 + random(4))); break;
      case Pending::Login: {
        std::array<uint8_t, PUB_KEY_SIZE> k; memcpy(k.data(), p.key, PUB_KEY_SIZE);
        _logged_in.push_back(k);
        _ev->beLoginInfo(p.key, p.value ? 3 : 0, 2);   // ACL role admin / guest, firmware level 2
        _ev->beLoginResult(p.key, true, p.value ? 1 : 0, (uint32_t)time(nullptr));
        break;
      }
      case Pending::Binary: fireBinary(p); break;
      case Pending::Cli: {
        const std::string& c = p.text;
        std::string out;
        if (c == "ver") out = "v1.18.0 (demo)";
        else if (c == "clock") out = "> the repeater's clock matches yours";
        else if (c.rfind("get ", 0) == 0) {
          const std::string k = c.substr(4);
          out = k == "name" ? "> Hilltop Repeater" : k == "radio" ? "> 869.525,250.0,11,5"
              : k == "tx" ? "> 22" : k == "advert.interval" ? "> 120" : k == "flood.max" ? "> 64"
              : k == "repeat" ? "> on" : k == "owner.info" ? "> Run by the demo club" : "> (demo) " + k;
        } else if (c.rfind("set ", 0) == 0) out = "OK";
        else out = "Error: the demo repeater only knows get, set, ver and clock";
        _ev->beCliReply(p.key, out.c_str());
        break;
      }
    }
  }
  // The repeater's answers to the admin app's binary requests (RepeaterAdminProto.h).
  void fireBinary(const Pending& p) {
    const uint8_t* q = (const uint8_t*)p.text.data();
    const size_t ql = p.text.size();
    std::vector<uint8_t> r;
    auto u16 = [&](uint16_t v) { r.push_back((uint8_t)v); r.push_back((uint8_t)(v >> 8)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) r.push_back((uint8_t)(v >> (8 * i))); };
    switch (ql ? q[0] : 0) {
      case 0x06: {   // GET_NEIGHBOURS: count at [2], offset [3..4], prefix length [6]
        const uint8_t plen = (ql > 6 && q[6] >= 1 && q[6] <= 8) ? q[6] : 4;
        static const char* const kNb[] = { "Sanne", "Diego", "Ridge Repeater", "Harbor Room", "Lars", "Femke" };
        static const uint32_t kAgo[] = { 95, 1500, 380, 640, 140, 3 * 86400 };
        static const int8_t kSnr[] = { 34, 10, 22, -6, 14, -18 };
        const int n = 6;
        u16(n); u16(n);
        for (int i = 0; i < n; ++i) {
          uint8_t k[PUB_KEY_SIZE]; demoKey(kNb[i], k);
          r.insert(r.end(), k, k + plen);
          u32(kAgo[i]);
          r.push_back((uint8_t)kSnr[i]);
        }
        break;
      }
      case 0x05: {   // GET_ACCESS_LIST: 6-byte prefix + permissions
        static const char* const kAcl[] = { NODE_NAME, "Sanne", "Diego" };
        static const uint8_t kPerm[] = { 3, 2, 1 };
        for (int i = 0; i < 3; ++i) {
          uint8_t k[PUB_KEY_SIZE]; demoKey(kAcl[i], k);
          r.insert(r.end(), k, k + 6);
          r.push_back(kPerm[i]);
        }
        break;
      }
      case 0x07: {   // GET_OWNER_INFO
        static const char kOwner[] = "v1.18.0\nHilltop Repeater\nRun by the demo club. Solar, 6 dBi on the water tower.";
        r.insert(r.end(), kOwner, kOwner + sizeof kOwner - 1);
        break;
      }
      default: return;   // unknown request: no answer, as a repeater
    }
    _ev->beBinaryReply(p.key, r.data(), r.size());
  }
  void queue(Pending::Kind k, const uint8_t key[PUB_KEY_SIZE], uint32_t value, uint32_t delay_ms) {
    Pending p{};
    p.due_ms = millis() + delay_ms; p.kind = k; p.value = value;
    memcpy(p.key, key, PUB_KEY_SIZE);
    _pending.push_back(p);
  }
  uint32_t nextTag() { uint32_t t = 0; while (!t) t = (uint32_t)random(0x7fffffff) ^ 0x5741d0e0u; return t; }

  MeshBackendEvents* _ev = nullptr;
  bool _started = false, _seeded = false;
  uint32_t _t0 = 0, _next_chatter = 0, _next_advert = 0, _chatter_every = 0, _chatter_burst = 0;
  unsigned _chatter_i = 0, _advert_i = 0, _reply_i = 0;
  std::vector<Pending> _pending;
  std::vector<std::array<uint8_t, PUB_KEY_SIZE>> _logged_in;
};

}  // namespace

MeshBackend* wadaCreateDemoBackend() { return new DemoBackend(); }
