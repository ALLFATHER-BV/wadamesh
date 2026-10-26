// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// =============================================================================
//  WebMesh.h -- the MyMesh stand-in for the web build (WEB_BUILD.md).
//
//  src/ui-touch/UITask.cpp includes this instead of src/MyMesh.h when WADA_WEB is
//  defined. It declares a class named MyMesh with EXACTLY the members the touch UI
//  uses, with the same signatures, and nothing that swallows unknown calls. That is
//  the drift alarm: when a firmware change adds `the_mesh.someNewThing()` to the UI,
//  the web build stops compiling here, and someone decides what that call means in
//  the browser (implement it on the backend, or mark it unavailable below).
//
//  Data types (ContactInfo, ChannelDetails, mesh::Identity, NodePrefs, RegionRegistry,
//  DataStore, ADV_TYPE_*, OUT_PATH_UNKNOWN, MSG_SEND_*) are the REAL headers. Only the
//  class is a stand-in. Its state is the same local mirror the device keeps (prefs,
//  contact + channel tables, counters); anything that needs the network goes to the
//  MeshBackend (MeshBackend.h).
//
//  Every member that cannot mean anything in a browser is marked [unavailable] and
//  listed in WEB_BUILD.md.
// =============================================================================
#include <Arduino.h>
#include <Mesh.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/BaseChatMesh.h>   // MSG_SEND_*, TxtTxDebugInfo, ContactInfo, ChannelDetails (declarations only)
#include <helpers/TxtDataHelpers.h>
#include "AbstractUITask.h"
#include "NodePrefs.h"
#include "DataStore.h"
#include "RegionRegistry.h"
#include <target.h>
#include "MeshBackend.h"

// ---- the preamble of src/MyMesh.h the UI reads --------------------------------
// (FIRMWARE_VERSION / FIRMWARE_BUILD_DATE / FIRMWARE_CORE_VERSION / FIRMWARE_VER_CODE
//  are passed by web/CMakeLists.txt, read from src/MyMesh.h: one source of truth.)
#define CTL_TYPE_NODE_DISCOVER_REQ    0x80
#define CTL_TYPE_NODE_DISCOVER_RESP   0x90
#ifndef COMPANION_WIRE_DEBUG
#define COMPANION_WIRE_DEBUG 0
#endif
#if COMPANION_WIRE_DEBUG
#define WIRE_DBG(...) Serial.printf(__VA_ARGS__)
#else
#define WIRE_DBG(...) do {} while (0)
#endif
#define MESHCOMOD_NAME "Meshcomod"
// Radio defaults: the web board has no radio; these are what Radio settings show
// until a backend reports the node's real values (EU narrow, like the CrowPanel env).
#ifndef LORA_FREQ
#define LORA_FREQ 869.618
#endif
#ifndef LORA_BW
#define LORA_BW 62.5
#endif
#ifndef LORA_SF
#define LORA_SF 8
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
#define LORA_TX_POWER 22
#endif
#ifndef MAX_LORA_TX_POWER
#define MAX_LORA_TX_POWER LORA_TX_POWER
#endif
#ifndef BLE_NAME_PREFIX
#define BLE_NAME_PREFIX "MeshCore-"
#endif
#define REQ_TYPE_GET_TELEMETRY_DATA     0x03

struct AdvertPath {
  uint8_t pubkey_prefix[7];
  uint8_t path_len;
  char    name[32];
  uint32_t recv_timestamp;
  uint32_t recv_seq;
  uint8_t type;
  bool used;
  uint8_t path[MAX_PATH_SIZE];
};

struct RecentlyHeardName {
  uint8_t pubkey_prefix[7];
  char name[32];
  uint32_t recv_seq;
  uint8_t type;
};

class MyMesh : private MeshBackendEvents {
public:
  MyMesh(mesh::RTCClock& rtc, AbstractUITask* ui);
  /** Boot: load prefs, start the backend, replay its contacts/channels. */
  void begin(MeshBackend* backend);
  void loop();
  // (board-facing, not the UI's) commands still on their way to a node: a restart
  // waits for them (web_main.cpp), as a device finishes its flash writes first.
  bool backendBusy() const { return _backend && _backend->busy(); }

