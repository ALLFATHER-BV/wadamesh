#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/MeshTouchTxTrace.h>
#include "AbstractUITask.h"

// Node-discovery control packet (standard MeshCore — matches examples/simple_repeater
// + simple_sensor): a zero-hop CTL_TYPE_NODE_DISCOVER_REQ that neighbouring repeaters
// answer DIRECTLY with a NODE_DISCOVER_RESP. Used by the touch signal probe.
#define CTL_TYPE_NODE_DISCOVER_REQ    0x80   // upper nibble = type; low bit = prefix_only
#define CTL_TYPE_NODE_DISCOVER_RESP   0x90

// Operational wire-debug. The companion link is a BINARY protocol over Serial (USB-CDC); writing raw
// text to Serial during a session interleaves into that stream and corrupts it — the MeshCore app
// then throws "Bad state: Streamsink is bound to a stream" and the connect / room login / repeater
// admin all fail (GH #25, #54, #23). Compiled out in release; build -DCOMPANION_WIRE_DEBUG=1 to get
// these traces back (only safe over a non-companion debug console).
#ifndef COMPANION_WIRE_DEBUG
#define COMPANION_WIRE_DEBUG 0
#endif
#if COMPANION_WIRE_DEBUG
#define WIRE_DBG(...) Serial.printf(__VA_ARGS__)
#else
#define WIRE_DBG(...) do {} while (0)
#endif

/*------------ Frame Protocol --------------*/
// Keep the fork's companion-protocol lineage (27 — a superset of upstream's 13)
// so the existing meshcomod web/phone client still matches. 1.16's new companion
// commands (raw packet, un-scoped flood, anon req) are added as handlers below;
// advertising them by bumping this is a coordinated client change for later.
#define FIRMWARE_VER_CODE 27

#ifndef FIRMWARE_BUILD_DATE
#define FIRMWARE_BUILD_DATE "7 Jun 2026"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "v1.16.0-touch"
#endif

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
#include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
#include <LittleFS.h>
#elif defined(ESP32)
#include <SPIFFS.h>
#endif

#include "DataStore.h"
#include "NodePrefs.h"

/* Synthetic local command contact: appears in contact list, messages to it are intercepted and never sent over mesh. */
#define MESHCOMOD_NAME "Meshcomod"

#include <RTClib.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/BaseSerialInterface.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <target.h>

/* ---------------------------------- CONFIGURATION ------------------------------------- */

#ifndef LORA_FREQ
#define LORA_FREQ 915.0
#endif
#ifndef LORA_BW
#define LORA_BW 250
#endif
#ifndef LORA_SF
#define LORA_SF 10
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
#define LORA_TX_POWER 20
#endif
#ifndef MAX_LORA_TX_POWER
#define MAX_LORA_TX_POWER LORA_TX_POWER
#endif

#ifndef MAX_CONTACTS
#define MAX_CONTACTS 100
#endif

#ifndef OFFLINE_QUEUE_SIZE
#define OFFLINE_QUEUE_SIZE 16
#endif

#ifndef HISTORY_RING_SIZE
#define HISTORY_RING_SIZE 128
#endif
#ifndef MAX_HISTORY_CLIENTS
#define MAX_HISTORY_CLIENTS 8
#endif
#ifndef MAX_CLIENT_ID_LEN
#define MAX_CLIENT_ID_LEN 31
#endif

#ifndef BLE_NAME_PREFIX
#define BLE_NAME_PREFIX "MeshCore-"
#endif

#include <helpers/BaseChatMesh.h>
#include <helpers/TransportKeyStore.h>

/* -------------------------------------------------------------------------------------- */

#define REQ_TYPE_GET_STATUS             0x01 // same as _GET_STATS
#define REQ_TYPE_KEEP_ALIVE             0x02
#define REQ_TYPE_GET_TELEMETRY_DATA     0x03

struct AdvertPath {
  uint8_t pubkey_prefix[7];
  uint8_t path_len;
  char    name[32];
  uint32_t recv_timestamp;
  uint8_t path[MAX_PATH_SIZE];
};

class MyMesh : public BaseChatMesh, public DataStoreHost {
public:
  MyMesh(mesh::Radio &radio, mesh::RNG &rng, mesh::RTCClock &rtc, SimpleMeshTables &tables, DataStore& store, AbstractUITask* ui=NULL);

  void begin(bool has_display);
  void startInterface(BaseSerialInterface &serial);

  const char *getNodeName();
  NodePrefs *getNodePrefs();
  uint32_t getBLEPin();
  bool     setBLEPin(uint32_t pin);   // user-chosen 6-digit pairing code (persisted; applies next boot)

  // Live device info accessors (used by the touch Settings → Device modal to
  // mirror the web client's "Device (live)" panel — public key prefix, channel
  // count, etc.). self_id and num_channels live in `protected:` of the base
  // classes, so we expose narrow getters here.
  const uint8_t *getSelfPubKey() const { return self_id.pub_key; }
  /** Export this node's 64-byte private key (settings backup). */
  size_t uiExportPrivKey(uint8_t dest[64]) { return self_id.writeTo(dest, 64); }
  /** Import a 64-byte private key from a settings backup: validate, persist,
   *  swap the active identity, reload contacts (invalidate shared secrets).
   *  Returns false on an invalid key or storage failure. */
  bool uiImportPrivKey(const uint8_t src[64]) {
    if (!mesh::LocalIdentity::validatePrivateKey(src)) return false;
    mesh::LocalIdentity identity;
    identity.readFrom(src, 64);
    if (!_store->saveMainIdentity(identity)) return false;
    self_id = identity;
    resetContacts();
    _store->loadContacts(this);
    return true;
  }
  /** Add/merge a contact from a settings backup (name + pubkey + flags + GPS).
   *  Returns false if the slot table is full. Mirrors how addContact works. */
  bool uiAddContactFromBackup(const uint8_t pub_key[32], const char* name, uint8_t type,
                              uint8_t flags, int32_t lat, int32_t lon,
                              uint32_t last_advert, uint32_t lastmod) {
    ContactInfo c{};
    memcpy(c.id.pub_key, pub_key, 32);
    c.type = type; c.flags = flags;
    c.gps_lat = lat; c.gps_lon = lon;
    c.last_advert_timestamp = last_advert; c.lastmod = lastmod;
    c.out_path_len = OUT_PATH_UNKNOWN;
    strncpy(c.name, name ? name : "", sizeof(c.name) - 1);
    return addContact(c);
  }
  // Channel count by iterating slots — `num_channels` is private in the base.
  // A channel slot is in-use when its name is non-empty (matches the lookup
  // pattern used elsewhere in the firmware).
  int getNumChannels() {
#ifdef MAX_GROUP_CHANNELS
    int n = 0;
    for (int i = 0; i < MAX_GROUP_CHANNELS; ++i) {
      ChannelDetails cd{};
      if (getChannel(i, cd) && cd.name[0] != '\0') ++n;
    }
    return n;
#else
    return 0;
#endif
  }