  // ---- identity + prefs -------------------------------------------------------
  mesh::LocalIdentity self_id;
  const uint8_t* getSelfPubKey() const { return self_id.pub_key; }
  NodePrefs* getNodePrefs() { return &_prefs; }
  bool savePrefs();
  bool setBLEPin(uint32_t pin);                         // the node's PIN (companion); demo: false
  uint32_t getOrphanedBlobs() const { return 0; }
  DataStore* getStore() { return nullptr; }             // no local DataStore: the About store lines hide
  mesh::RTCClock* getRTCClock() const { return _rtc; }
  void applyRadioFromPrefs();                           // forwards the radio prefs to the backend

  // ---- contacts (the BaseChatMesh table, mirrored) ------------------------------
  int getNumContacts() const { return _num_contacts; }
  bool getContactByIdx(uint32_t idx, ContactInfo& contact);
  ContactInfo* lookupContactByPubKey(const uint8_t* pub_key, int prefix_len);
  bool addContact(const ContactInfo& contact);
  void resetPathTo(ContactInfo& recipient);
  int uiContactIdxByPubKey(const uint8_t pub_key[32]);
  bool uiResetContactPath(const uint8_t pub_key[32]);
  bool uiShareContact(const uint8_t pub_key[32]);       // companion: CMD_SHARE_CONTACT; demo: false
  bool uiSetContactGps(const uint8_t pub_key[32], int32_t lat_e6, int32_t lon_e6);
  bool uiAddManualContact(const uint8_t pub_key[32], const char* name);
  bool uiAddDiscoveredContact(const uint8_t pub_key[32], uint8_t type, const char* name);
  bool uiPersistContacts() { return true; }             // the mirror lives in the backend
  bool uiGetContactTelemetryPerm(const uint8_t pub_key[32], uint8_t telem_perm);
  bool uiSetContactTelemetryPerm(const uint8_t pub_key[32], uint8_t telem_perm, bool on);
  bool uiSetContactFavorite(const uint8_t pub_key[32], bool fav);
  void flushContactsIfDirty() {}
  bool uiRemoveContact(const ContactInfo& c);
  bool uiRemoveContactNoSave(const uint8_t pub_key[PUB_KEY_SIZE]);
  bool removeTransientContact(const uint8_t pub_key[PUB_KEY_SIZE]);
  bool uiIsMeshcomodRecipient(const uint8_t* pub_key_prefix_6) const { (void)pub_key_prefix_6; return false; }
  bool uiHasConnectionTo(const uint8_t pub_key[32]) { return _backend && _backend->hasConnectionTo(pub_key); }
  bool uiHopName(const uint8_t* hash, int prefix_len, char* out, size_t out_sz);
  bool uiHopPos(const uint8_t* hash, int prefix_len, double* out_lat, double* out_lon);
  int  getRecentlyHeard(RecentlyHeardName dest[], int max_num);
  bool getAdvertPath(const uint8_t* pubkey_prefix, int prefix_len, uint8_t& path_len,
                     uint8_t* path, size_t path_cap);
  void uiExportBackup(Print& out, double node_lat, double node_lon);   // the device's file, without the private key
  bool uiImportBackup(Stream& in, uint8_t sections, bool replace_channels, bool replace_contacts,
                      int* out_channels, int* out_contacts,         // settings, channels, contacts (no key)
                      bool* out_saved = nullptr);                   // false: prefs not written (#708)
  bool uiFactoryReset();                                // wipes the host data dir's SPIFFS

  // ---- channels -----------------------------------------------------------------
  bool getChannel(int idx, ChannelDetails& dest);
  int findFirstEmptyChannelSlot();
  bool uiAddOrUpdateChannel(int idx, const char* name, const uint8_t secret16[16]);
  bool uiDeleteChannel(int idx);
  bool uiJoinPublicChannel();
  void setDefaultFloodScope(const char* region_name);
  void setScopeDirectFloods(bool on) { _scope_direct_floods = on; }   // [unavailable] a node setting the protocol lacks
  bool pushChannelScope(const char* region_name);
  void popChannelScope() {}
  RegionRegistry& regionRegistry() { return _region_reg; }