  void loop();
  void handleCmdFrame(size_t len);

  // ---- Settings backup (MeshCore-app-compatible JSON) ----
  /** Write the full backup JSON (name, public/private key, radio + position
   *  settings, channels, contacts) to `out`. Matches the stock MeshCore app
   *  export shape so the file opens in the app / web client. */
  void uiExportBackup(Print& out, double node_lat, double node_lon);
  /** Apply a parsed backup. `sections` bit flags choose what to apply:
   *  bit0=identity(name+key) bit1=radio bit2=position bit3=channels bit4=contacts.
   *  `replace_*` clears existing channels/contacts first. Returns false on a
   *  malformed document. Fills counts (channels/contacts applied) if non-null. */
  bool uiImportBackup(Stream& in, uint8_t sections,
                      bool replace_channels, bool replace_contacts,
                      int* out_channels, int* out_contacts);
  /** Factory reset: wipe ALL persistent data — formats the internal filesystem
   *  (identity, contacts, channels, saved messages, exported backups) and erases
   *  NVS (Wi-Fi credentials + every setting). The caller MUST reboot immediately
   *  after; a fresh identity is generated on the next boot. Returns false if the
   *  format/erase failed. */
  bool uiFactoryReset() { return _store->formatFileSystem(); }
  bool advert();
  /** Send advert with explicit routing: true = flood (multi-hop), false = zero-hop.
   *  `advert()` (zero-hop) is kept for backward compatibility. */
  bool sendAdvert(bool flood);
  void enterCLIRescue();

  int  getRecentlyHeard(AdvertPath dest[], int max_num);

  // On-device terminal: register an output sink (nullptr = off) and run a local
  // CLI command. Replies flow through pushMeshcomodReply -> the sink.
  static void setTerminalSink(void (*cb)(const char* line));
  void runLocalCli(const char* cmd);

#if defined(ESP32) && (defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION))
  /** HTTP OTA: one UTF-8 line as PUSH_CODE_BINARY_RESPONSE (ESP32Board → meshcoreRepeaterTcpOtaEmitLine). */
  void pushCompanionOtaProgressLine(const char* line);
#endif

protected:
  float getAirtimeBudgetFactor() const override;
  int getInterferenceThreshold() const override;
  int calcRxDelay(float score, uint32_t air_time) const override;
  uint8_t getExtraAckTransmitCount() const override;
  uint8_t getAutoAddMaxHops() const override;
  bool filterRecvFloodPacket(mesh::Packet* packet) override;
  bool allowPacketForward(const mesh::Packet* packet) override;

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis);
  // Meshcomod: synthetic local command contact (no RF)
  static const uint8_t MESHCOMOD_PUB_KEY_PREFIX[6];
  void getMeshcomodContact(ContactInfo& dest);
  bool isMeshcomodRecipient(const uint8_t* pub_key_prefix_6) const;
  bool handleMeshcomodCommand(const char* text, int text_len);
  void pushMeshcomodReply(const char* text, bool immediate_current = false);

  void sendFloodScoped(const ContactInfo& recipient, mesh::Packet* pkt, uint32_t delay_millis=0) override;
  void sendFloodScoped(const mesh::GroupChannel& channel, mesh::Packet* pkt, uint32_t delay_millis=0) override;

  void logRxRaw(float snr, float rssi, const uint8_t raw[], int len) override;
  bool isAutoAddEnabled() const override;
  bool shouldAutoAddContactType(uint8_t type) const override;
  bool shouldOverwriteWhenFull() const override;
  void onContactsFull() override;
  void onContactOverwrite(const uint8_t* pub_key) override;
  bool onContactPathRecv(ContactInfo& from, uint8_t* in_path, uint8_t in_path_len, uint8_t* out_path, uint8_t out_path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len, const uint8_t* path) override;
  void onContactPathUpdated(const ContactInfo &contact) override;
  ContactInfo* processAck(const uint8_t *data) override;
  void queueMessage(const ContactInfo &from, uint8_t txt_type, mesh::Packet *pkt, uint32_t sender_timestamp,
                    const uint8_t *extra, int extra_len, const char *text);

  void onMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                     const char *text) override;
  void onCommandDataRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                         const char *text) override;
  void onSignedMessageRecv(const ContactInfo &from, mesh::Packet *pkt, uint32_t sender_timestamp,
                           const uint8_t *sender_prefix, const char *text) override;
  void onChannelMessageRecv(const mesh::GroupChannel &channel, mesh::Packet *pkt, uint32_t timestamp,
                            const char *text) override;

  uint8_t onContactRequest(const ContactInfo &contact, uint32_t sender_timestamp, const uint8_t *data,
                           uint8_t len, uint8_t *reply) override;
  void onContactResponse(const ContactInfo &contact, const uint8_t *data, uint8_t len) override;
  void onControlDataRecv(mesh::Packet *packet) override;
  void onRawDataRecv(mesh::Packet *packet) override;
  void onTraceRecv(mesh::Packet *packet, uint32_t tag, uint32_t auth_code, uint8_t flags,
                   const uint8_t *path_snrs, const uint8_t *path_hashes, uint8_t path_len) override;

  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override;
  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis, uint8_t path_len) const override;
  void onSendTimeout() override;

  // DataStoreHost methods
  bool onContactLoaded(const ContactInfo& contact) override { return addContact(contact); }
  bool getContactForSave(uint32_t idx, ContactInfo& contact) override { return getContactByIdx(idx, contact); }
  bool onChannelLoaded(uint8_t channel_idx, const ChannelDetails& ch) override { return setChannel(channel_idx, ch); }
  bool getChannelForSave(uint8_t channel_idx, ChannelDetails& ch) override { return getChannel(channel_idx, ch); }

  void clearPendingReqs() {
    pending_login = pending_status = pending_telemetry = pending_discovery = pending_req = 0;
  }

public:
  /** Which kind of touch-UI request is currently in flight, so the response
   *  matcher in onContactResponse routes to the right callback. */
  enum class UiReqKind : uint8_t { None = 0, Status = 1, Telemetry = 2 };

  /** Fire a REQ_TYPE_GET_STATUS from the touch UI side. Result is delivered
   *  via AbstractUITask::onPingReply when the reply arrives.
   *  Returns the MSG_SEND_* result code. */
  int sendStatusPingForUI(ContactInfo& recipient) {
    uint32_t tag = 0, est = 0;
    /* Clear any stale serial-side status pending so the legacy match doesn't
     * eat our reply before the UI hook sees it. */
    pending_status = 0;
    int r = sendRequest(recipient, REQ_TYPE_GET_STATUS, tag, est);
    if (r == MSG_SEND_SENT_FLOOD || r == MSG_SEND_SENT_DIRECT) {
      memcpy(&_ui_pending_status, recipient.id.pub_key, 4);
      _ui_pending_kind = UiReqKind::Status;
      _ui_pending_tag  = tag;   // request tag, reflected by the repeater
    }
    return r;
  }

  /** Same as sendStatusPingForUI, but sends an unauthenticated guest LOGIN
   *  first. Repeaters need the sender to be in their ACL before they will
   *  decrypt a PAYLOAD_TYPE_REQ; the ACL is only populated by handleLoginReq.
   *  An empty-password sendLogin matches repeaters whose guest_password is
   *  blank (the typical out-of-box default) and adds us as a guest. The
   *  dispatcher serializes outbound TX, so the LOGIN ANON_REQ reaches the
   *  destination before the STATUS REQ that follows.
   *  No-op if the LOGIN packet pool is empty; falls through to send the
   *  STATUS REQ anyway so a repeater that already knows us still replies. */
  int sendStatusPingWithGuestLoginForUI(ContactInfo& recipient) {
    uint32_t login_est = 0;
    sendLogin(recipient, "", login_est);
    return sendStatusPingForUI(recipient);
  }

  /** Same chained-login flavour for telemetry — repeaters and sensors also
   *  require ACL membership for REQ_TYPE_GET_TELEMETRY_DATA. */
  int sendTelemetryRequestWithGuestLoginForUI(ContactInfo& recipient) {
    uint32_t login_est = 0;
    sendLogin(recipient, "", login_est);
    return sendTelemetryRequestForUI(recipient);
  }

  /** Touch-UI manual STATUS/TELEMETRY request that DEFERS the REQ until the
   *  guest LOGIN is acknowledged. The chained helpers above fire LOGIN and REQ
   *  back-to-back, but a repeater drops a PAYLOAD_TYPE_REQ from a sender it
   *  hasn't ACL'd yet — and on first contact the ACL entry isn't committed by
   *  the time the REQ is processed, so the first request usually gets no reply
   *  (the user had to tap twice). This sends ONLY the blank-password LOGIN now
   *  and arms _ui_login_then; onContactResponse issues the REQ once the
   *  LOGIN-OK lands, by which point we're in the ACL and a direct out_path has
   *  been learned, so the REQ decrypts on the first try.
   *  Returns the LOGIN's MSG_SEND_* result — the UI shows "requesting…" on a
   *  successful send and arms its own reply deadline. Manual paths only;
   *  auto-poll keeps the immediate chained send above (a single arm slot can't
   *  serve its multi-node loop, and a missed poll just retries next interval). */
  int uiSendRequestAfterGuestLogin(ContactInfo& recipient, UiReqKind kind) {
    uint32_t login_est = 0;
    int r = sendLogin(recipient, "", login_est);
    if (r == MSG_SEND_SENT_FLOOD || r == MSG_SEND_SENT_DIRECT) {
      memcpy(&_ui_login_then, recipient.id.pub_key, 4);
      _ui_login_then_kind = kind;
    } else {
      cancelUIDeferredLogin();
    }
    return r;
  }

  /** Admin login for the touch UI repeater admin console.
   *  Sends a sendLogin with the given password (empty = guest), records
   *  pending_login so onContactResponse's existing login branch can route
   *  the response. The same branch now also fires
   *  AbstractUITask::onAdminLoginResult so the UI can flip from "logging
   *  in…" to "logged in" (or "failed"). */
  int uiSendAdminLogin(ContactInfo& recipient, const char* password) {
    uint32_t est = 0;
    int r = sendLogin(recipient, password ? password : "", est);
    if (r == MSG_SEND_SENT_FLOOD || r == MSG_SEND_SENT_DIRECT) {
      memcpy(&pending_login, recipient.id.pub_key, 4);
    }
    // Diagnostic (room-server login trace): which contact/type we sent the login
    // to and the send result. Pair with the "[ROOM] login resp" line in
    // onContactResponse to see exactly where a room login breaks.
    WIRE_DBG("[ROOM] login send '%s' type=%u pwlen=%u sync_since=%lu -> r=%d\n",
                  recipient.name, (unsigned)recipient.type,
                  (unsigned)(password ? strlen(password) : 0),
                  (unsigned long)recipient.sync_since, r);
    return r;
  }

  /** Is a keep-alive session to this server currently armed and alive?
   *  (Room chat UI: the core frees the slot 2.5x the interval after the last
   *  ACK/activity, so false = the session needs a re-login.) */
  bool uiHasConnectionTo(const uint8_t pub_key[32]) { return hasConnectionTo(pub_key); }

  /** Re-login to a room server WITHOUT a password. For a client already in the
   *  room's ACL a blank login replies LOGIN_OK with the stored permissions and
   *  skips the login replay guard (simple_room_server onAnonDataRecv, blank-
   *  password branch) — and when it arrives flooded the server also resets its
   *  stale out_path to us. The blank login itself resets NOTHING else server-side
   *  (not push_failures, not last_activity); the actual heal is the chain it
   *  starts: LOGIN_OK -> onContactResponse arms the room keep-alive -> the first
   *  REQ_TYPE_KEEP_ALIVE refreshes the server's last_activity and clears its
   *  push-abandon counter, so pushes resume (issue #89: self-heal + the chat
   *  sheet's "Log in again"). NOTE it cannot recover a server that REBOOTED
   *  (non-admin ACL entries aren't persisted there) — that needs a passworded
   *  Join from the Contacts sheet. */
  int uiRoomRelogin(const uint8_t pub_key[32]) {
    ContactInfo* c = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (!c || c->type != ADV_TYPE_ROOM) return MSG_SEND_FAILED;
    return uiSendAdminLogin(*c, "");
  }

  /** Send a CLI command line to a previously-logged-in repeater / room
   *  server. The reply lands in AbstractUITask::onAdminCommandReply via
   *  queueMessage when the server returns a TXT_TYPE_CLI_DATA frame.
   *  No-op if recipient isn't in our contacts; returns MSG_SEND_FAILED. */
  int uiSendAdminCommand(ContactInfo& recipient, const char* text) {
    if (!text || !text[0]) return MSG_SEND_FAILED;
    uint32_t est = 0;
    uint32_t ts = getRTCClock()->getCurrentTimeUnique();
    return sendCommandData(recipient, ts, 0, text, est);
  }
  /** Cancel UI ping pending state (used by timeout). */
  void cancelUIPingPending() {
    _ui_pending_status = 0;
    _ui_pending_kind   = UiReqKind::None;
    _ui_pending_tag    = 0;
  }
  /** Disarm a deferred guest-login-then-request (see uiSendRequestAfterGuestLogin)
   *  so a late LOGIN-OK can't fire a REQ after the UI gave up. Kept separate from
   *  cancelUIPingPending() so a ping timeout never disarms a telemetry request. */
  void cancelUIDeferredLogin() {
    _ui_login_then      = 0;
    _ui_login_then_kind = UiReqKind::None;
  }
  /** True if a UI ping is still waiting on a reply. */
  bool hasUIPingPending() const { return _ui_pending_status != 0; }

  /** Register an expected ACK hash that came out of a touch-UI sendMessage
   *  call, so MyMesh::processAck can match the inbound ACK and dispatch
   *  onMessageAcked back to the UI. The companion-serial CMD_SEND_TXT_MSG
   *  handler already does this for app-originated messages; the touch UI
   *  reaches sendMessage directly and skipped this until now. */
  void uiRegisterExpectedAck(uint32_t expected_ack, const uint8_t pub_key[32]) {
    if (expected_ack == 0) return;
    ContactInfo* c = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (!c) return;
    expected_ack_table[next_ack_idx].msg_sent = _ms->getMillis();
    expected_ack_table[next_ack_idx].ack = expected_ack;
    expected_ack_table[next_ack_idx].contact = c;
    // EXPECTED_ACK_TABLE_SIZE is #defined further down in this header next
    // to the table itself; hard-code 8 here so this inline helper compiles
    // wherever it's used.
    next_ack_idx = (next_ack_idx + 1) % 8;
  }

  // ---- "Repeats heard" for sent floods + route of the last received flood ----
  // When we originate a flood TXT, repeaters re-broadcast it and our own radio
  // hears the echoes (same payload, longer path). We fingerprint the payload at
  // send time (sendFloodScoped) and match echoes in logRxRaw, counting repeats
  // per recent send. The touch UI stamps its outgoing bubble with the
  // fingerprint (uiLastSentFp) and later reads the count (uiRepeatsForFp).
  // _last_rx_path holds the path hashes of the just-received flood so the UI's
  // newMsg* handler (called synchronously next) can stash the inbound route.
  static const int UI_ECHO_SLOTS = 12;
  uint32_t _echo_fp[UI_ECHO_SLOTS]  = {0};
  uint8_t  _echo_rep[UI_ECHO_SLOTS] = {0};
  static const int ECHO_MAX_HOPS = 3;                         // repeaters remembered per echoed send
  uint8_t  _echo_hop[UI_ECHO_SLOTS][ECHO_MAX_HOPS][4] = {};   // last-hop hash of each distinct echo
  uint8_t  _echo_hop_sz[UI_ECHO_SLOTS] = {0};                 // hash size used for those hops
  uint8_t  _echo_hop_n[UI_ECHO_SLOTS]  = {0};                 // count, 0..ECHO_MAX_HOPS
  uint8_t  _echo_idx = 0;
  uint32_t _last_sent_fp = 0;
  uint8_t  _last_rx_path[32] = {0};
  uint8_t  _last_rx_path_n  = 0;
  uint16_t _last_rx_scope     = 0;     // transport_codes[0] of the last RX flood ("scope")
  bool     _last_rx_has_scope = false; // false if the packet carried no transport codes
  uint32_t _last_sender_ts    = 0;     // embedded send-time of the last inbound msg (UI bubble ts; 0 = use now)
  volatile bool _echo_dirty = false;   // a repeat was counted -> UI should refresh

  // ---- Live signal strength (top-bar icon) ----
  // Updated on EVERY received packet in logRxRaw(): the SNR (×4) + millis() at RX.
  // The UI maps it to bars and dims them when nothing's been heard for a while.
  int8_t   _ui_sig_snr_q4 = -128;   // SNR_dB * 4 of the last RX (-128 = nothing yet)
  int8_t   _ui_sig_rssi   = 0;      // RSSI dBm of the last RX
  uint32_t _ui_sig_ms     = 0;      // millis() at that RX (0 = nothing heard yet)
  int8_t   uiSignalSnrQ4() const { return _ui_sig_snr_q4; }
  int8_t   uiSignalRssi()  const { return _ui_sig_rssi; }
  uint32_t uiSignalMs()    const { return _ui_sig_ms; }

  // ---- Recent-RX ring (RF Monitor app) ----
  // One record per received frame, captured in logRxRaw(): payload type / route
  // / hop count / length + signal, so the Monitor page can show a live "what am
  // I hearing" feed without a core hook. Newest-first via uiRxLogGet (i=0 = most
  // recent). Board-independent — populated on every board, not just touch.
  struct UiRxRec {
    uint32_t ms;        // millis() at RX
    int8_t   rssi;      // dBm
    int8_t   snr_q4;    // SNR_dB * 4
    uint8_t  ptype;     // payload type  (raw[0]>>2)&0x0F
    uint8_t  route;     // route type    raw[0]&0x03
    uint8_t  hops;      // path length carried (0 = heard direct from origin)
    uint8_t  len;       // frame length (clamped to 255)
  };
  static const int UI_RXLOG_MAX = 16;
  UiRxRec  _ui_rxlog[UI_RXLOG_MAX];
  uint8_t  _ui_rxlog_head = 0;        // next write slot
  uint8_t  _ui_rxlog_cnt  = 0;        // valid entries (<= UI_RXLOG_MAX)
  uint8_t  uiRxLogCount() const { return _ui_rxlog_cnt; }
  bool     uiRxLogGet(uint8_t i, UiRxRec& out) const {
    if (i >= _ui_rxlog_cnt) return false;
    uint8_t idx = (uint8_t)((_ui_rxlog_head + UI_RXLOG_MAX - 1 - i) % UI_RXLOG_MAX);
    out = _ui_rxlog[idx];
    return true;
  }
  // Record a reception into the ring (called from logRxRaw).
  void uiRxLogPush(uint32_t ms, int8_t rssi, int8_t snr_q4,
                   uint8_t ptype, uint8_t route, uint8_t hops, uint8_t len) {
    UiRxRec& r = _ui_rxlog[_ui_rxlog_head];
    r.ms = ms; r.rssi = rssi; r.snr_q4 = snr_q4;
    r.ptype = ptype; r.route = route; r.hops = hops; r.len = len;
    _ui_rxlog_head = (uint8_t)((_ui_rxlog_head + 1) % UI_RXLOG_MAX);
    if (_ui_rxlog_cnt < UI_RXLOG_MAX) _ui_rxlog_cnt++;
  }

  /** Fingerprint of the most-recently originated flood TXT payload (0 if none). */
  uint32_t uiLastSentFp() const { return _last_sent_fp; }

  /** Echoes (repeater re-broadcasts) heard of the flood TXT with this payload
   *  fingerprint. 0 if unknown / evicted from the ring. */
  uint8_t uiRepeatsForFp(uint32_t fp) const {
    if (fp == 0) return 0;
    for (int i = 0; i < UI_ECHO_SLOTS; i++) if (_echo_fp[i] == fp) return _echo_rep[i];
    return 0;
  }

  /** How many distinct repeaters we captured echoing this sent fingerprint. */
  uint8_t uiRepeatHopCount(uint32_t fp) const {
    if (fp == 0) return 0;
    for (int i = 0; i < UI_ECHO_SLOTS; i++) if (_echo_fp[i] == fp) return _echo_hop_n[i];
    return 0;
  }
  /** Copy the idx-th captured repeater hash for `fp` into out[] (needs >= 4 bytes);
   *  returns the hash size (0 if none). Bounded, read-only. */
  uint8_t uiRepeatHop(uint32_t fp, uint8_t idx, uint8_t* out, uint8_t out_sz) const {
    if (fp == 0 || !out) return 0;
    for (int i = 0; i < UI_ECHO_SLOTS; i++) if (_echo_fp[i] == fp) {
      if (idx >= _echo_hop_n[i]) return 0;
      uint8_t sz = _echo_hop_sz[i]; if (sz > out_sz) sz = out_sz; if (sz > 4) sz = 4;
      memcpy(out, _echo_hop[i][idx], sz);
      return sz;
    }
    return 0;
  }

  /** Path (repeater hashes) of the most-recently received flood; copies up to
   *  `max` bytes into buf, returns the count. Read synchronously from the
   *  newMsg* handler that follows reception. */
  uint8_t lastRxPath(uint8_t* buf, uint8_t max) const {
    uint8_t n = _last_rx_path_n < max ? _last_rx_path_n : max;
    if (buf && n) memcpy(buf, _last_rx_path, n);
    return n;
  }
  /** Scope (transport_codes[0]) of the last received flood; *has = false when
   *  the packet carried no transport codes. */
  uint16_t lastRxScope(bool* has) const { if (has) *has = _last_rx_has_scope; return _last_rx_scope; }
  /** Consume the embedded send-time stashed right before the last UI notify (room
   *  history replay carries old send-times; without this the UI stamps "now").
   *  Returns 0 when nothing was stashed -> the caller keeps the delivery time. */
  uint32_t uiConsumeLastSenderTs() { uint32_t t = _last_sender_ts; _last_sender_ts = 0; return t; }
  /** Capture route + scope of a just-received flood for the Info popup. Call
   *  synchronously right before the newMsg* notification. */
  void uiStashRxMeta(mesh::Packet* pkt) {
    _last_rx_path_n = 0;
    if (pkt && pkt->isRouteFlood()) {
      int nb = (int)pkt->getPathHashCount() * (int)pkt->getPathHashSize();
      if (nb > (int)sizeof(_last_rx_path)) nb = (int)sizeof(_last_rx_path);
      if (nb > 0) { memcpy(_last_rx_path, pkt->path, nb); _last_rx_path_n = (uint8_t)nb; }
    }
    const uint8_t rt = pkt ? pkt->getRouteType() : 0xFF;
    _last_rx_has_scope = (rt == ROUTE_TYPE_TRANSPORT_FLOOD || rt == ROUTE_TYPE_TRANSPORT_DIRECT);
    _last_rx_scope = (_last_rx_has_scope && pkt) ? pkt->transport_codes[0] : 0;
  }

  /** Track a freshly-sent flood TXT fingerprint (called from sendFloodScoped). */
  void uiTrackSentFp(uint32_t fp) {
    if (fp == 0) return;
    _last_sent_fp = fp;
    for (int i = 0; i < UI_ECHO_SLOTS; i++) if (_echo_fp[i] == fp) { _echo_rep[i] = 0; _echo_hop_n[i] = 0; return; }
    _echo_fp[_echo_idx] = fp; _echo_rep[_echo_idx] = 0; _echo_hop_n[_echo_idx] = 0;
    _echo_idx = (uint8_t)((_echo_idx + 1) % UI_ECHO_SLOTS);
  }

  /** Count one echo of fingerprint `fp` (called from logRxRaw on a match). `hop`
   *  (optional) is the re-flooding repeater's hash — the echo's last path hop —
   *  recorded deduped + bounded so the sent-message Info can name the repeaters. */
  /** Returns true when `fp` matched one of OUR recent sends (i.e. this RX is an
   *  echo of our own flood) — logRxRaw uses that to let the frame through to BLE
   *  for the app's "Repeats heard" (issue #94). */
  bool uiCountEcho(uint32_t fp, const uint8_t* hop = nullptr, uint8_t hop_sz = 0) {
    for (int i = 0; i < UI_ECHO_SLOTS; i++)
      if (_echo_fp[i] == fp) {
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
        return true;
      }
    return false;
  }
  /** True once if a repeat was counted since the last call — the UI uses this
   *  to refresh the chat so the bubble's repeat tag updates live. */
  bool takeEchoDirty() { bool d = _echo_dirty; _echo_dirty = false; return d; }

  /** UI: resolve a path-hop hash prefix to its (repeater) contact name. Bounded
   *  + null-safe; writes out[] NUL-terminated and returns true ONLY on a named
   *  match (otherwise the caller shows the bare hash). Read-only contact lookup,
   *  so it can't corrupt anything even if called mid-RX. */
  bool uiHopName(const uint8_t* hash, int prefix_len, char* out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    if (!hash || prefix_len <= 0) return false;
    ContactInfo* c = lookupContactByPubKey(hash, prefix_len);
    if (!c || c->name[0] == '\0') return false;
    strncpy(out, c->name, out_sz - 1);
    out[out_sz - 1] = '\0';
    return true;
  }

  /** Resolve a path-hop hash prefix to the repeater's advertised position.
   *  Returns true + fills lat/lon (degrees) when the contact is known AND has a
   *  non-zero GPS position; false otherwise. Read-only — safe from the UI. */
  bool uiHopPos(const uint8_t* hash, int prefix_len, double* out_lat, double* out_lon) {
    if (!hash || prefix_len <= 0) return false;
    ContactInfo* c = lookupContactByPubKey(hash, prefix_len);
    if (!c) return false;
    if (c->gps_lat == 0 && c->gps_lon == 0) return false;
    if (out_lat) *out_lat = (double)c->gps_lat / 1.0e6;
    if (out_lon) *out_lon = (double)c->gps_lon / 1.0e6;
    return true;
  }

  /** Send a 0-hop trace ping to a single neighbor (typically a repeater).
   *  Returns the trace tag we chose (non-zero on success) so the UI can
   *  match the onTracePingResult callback. The trace path is just the
   *  neighbor's hash; when they retransmit, our radio picks it up and the
   *  dispatcher fires onTraceRecv at us with the SNRs in both directions.
   *  Returns 0 on failure. */
  uint32_t uiSendTracePing(const uint8_t pub_key[32]) {
    if (!pub_key) return 0;
    uint8_t hash_sz = (uint8_t)(_prefs.path_hash_mode + 1);
    if (hash_sz == 0 || hash_sz > 4) hash_sz = 1;
    uint8_t flags = (uint8_t)(_prefs.path_hash_mode & 0x03);
    uint32_t tag = 0;
    getRNG()->random((uint8_t*)&tag, sizeof(tag));
    if (tag == 0) tag = 1;
    // Stash so onTraceRecv can route the right callback to the UI.
    _ui_trace_ping_tag = tag;
    mesh::Packet* pkt = createTrace(tag, 0, flags);
    if (!pkt) { _ui_trace_ping_tag = 0; return 0; }
    sendDirect(pkt, pub_key, hash_sz);
    return tag;
  }

  /** Send a FULL-ROUTE trace toward `c` along its entire known path, so every
   *  repeater on the way appends its RX-SNR (vs uiSendTracePing, which only
   *  probes the immediate neighbour). The result arrives via the same
   *  onTracePingResult callback, but with one SNR reading per hop. Falls back
   *  to a single-hop trace when the path to `c` is unknown (flood-routed
   *  contact — there's no fixed path to walk). Returns the tag, 0 on failure. */
  uint32_t uiSendTraceRoute(const ContactInfo& c) {
    uint8_t flags = (uint8_t)(_prefs.path_hash_mode & 0x03);
    uint32_t tag = 0;
    getRNG()->random((uint8_t*)&tag, sizeof(tag));
    if (tag == 0) tag = 1;
    _ui_trace_ping_tag = tag;
    mesh::Packet* pkt = createTrace(tag, 0, flags);
    if (!pkt) { _ui_trace_ping_tag = 0; return 0; }
    if (c.out_path_len != OUT_PATH_UNKNOWN && c.out_path_len > 0 &&
        c.out_path_len <= MAX_PATH_SIZE) {             // clamp: a corrupt out_path_len (>64)
      sendDirect(pkt, c.out_path, c.out_path_len);   // would overrun sendDirect's payload memcpy -> reboot
    } else {
      uint8_t hash_sz = (uint8_t)(_prefs.path_hash_mode + 1);
      if (hash_sz == 0 || hash_sz > 4) hash_sz = 1;
      sendDirect(pkt, c.id.pub_key, hash_sz);         // unknown path → single hop
    }
    return tag;
  }

  /** SIGNAL PROBE (the standard MeshCore node-discovery): broadcast a zero-hop
   *  NODE_DISCOVER_REQ control packet asking repeaters to answer. Each neighbouring
   *  repeater replies DIRECTLY with a NODE_DISCOVER_RESP — this is the exact packet the
   *  Ultra / KiekR GUIs use, and what repeater firmware actually answers. (A TRACE or a
   *  bare zero-hop advert gets no reply: repeaters don't retransmit an unpathed trace,
   *  so the old trace-probe read nothing — thanks to Tarmo for decoding the real packet.)
   *  onControlDataRecv matches _ui_sig_probe_tag on the reply and captures its SNR/RSSI
   *  into _ui_sig_*. Never floods (zero-hop, not forwarded). Returns the tag. */
  uint32_t uiSendSignalProbe() {
    uint8_t data[10];
    data[0] = CTL_TYPE_NODE_DISCOVER_REQ;            // 0x80; low bit (prefix_only) stays 0
    data[1] = (uint8_t)(1 << ADV_TYPE_REPEATER);     // filter: ask repeaters to answer
    getRNG()->random(&data[2], 4);                   // random tag, to match the responses to
    memcpy(&_ui_sig_probe_tag, &data[2], 4);
    if (_ui_sig_probe_tag == 0) { _ui_sig_probe_tag = 1; memcpy(&data[2], &_ui_sig_probe_tag, 4); }
    uint32_t since = 0;                              // 0 = answer regardless of freshness
    memcpy(&data[6], &since, 4);
    mesh::Packet* pkt = createControlData(data, sizeof(data));
    if (!pkt) { _ui_sig_probe_tag = 0; return 0; }
    sendZeroHop(pkt);                                // repeaters reply directly; never floods
    return _ui_sig_probe_tag;
  }

  /** Request CayenneLPP telemetry from a remote contact. Reply is delivered
   *  via AbstractUITask::onTelemetryReply with the raw LPP payload after the
   *  4-byte timestamp header. Falls back to onPingReply if the UI didn't
   *  override the telemetry hook.
   *  Returns the MSG_SEND_* result code. */
  int sendTelemetryRequestForUI(ContactInfo& recipient) {
    uint32_t tag = 0, est = 0;
    pending_telemetry = 0;
    int r = sendRequest(recipient, REQ_TYPE_GET_TELEMETRY_DATA, tag, est);
    if (r == MSG_SEND_SENT_FLOOD || r == MSG_SEND_SENT_DIRECT) {
      memcpy(&_ui_pending_status, recipient.id.pub_key, 4);
      _ui_pending_kind = UiReqKind::Telemetry;
      _ui_pending_tag  = tag;   // request tag, reflected by the repeater
    }
    return r;
  }