  // ---- sending ------------------------------------------------------------------
  int sendMessage(const ContactInfo& recipient, uint32_t timestamp, uint8_t attempt,
                  const char* text, uint32_t& expected_ack, uint32_t& est_timeout,
                  uint32_t* out_packet_hash4 = nullptr, TxtTxDebugInfo* out_dbg = nullptr);
  bool sendGroupMessage(uint32_t timestamp, mesh::GroupChannel& channel, const char* sender_name,
                        const char* text, int text_len);
  bool getLastTxtTxHash4(uint32_t& out_hash4) const { out_hash4 = _last_txt_hash4; return _last_txt_hash4 != 0; }
  bool advert() { return sendAdvert(false); }
  bool sendAdvert(bool flood);
  void uiRegisterExpectedAck(uint32_t expected_ack, const uint8_t pub_key[32]);
  void setCompanionRetryEnabled(bool on) { _companion_retry_enabled = on; }
  bool getNextCompanionRetryWakeDelay(uint32_t& delay_millis) const { (void)delay_millis; return false; }
  bool hasPendingWork() const { return false; }
  void persistSyncHistoryNow() {}                       // no companion sync ring on the web board

  // ---- repeats heard + RX metadata -------------------------------------------------
  uint32_t uiLastSentFp() const { return _last_sent_fp; }
  uint8_t uiRepeatsForFp(uint32_t fp) const;
  uint8_t uiRepeatHopCount(uint32_t fp) const;
  uint8_t uiRepeatHop(uint32_t fp, uint8_t idx, uint8_t* out, uint8_t out_sz) const;
  bool takeEchoDirty() { bool d = _echo_dirty; _echo_dirty = false; return d; }
  uint8_t lastRxPath(uint8_t* buf, uint8_t max) const {
    uint8_t n = _last_rx_path_n < max ? _last_rx_path_n : max;
    if (buf && n) memcpy(buf, _last_rx_path, n);
    return n;
  }
  uint16_t lastRxScope(bool* has) const { if (has) *has = _last_rx_has_scope; return _last_rx_scope; }
  uint8_t lastRxScopeSlot() const { return _last_rx_scope_slot; }
  bool lastRxScopeIsHome() const { return false; }   // [unavailable] the node's region key is not reported
  uint32_t uiConsumeLastSenderTs() { uint32_t t = _last_sender_ts; _last_sender_ts = 0; return t; }

  // ---- live signal (status bar) ------------------------------------------------------
  int8_t   uiSignalSnrQ4() const { return _ui_sig_snr_q4; }
  int8_t   uiSignalRssi()  const { return _ui_sig_rssi; }
  uint32_t uiSignalMs()    const { return _ui_sig_ms; }

  // ---- UI requests (status / telemetry / login / trace) --------------------------------
  enum class UiReqKind : uint8_t { None = 0, Status = 1, Telemetry = 2, Regions = 3, Binary = 4 };
  int sendStatusPingForUI(ContactInfo& recipient);       // straight request (the admin app; no guest login)
  int sendTelemetryRequestForUI(ContactInfo& recipient);
  int uiSendBinaryRequest(ContactInfo& recipient, const uint8_t* payload, uint8_t len);   // -> onBinaryReply
  struct UiLoginInfo { uint8_t pub4[4]; uint8_t acl; uint8_t fw_level; bool valid; };
  const UiLoginInfo& uiLastLoginInfo() const { return _ui_last_login; }
  int sendTelemetryRequestWithGuestLoginForUI(ContactInfo& recipient);
  int uiSendRequestAfterGuestLogin(ContactInfo& recipient, UiReqKind kind);
  int uiSendAdminLogin(ContactInfo& recipient, const char* password);       // backend login (demo: its repeater / room)
  int uiSendAdminCommand(ContactInfo& recipient, const char* text);         // backend CLI line
  int uiRoomRelogin(const uint8_t pub_key[32]);
  int sendRegionsRequestForUI(const uint8_t pub_key[PUB_KEY_SIZE], uint32_t& est_timeout);
  void cancelUIPingPending() { _ui_pending_kind = UiReqKind::None; memset(_ui_pending_key, 0, sizeof _ui_pending_key); }
  void cancelUIDeferredLogin() { if (_backend) _backend->cancelRequests(); }
  bool hasUIRequestPending() const { return _ui_pending_kind != UiReqKind::None; }
  uint32_t uiSendTracePing(const uint8_t pub_key[32]);
  uint32_t uiSendTraceRoute(const ContactInfo& c);
  uint32_t uiSendSignalProbe() { return 0; }             // [unavailable] no own radio to probe with