private:

public:
  bool savePrefs() { return _store->savePrefs(_prefs, sensors.node_lat, sensors.node_lon); }

  // Set the default flood scope (region) used to tag outgoing flood packets, so
  // repeaters on region-scoped networks ("flood only for their region") will
  // re-broadcast them. A public "#hashtag" region's scope key is SHA256("#name");
  // a blank/NULL name clears it (unscoped, the default). Persists immediately.
  void setDefaultFloodScope(const char* region_name);

  // Opt-in: when true, direct/login/admin UNICAST floods are also tagged with the default
  // region scope, so a region-scoped repeater that is your ONLY path will re-flood them.
  // Default false keeps them unscoped (cross-region login/DMs work). (Issue #64.)
  void setScopeDirectFloods(bool on) { scope_direct_floods = on; }

  // Per-channel flood-scope override. pushChannelScope() derives a transient scope
  // from "#region" (SHA256) for the NEXT channel send and returns true (the caller
  // must popChannelScope() right after that send to restore); a blank/NULL name is
  // a no-op returning false. Lets a channel override the default flood scope.
  bool pushChannelScope(const char* region_name);
  void popChannelScope();

  // Re-apply the persisted radio settings (freq/bw/sf/cr, TX power, RX-boost) to
  // the live radio — the same calls begin() makes at boot. Lets the UI change
  // region / radio params and have them take effect immediately, no reboot.
  void applyRadioFromPrefs();

  /** Find the first unused channel slot, or -1 if the table is full. A slot
   *  is considered free when its name is empty (matches getNumChannels). */
  int findFirstEmptyChannelSlot() {
#ifdef MAX_GROUP_CHANNELS
    for (int i = 0; i < MAX_GROUP_CHANNELS; ++i) {
      ChannelDetails cd{};
      if (!getChannel(i, cd) || cd.name[0] == '\0') return i;
    }
#endif
    return -1;
  }

  /** UI-side equivalent of the CMD_SET_CHANNEL serial handler in handleCmdFrame.
   *  Writes channel `idx` with `name` (up to 31 chars) and a 16-byte secret,
   *  persists it to NVS and pokes the UI to refresh its thread list.
   *  Returns false if `idx` is out of range or setChannel rejects the slot. */
  bool uiAddOrUpdateChannel(int idx, const char* name, const uint8_t secret16[16]) {
    if (idx < 0) return false;
    ChannelDetails channel{};
    StrHelper::strncpy(channel.name, name, sizeof(channel.name));
    memset(channel.channel.secret, 0, sizeof(channel.channel.secret));
    memcpy(channel.channel.secret, secret16, 16);
    if (!setChannel(idx, channel)) return false;
    saveChannels();
    if (_ui) _ui->onThreadsChanged();
    return true;
  }

  /** Wipe the cached return path for a contact so the next outgoing message
   *  re-floods instead of routing through a stale hop list. Returns false
   *  when the contact isn't in the table any more. */
  bool uiResetContactPath(const uint8_t pub_key[32]) {
    ContactInfo* slot = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (!slot) return false;
    slot->out_path_len = OUT_PATH_UNKNOWN;
    memset(slot->out_path, 0, sizeof(slot->out_path));
    saveContacts();
    return true;
  }

  /** Update a contact's stored GPS position (microdegrees, *1e6) — e.g. from a
   *  telemetry reply that carried a CayenneLPP GPS field. Lets contacts that
   *  don't flood position adverts (but do answer telemetry) appear on the map
   *  (issue #27). Returns false if the contact isn't in the table. */
  bool uiSetContactGps(const uint8_t pub_key[32], int32_t lat_e6, int32_t lon_e6) {
    ContactInfo* slot = lookupContactByPubKey(pub_key, PUB_KEY_SIZE);
    if (!slot) return false;
    slot->gps_lat = lat_e6;
    slot->gps_lon = lon_e6;
    slot->lastmod = getRTCClock()->getCurrentTime();
    saveContacts();
    return true;
  }

  /** Manually add a chat-type peer to contacts[] from a raw 32-byte pubkey
   *  + display name. Used by the Contacts tab "+" → "Add by pubkey" flow
   *  when we want to talk to someone whose advert we haven't received yet.
   *  Returns false if the pubkey is already a contact, the name is empty,
   *  or the contact table is full. */
  bool uiAddManualContact(const uint8_t pub_key[32], const char* name) {
    if (!name || !name[0]) return false;
    if (lookupContactByPubKey(pub_key, PUB_KEY_SIZE) != nullptr) return false;
    ContactInfo ci{};
    memcpy(ci.id.pub_key, pub_key, PUB_KEY_SIZE);
    ci.type         = ADV_TYPE_CHAT;
    ci.out_path_len = OUT_PATH_UNKNOWN;
    ci.last_advert_timestamp = 0;          // unknown — we never heard them
    ci.lastmod      = getRTCClock()->getCurrentTime();
    StrHelper::strncpy(ci.name, name, sizeof(ci.name));
    if (!addContact(ci)) return false;
    saveContacts();
    if (_ui) _ui->onThreadsChanged();
    return true;
  }

  /** Persist the in-RAM contact table to flash (/contacts3). Public wrapper so
   *  UI paths that insert via the base addContact() — e.g. the Discovered-list
   *  "Add to contacts" button — can persist; otherwise that contact is RAM-only
   *  and lost on reboot. */
  bool uiPersistContacts() { saveContacts(); return true; }
  // Flush a pending (possibly coalesced) contacts write before a deliberate
  // shutdown/reboot, so the on-device power paths don't lose the last refresh
  // window on card-less devices (see MyMesh::loop). No-op when nothing is pending.
  void flushContactsIfDirty() { if (dirty_contacts_expiry) { saveContacts(); dirty_contacts_expiry = 0; } }

  /** Remove a contact from a device-UI action and PERSIST it (mirrors the
   *  companion app's CMD_REMOVE_CONTACT). The base removeContact() only drops it
   *  from RAM, so without rewriting /contacts3 the contact reappears on the next
   *  reboot. Also deletes its stored blob and pings the chats list. */
  bool uiRemoveContact(const ContactInfo& c) {
    ContactInfo* slot = lookupContactByPubKey(c.id.pub_key, PUB_KEY_SIZE);
    if (!slot || !removeContact(*slot)) return false;
    _store->deleteBlobByKey(c.id.pub_key, PUB_KEY_SIZE);
    saveContacts();
    if (_ui) _ui->onThreadsChanged();
    return true;
  }

  /** Clear channel slot `idx` (zero name + secret), persist, and ping the UI
   *  so the chats list drops the entry. Returns false if the index is out of
   *  range. Used by the long-press → Delete action on the chats list. */
  bool uiDeleteChannel(int idx) {
#ifdef MAX_GROUP_CHANNELS
    if (idx < 0 || idx >= MAX_GROUP_CHANNELS) return false;
    ChannelDetails empty{};
    if (!setChannel(idx, empty)) return false;
    saveChannels();
    if (_ui) _ui->onThreadsChanged();
    return true;
#else
    (void)idx;
    return false;
#endif
  }

  /** Idempotently re-add the default "Public" channel (PSK matches
   *  MyMesh.cpp's PUBLIC_GROUP_PSK). Returns true if the channel is present
   *  after the call, false if no slot was available. */
  bool uiJoinPublicChannel() {
#ifdef MAX_GROUP_CHANNELS
    for (int i = 0; i < MAX_GROUP_CHANNELS; ++i) {
      ChannelDetails cd{};
      if (getChannel(i, cd) && cd.name[0] != '\0' &&
          strncasecmp(cd.name, "Public", 6) == 0) return true;
    }
#endif
    if (addChannel("Public", "izOH6cXN6mrJ5e26oRXNcg==") == nullptr) return false;
    saveChannels();
    if (_ui) _ui->onThreadsChanged();
    return true;
  }

  // To check if there is pending work
  bool hasPendingWork() const;

  // Number of companion clients currently connected on any transport.
  // Used by the idle light-sleep gate (TouchSleep) to confirm no one is
  // actively talking to us before the node parks in light sleep.
  int getProtoNumClients() const { return proto_num_clients; }

  // True if the radio is currently mid-receive of a packet (preamble→RxDone).
  // _radio is the protected Dispatcher::Radio pointer; isReceiving() returns false
  // by default and is overridden by RadioLibWrapper with a real preamble-detect check.
  bool isRadioReceiving() const { return _radio && _radio->isReceiving(); }

private:
  void writeOKFrame();
  void writeErrFrame(uint8_t err_code);
  void writeDisabledFrame();
  /** Returns bytes written, or 0 on failure. Caller can retry on 0. */
  size_t writeContactRespFrame(uint8_t code, const ContactInfo &contact, bool to_all = false);
  void updateContactFromFrame(ContactInfo &contact, uint32_t& last_mod, const uint8_t *frame, int len);
  void addToOfflineQueue(const uint8_t frame[], int len);
  int getFromOfflineQueue(uint8_t frame[]);
  /** Appends frame to ring; returns assigned seq, or 0 if rejected. */
  uint32_t addToHistoryRing(const uint8_t frame[], int len);
  /** Get next history frame for client. If do_advance is false, does not advance last_delivered_seq (call commitHistoryForClient after successful write). */
  int getNextFromHistoryForClient(const char* client_id, uint8_t frame[], uint32_t* out_seq = nullptr, bool do_advance = true);
  void commitHistoryForClient(const char* client_id, uint32_t seq);
  /** After a successful V3 live broadcast, bump sync watermarks for clients that understand V3 on the wire (target_ver >= 3 or unknown 0xFF).
   *  Legacy apps (CMD_DEVICE_QUERY second byte < 3) are skipped so CMD_SYNC_NEXT_MESSAGE can still deliver adapted 7/8 they never got from live 16/17. */
  void advanceHistoryClientsAfterV3Broadcast(uint32_t seq);
  void setClientTargetVer(const char* client_id, uint8_t target_ver);
  uint8_t getClientTargetVer(const char* client_id) const;
  void setClientAppName(const char* client_id, const char* app_name);
  bool shouldAdvanceClientAfterV3Broadcast(const char* client_id) const;
  int adaptHistoryFrameForClient(const char* client_id, const uint8_t src[], int src_len, uint8_t dest[]) const;
  void sendSyncSinceDelta(uint32_t T);
  int getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]) override { 
    return _store->getBlobByKey(key, key_len, dest_buf);
  }
  bool putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], int len) override {
    return _store->putBlobByKey(key, key_len, src_buf, len);
  }

  void checkCLIRescueCmd();
  void checkSerialInterface();
  bool isValidClientRepeatFreq(uint32_t f) const;

  // helpers, short-cuts
  void saveChannels() { _store->saveChannels(this); }
  void saveContacts();

  DataStore* _store;
  NodePrefs _prefs;
  uint32_t pending_login;
  uint32_t pending_status;
  uint32_t pending_telemetry, pending_discovery;   // pending _TELEMETRY_REQ
  uint32_t pending_req;   // pending _BINARY_REQ
  /** UI-side ping tracker: holds first 4 bytes of recipient pub_key when the
   *  touch UI fired a status request. Independent from `pending_status` so it
   *  doesn't collide with the companion-serial workflow. */
  uint32_t _ui_pending_status;
  /** Which UI-side request is in flight against `_ui_pending_status` —
   *  STATUS replies should route to onPingReply, TELEMETRY replies to
   *  onTelemetryReply (CayenneLPP-aware). */
  UiReqKind _ui_pending_kind = UiReqKind::None;
  /** Reflected-tag match for the UI request. STATUS/TELEMETRY responses
   *  echo our request's 4-byte timestamp at data[0..3], but a chained
   *  guest LOGIN response carries the *repeater's* own clock there — so
   *  matching by pubkey alone misroutes the login OK as the REQ reply.
   *  Compare tag in onContactResponse to keep the two streams separate. */
  uint32_t _ui_pending_tag = 0;
  /** Deferred guest-login-then-request arm: holds the first 4 bytes of the
   *  recipient pub_key (0 = none) plus which REQ to send, set by
   *  uiSendRequestAfterGuestLogin(). onContactResponse fires the REQ when the
   *  matching LOGIN-OK arrives, removing the ACL-commit race on the first try. */
  uint32_t _ui_login_then = 0;
  UiReqKind _ui_login_then_kind = UiReqKind::None;
  BaseSerialInterface *_serial;
  AbstractUITask* _ui;

  ContactsIterator _iter;
  uint32_t _iter_filter_since;
  uint32_t _most_recent_lastmod;
  uint32_t _contact_send_index;  // temporary diagnostic: index of CONTACT sent
  int _contact_list_reply_target;  // reply target for this contact list (so CONTACT/END go to same client as START)
  uint32_t _active_ble_pin;
  bool _iter_started;
  bool _cli_rescue;
  bool send_unscoped;   // force un-scoped flood (instead of using send_scope)
  bool scope_direct_floods = false;  // opt-in (#64): tag direct/login/admin floods with the default region scope
  char cli_command[80];
  uint8_t app_target_ver;
  uint8_t *sign_data;
  uint32_t sign_data_len;
  unsigned long dirty_contacts_expiry;
  // Advert-driven contacts-save coalescer (see MyMesh::loop). On card-less devices a
  // full contacts rewrite can trigger a multi-second SPIFFS GC that freezes the loop;
  // constant advert refreshes would churn it. A change in the contact SET saves
  // promptly; pure refreshes coalesce to CONTACTS_REFRESH_SAVE_INTERVAL. -1 forces
  // the first save. Kept in sync by every MyMesh::saveContacts() call.
  int      _last_saved_contacts_n = -1;
  uint32_t _next_contacts_refresh_save = 0;

  TransportKey send_scope;
  TransportKey _chan_scope_saved;             // push/popChannelScope stash
  bool         _chan_scope_pushed = false;
  bool         _chan_scope_saved_unscoped = false;

  uint8_t cmd_frame[MAX_FRAME_SIZE + 1];
  uint8_t out_frame[MAX_FRAME_SIZE + 1];
  CayenneLPP telemetry;

  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];

    bool isChannelMsg() const;
  };
  int offline_queue_len;
  Frame offline_queue[OFFLINE_QUEUE_SIZE];

  // Per-client history: ring buffer of frames + per-client read position
  struct HistoryEntry {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];
    uint32_t seq;
  };
  struct ClientHistoryState {
    char client_id[MAX_CLIENT_ID_LEN + 1];
    uint32_t last_delivered_seq;
  };
  struct ClientProtoState {
    char client_id[MAX_CLIENT_ID_LEN + 1];
    uint8_t target_ver;
    bool prefer_live_advance;
  };
  HistoryEntry history_ring[HISTORY_RING_SIZE];
  int history_count;
  int history_head;
  uint32_t history_next_seq;
  ClientHistoryState history_clients[MAX_HISTORY_CLIENTS];
  int history_num_clients;
  ClientProtoState proto_clients[MAX_HISTORY_CLIENTS];
  int proto_num_clients;

  struct AckTableEntry {
    unsigned long msg_sent;
    uint32_t ack;
    ContactInfo* contact;
  };
  #define EXPECTED_ACK_TABLE_SIZE 8
  AckTableEntry expected_ack_table[EXPECTED_ACK_TABLE_SIZE]; // circular table
  int next_ack_idx;

  #define ADVERT_PATH_TABLE_SIZE   16
  AdvertPath advert_paths[ADVERT_PATH_TABLE_SIZE]; // circular table

  // One-shot auto-advert on boot. Recipients with auto-add ON pick up our
  // current pubkey, which is critical when the touch firmware regenerates
  // identity after a SPIFFS wipe (otherwise old contacts have stale pubkey
  // and silently drop our DMs because shared-secret no longer matches).
  // 0 = already fired or disabled.
  uint32_t _boot_advert_due_ms = 0;
  bool     _boot_advert_done   = false;

  // Most recently sent UI-initiated trace-ping tag. onTraceRecv compares
  // this against the trace's tag field: if it matches, the trace was ours
  // (a Ping from the action sheet) and we forward the SNRs to the UI; if
  // not, it's an app-originated trace and we route to the companion app
  // path. Single-slot is enough because the UI gates a new ping until the
  // last one resolves or times out.
  uint32_t _ui_trace_ping_tag = 0;
  uint32_t _ui_sig_probe_tag  = 0;   // in-flight silent signal probe (updates _ui_sig_* only, no popup)
};

#if defined(ESP32_PLATFORM)
extern MyMesh& the_mesh;   // PSRAM-resident (placement-new'd in main.cpp); reference keeps call sites unchanged
#else
extern MyMesh the_mesh;
#endif