  // ---- Discover app (active node discovery) --------------------------------------------
  struct DiscoverHit {
    uint8_t  pubkey[32];
    uint8_t  node_type;
    int8_t   our_snr_q4;
    int8_t   our_rssi;
    int8_t   their_snr_q4;
    uint8_t  path_len;
    uint32_t first_ms;
    uint32_t last_ms;
    uint16_t heard;
  };
  static const int DISCOVER_MAX = 64;
  uint32_t uiStartDiscoverScan(uint8_t type_filter = 0) { (void)type_filter; return 0; }   // [unavailable]
  uint8_t  discoverCount() const { return 0; }
  bool     discoverGet(uint8_t i, DiscoverHit& out) const { (void)i; (void)out; return false; }
  void     discoverClear() {}

  // ---- RF monitor ring ------------------------------------------------------------------
  struct UiRxRec {
    uint32_t ms;
    int8_t   rssi;
    int8_t   snr_q4;
    uint8_t  ptype;
    uint8_t  route;
    uint8_t  hops;
    uint8_t  len;
    uint8_t  org_kind;
    uint8_t  org[4];
  };
  static const int UI_RXLOG_MAX = 16;
  uint8_t uiRxLogCount() const { return _rxlog_cnt; }   // from the node's raw RX pushes (demo: empty)
  bool    uiRxLogGet(uint8_t i, UiRxRec& out) const {
    if (i >= _rxlog_cnt) return false;
    out = _rxlog[(uint8_t)((_rxlog_head + UI_RXLOG_MAX - 1 - i) % UI_RXLOG_MAX)];
    return true;
  }

  // ---- radio counters (Dispatcher) ---------------------------------------------------------
  uint32_t getNumSentFlood() const { return _n_sent_flood; }
  uint32_t getNumSentDirect() const { return _n_sent_direct; }
  uint32_t getNumRecvFlood() const { return _n_recv_flood; }
  uint32_t getNumRecvDirect() const { return _n_recv_direct; }
  unsigned long getTotalAirTime() const { return 0; }
  unsigned long getReceiveAirTime() const { return 0; }
  unsigned long getRemainingTxBudget() const { return 0; }
  bool isRadioReceiving() const { return false; }
  int getProtoNumClients() const { return 0; }

  // ---- on-device terminal -----------------------------------------------------------------
  static void setTerminalSink(void (*cb)(const char* line));
  void runLocalCli(const char* cmd);                    // the node's CLI (companion, MeshCore 1.18+); else a small subset

private:
  // MeshBackendEvents
  void beContact(const ContactInfo& c, bool heard_advert, uint8_t path_len) override;
  void beDiscovered(const ContactInfo& c, uint8_t path_len) override;
  void beOutgoingDirect(const uint8_t pub_key[32], const char* text, uint32_t expected_ack) override;
  void beOutgoingChannel(int channel_idx, const char* text) override;
  void beChannel(int idx, const ChannelDetails& ch) override;
  void beDirectMessage(const uint8_t pub_key[32], uint32_t sender_ts, const char* text,
                       uint8_t path_len, bool flood, int8_t snr_q4, int8_t rssi) override;
  void beChannelMessage(int channel_idx, uint32_t sender_ts, const char* text,
                        uint8_t path_len, int8_t snr_q4, int8_t rssi) override;
  void beRoomMessage(const uint8_t room_pub_key[32], const char* author, uint32_t sender_ts,
                     const char* text, uint8_t path_len, int8_t snr_q4, int8_t rssi) override;
  void beAck(uint32_t expected_ack) override;
  void beRepeatHeard(uint32_t sent_fp, const uint8_t* hop, uint8_t hop_sz) override;
  void beStatusReply(const uint8_t pub_key[32], const uint8_t* data, size_t len) override;
  void beTelemetryReply(const uint8_t pub_key[32], const uint8_t* lpp, size_t len) override;
  void beTraceResult(uint32_t tag, int8_t their_snr, int8_t our_snr,
                     uint8_t extra_hops, const int8_t* extra_snrs) override;
  void beSignal(int8_t snr_q4, int8_t rssi) override;
  void beSelfInfo(const NodePrefs& node_prefs, const uint8_t pub_key[32], int32_t lat_e6, int32_t lon_e6) override;
  bool _have_own_pos = false;   // a position saved in this browser (k_pos_path) outranks the node's
  void beSelfPosition(int32_t lat_e6, int32_t lon_e6) override;
  void beContactRemoved(const uint8_t pub_key[32]) override;
  void beRxRaw(int8_t snr_q4, int8_t rssi, const uint8_t* raw, size_t len) override;
  void beLoginResult(const uint8_t pub_key[32], bool ok, uint8_t perms, uint32_t server_clock) override;
  void beLoginInfo(const uint8_t pub_key[32], uint8_t acl, uint8_t fw_level) override;
  void beBinaryReply(const uint8_t pub_key[32], const uint8_t* data, size_t len) override;
  int  uiStraightRequest(ContactInfo& recipient, UiReqKind kind);
  void beCliReply(const uint8_t pub_key[32], const char* text) override;
  void beRegionsReply(const uint8_t pub_key[32], const uint8_t* data, size_t len) override;
  void beLinkState(bool up, const char* detail) override;
  void beTerminalLine(const char* text) override;
  void trackSentFp(uint32_t fp);
  void stashRxMeta(bool for_channel, const uint8_t* key_or_hash, uint8_t path_len);

  void loadPrefs();
  void syncAdvertPosition(uint32_t now_ms);   // this computer's position -> the node, when shared
  int  channelIdxOf(const mesh::GroupChannel& ch) const;
  void noteHeard(const ContactInfo& c);

  mesh::RTCClock* _rtc;
  AbstractUITask* _ui;
  MeshBackend* _backend = nullptr;
  NodePrefs _prefs{};
  // The position the node holds as far as we know (its SELF_INFO, or what we last told
  // it), undisplaced; a local position that differs from it is what syncAdvertPosition sends.
  double _node_pos_lat = 0, _node_pos_lon = 0;
  uint32_t _pos_sync_ms = 0, _pos_pushed_ms = 0;
  ContactInfo _contacts[MAX_CONTACTS];
  int _num_contacts = 0;
  ChannelDetails _channels[MAX_GROUP_CHANNELS];
  RegionRegistry _region_reg;
  bool _scope_direct_floods = false;
  bool _companion_retry_enabled = false;

  int _msgcount = 0;
  uint32_t _last_sender_ts = 0;
  uint32_t _last_txt_hash4 = 0;
  uint32_t _last_sent_fp = 0;
  uint8_t  _last_rx_path[32] = {0};
  uint8_t  _last_rx_path_n = 0;
  static const int UI_ECHO_SLOTS = 12;
  uint32_t _echo_fp[UI_ECHO_SLOTS] = {0};
  uint8_t  _echo_rep[UI_ECHO_SLOTS] = {0};
  uint8_t  _echo_idx = 0;
  static const int ECHO_MAX_HOPS = 8;
  uint8_t  _echo_hop[UI_ECHO_SLOTS][ECHO_MAX_HOPS][4] = {};
  uint8_t  _echo_hop_n[UI_ECHO_SLOTS] = {0};
  uint8_t  _echo_hop_sz[UI_ECHO_SLOTS] = {0};
  volatile bool _echo_dirty = false;

  // Raw RX (beRxRaw): the RF monitor ring, the last message packet (for the Info
  // popup's route + scope), advert routes.
  UiRxRec  _rxlog[UI_RXLOG_MAX] = {};
  uint8_t  _rxlog_head = 0, _rxlog_cnt = 0;
  uint8_t  _last_msg_raw[256] = {0};
  uint16_t _last_msg_raw_len = 0;
  uint32_t _last_msg_raw_ms = 0;
  bool     _last_rx_has_scope = false;
  uint16_t _last_rx_scope = 0;
  uint8_t  _last_rx_scope_slot = REGION_SLOT_NONE;
  static const int ADVERT_PATHS = 32;
  AdvertPath _advert_paths[ADVERT_PATHS] = {};
  uint32_t _advert_seq = 0;
  int8_t   _last_rx_snr_q4 = 0;
  int8_t   _last_rx_rssi = 0;

  int8_t   _ui_sig_snr_q4 = -128;
  int8_t   _ui_sig_rssi = 0;
  uint32_t _ui_sig_ms = 0;

  UiReqKind _ui_pending_kind = UiReqKind::None;
  UiLoginInfo _ui_last_login = {};
  uint8_t   _ui_pending_key[PUB_KEY_SIZE] = {0};
  uint32_t  _ui_pending_at = 0;
  uint32_t  _ui_trace_tag = 0;

  uint32_t _n_sent_flood = 0, _n_sent_direct = 0, _n_recv_flood = 0, _n_recv_direct = 0;

  static const int HEARD_MAX = 32;
  RecentlyHeardName _heard[HEARD_MAX];
  int _heard_n = 0;
  uint32_t _heard_seq = 0;
};

extern MyMesh& the_mesh;
