#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/SensorManager.h>
#include <helpers/BaseSerialInterface.h>
#include <Arduino.h>
#include <helpers/sensors/LPPDataHelpers.h>

#ifndef LED_STATE_ON
  #define LED_STATE_ON 1
#endif

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif
#ifdef PIN_VIBRATION
  #include <helpers/ui/GenericVibration.h>
#endif

#include "../AbstractUITask.h"
#include "../NodePrefs.h"

struct ContactInfo;

/** Maps bottom tabs (Home, Chats, Contacts, Set — no separate Net tab). */
enum class TouchUiScreen : uint8_t { Home = 0, ChatInbox = 1, Contacts = 2, Settings = 3 };

class UITask : public AbstractUITask {
public:
  static const int MAX_UI_MESSAGES = 96;
  static const int MAX_UI_THREADS = 48;
  static const int MAX_THREAD_NAME = 32;
  static const int MAX_SENDER_NAME = 24;
  static const int MAX_MSG_TEXT = 160;   // full LoRa text length (was 96 -> cut long msgs ~3 lines)
  static const int MAX_UI_PATH = 32;  // inbound-route bytes/message for the Info popup (covers deep + multi-byte-hash routes)

  // Outgoing-DM delivery state. None for incoming + channel messages.
  enum : uint8_t {
    DELIV_NONE      = 0,  // unknown (incoming msgs, channel posts, or pre-ACK history loaded from disk)
    DELIV_SENT      = 1,  // sendMessage returned SENT_*, ack not yet received
    DELIV_DELIVERED = 2,  // ack matched in MyMesh::processAck → onMessageAcked
    DELIV_FAILED    = 3,  // sendMessage returned FAILED
  };

  // Per-message RX metadata. Populated from the LoRa packet at receive time
  // (in MyMesh::queueMessage / onChannelMessageRecv) and surfaced via the
  // bubble long-press "Info" sheet. Outgoing messages + history loaded from
  // pre-v4 disk don't have these — meta_flags bit 0 distinguishes.
  enum : uint8_t {
    MSG_META_HAS_RX    = (1u << 0),  // snr_q4/rssi/path_len populated
    MSG_META_IS_FLOOD  = (1u << 1),  // packet was flooded (path_len = hop count); else 0xFF / direct
    MSG_META_HAS_SCOPE = (1u << 2),  // in_scope holds a valid transport scope (transport_codes[0])
  };

  struct UIMessage {
    uint32_t ts;
    bool channel;
    bool outgoing;
    uint32_t ack_hash;       // expected-ack for outgoing DMs (0 if none / channel / incoming)
    uint8_t  deliv_state;    // see DELIV_* above; defaults to DELIV_NONE
    uint8_t  meta_flags;     // see MSG_META_* (0 = no RX metadata)
    uint8_t  path_len;       // hop count for flood packets; 0xFF for direct/routed; 0 if unknown
    int8_t   snr_q4;         // SNR × 4 (matches on-wire encoding); 0 if unknown
    int8_t   rssi;           // dBm; 0 if unknown
    // RAM-only (not persisted): route + repeats metadata, valid for the current
    // session. sent_fp links an outgoing flood to MyMesh's "repeats heard" ring;
    // in_path[] holds the repeater hashes an inbound flood traversed.
    uint32_t sent_fp;
    uint16_t in_scope;       // transport scope (transport_codes[0]); valid iff MSG_META_HAS_SCOPE
    uint8_t  in_path_n;
    uint8_t  in_path[MAX_UI_PATH];
    char thread[MAX_THREAD_NAME + 1];
    char sender[MAX_SENDER_NAME + 1];
    char text[MAX_MSG_TEXT + 1];
  };

  struct UIThread {
    bool used;
    bool channel;
    bool has_mention;   // an unread message in this thread @mentions me (RAM-only hint)
    uint16_t unread;
    uint32_t last_ts;
    /** Mesh contact index for DM sends, or -1 if unknown / not mapped. */
    int16_t mesh_contact_idx;
    /** Full contact pubkey for stable DM mapping across refresh/reorders. */
    uint8_t mesh_contact_pub[32];
    /** First 6 bytes of contact pubkey for stable DM mapping across reorders. */
    uint8_t mesh_contact_key6[6];
    /** Channel slot index for group sends, or -1 if unknown. */
    int16_t mesh_channel_slot;
    char name[MAX_THREAD_NAME + 1];
  };

private:
  DisplayDriver* _display;
  SensorManager* _sensors;
#ifdef PIN_BUZZER
  genericBuzzer buzzer;
#endif
#ifdef PIN_VIBRATION
  GenericVibration vibration;
#endif
  unsigned long _next_refresh, _auto_off;
  /* Screen timeout: track last input activity and turn off the TFT when
   * idle for `_screen_timeout_ms` (0 = never sleep). Cached from NVS prefs;
   * any touch / user-button press resets the deadline and rewakes the panel. */
  unsigned long _last_input_ms;
  uint32_t _screen_timeout_ms;
  bool _screen_off;
  /* True when the user explicitly locked via the BOOT button — touch can't
   * unlock from this state, only another BOOT button press. False = the
   * screen turned off from idle timeout, in which case touch wakes it. */
  bool _manual_lock;
  NodePrefs* _node_prefs;
  // GPS auto-location: once a fix is seen, keep the node location (node_lat/lon,
  // used by the profile + adverts) synced to GPS and persist it occasionally
  // (rate-limited) so it survives a reboot. Updated each loop via updateGpsLocation().
  bool _gps_had_fix = false;
  double _gps_saved_lat = 0.0, _gps_saved_lon = 0.0;
  unsigned long _gps_next_persist_ms = 0;
  void updateGpsLocation(unsigned long now);
  char _alert[80];
  unsigned long _alert_expiry;
  int _msgcount;
  int _ui_msg_count;
  int _ui_msg_head;
  unsigned long _next_thread_seed;
  UIMessage* _ui_msgs   = nullptr;   // ring of recent messages — PSRAM-allocated in begin()
  UIThread*  _ui_threads = nullptr;  // thread table — PSRAM-allocated in begin()
  unsigned long ui_started_at, next_batt_chck;
  int next_backlight_btn_check = 0;
#ifdef PIN_STATUS_LED
  int led_state = 0;
  int next_led_change = 0;
  int last_led_increment = 0;
#endif

#ifdef PIN_USER_BTN_ANA
  unsigned long _analogue_pin_read_millis = millis();
#endif

  UIScreen* splash;
  UIScreen* home;
  UIScreen* chats;
  UIScreen* channels;
  UIScreen* thread_view;
  UIScreen* network;
  UIScreen* settings;
  UIScreen* curr;
  int _active_thread_idx;
  bool _active_thread_is_channel;
  int _thread_scroll;
  bool _composer_mode;
  int _composer_char_idx;
  int _composer_action_idx;
  char _compose_buf[128];
  unsigned long _next_mesh_thread_refresh;
  TouchUiScreen _touch_screen;
  bool _active_dm_contact_set;
  uint8_t _active_dm_contact_pub[32];
  bool _history_dirty;
  unsigned long _next_history_flush_ms;

  void userLedHandler();

  // Button action handlers
  char checkDisplayOn(char c);
  char handleLongPress(char c);
  char handleDoubleClick(char c);
  char handleTripleClick(char c);
  int findOrCreateThread(const char* name, bool channel);
  bool looksLikeKnownChannel(const char* name) const;
public:
  void refreshThreadsFromMesh();
  /** Drop UI thread `idx` and any cached messages tied to it. Returns false
   *  if the index is out of range or the slot wasn't in use. Used by the
   *  long-press → Delete chat action; for channel threads, the caller is
   *  expected to also free the the_mesh channel slot. */
  bool removeThread(int idx);
public:
  // Resolve the contact backing the currently-open conversation thread (used by
  // e.g. the message Info popup's "Trace route"). Returns false for channels or
  // when no DM thread is active.
  bool lookupActiveContact(ContactInfo& out) const;
private:
  void syncThreadMeshSlots(const char* thread_name, bool channel);
  int findThreadByName(const char* name, bool channel) const;
  void sortThreadsByRecent(bool channel_mode, int out_indexes[], int& out_count) const;
  int appendMessage(const char* thread, const char* sender, const char* text, bool channel, bool outgoing, bool mark_unread, uint32_t ack_hash = 0, uint8_t deliv_state = DELIV_NONE,
                    uint8_t meta_flags = 0, uint8_t path_len = 0, int8_t snr_q4 = 0, int8_t rssi = 0,
                    const uint8_t* in_path = nullptr, uint8_t in_path_n = 0, uint32_t sent_fp = 0,
                    uint16_t in_scope = 0);
  // Shared core for newMsg / newMsgFromPubWithMeta — see UITask.cpp.
  void newMsgImpl(uint8_t path_len, const char* from_name, const char* text, int msgcount,
                  uint8_t meta_flags, int8_t snr_q4, int8_t rssi);
public:
  /** Match an arriving ACK (4-byte hash) against the last few outgoing DMs
   *  and flip their delivery state to DELIV_DELIVERED so the chat detail
   *  shows the double-check. No-op if no match. */
  void onMessageAcked(uint32_t ack_hash);
  /** Trace-ping reply landed: open a modal with the bidirectional SNR
   *  numbers. Called by MyMesh::onTraceRecv when the trace's tag matches
   *  the one we issued from the contact action sheet's Trace Ping button. */
  void onTracePingResult(uint32_t tag, int8_t their_snr, int8_t our_snr,
                         uint8_t extra_hops, const int8_t* extra_snrs) override;
private:
  int getThreadMessageIndexes(int thread_idx, int out_indexes[], int max_out, bool newest_first) const;
  void setActiveThread(int idx, bool channel_mode);
  void resetComposer();
  void appendComposerChar(char c);
  void appendComposerText(const char* text);
  void backspaceComposerChar();
  bool sendComposerToActiveThread();
  void markHistoryDirty(unsigned long delay_ms = 600);
  void flushHistoryIfDue(unsigned long now);
  bool loadHistoryFromStorage();
  bool saveHistoryToStorage();

  void setCurrScreen(UIScreen* c);

public:

  UITask(mesh::MainBoard* board, BaseSerialInterface* serial)
      : AbstractUITask(board, serial), _display(NULL), _sensors(NULL), _touch_screen(TouchUiScreen::Home) {
    next_batt_chck = _next_refresh = 0;
    ui_started_at = 0;
    curr = NULL;
    _next_mesh_thread_refresh = 0;
    _active_dm_contact_set = false;
    memset(_active_dm_contact_pub, 0, sizeof(_active_dm_contact_pub));
    _history_dirty = false;
    _next_history_flush_ms = 0;
    _last_input_ms = 0;
    _screen_timeout_ms = 20000;  // overridden by NVS in begin()
    _screen_off = false;
    _manual_lock = false;
  }
  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  /** Diagnostic: update the top-layer touch badge (and force a repaint) so the
   *  user can see boot progress even if Arduino loop() hasn't started yet. */
  void setBootPhase(const char* label);

  void gotoHomeScreen() { _composer_mode = false; setCurrScreen(home); }
  void gotoChatsScreen() { setCurrScreen(chats); }
  void gotoChannelsScreen() { setCurrScreen(channels); }
  void gotoThreadScreen() { setCurrScreen(thread_view); }
  /** Legacy name: network UI lives under Settings tab on touch. */
  void gotoNetworkScreen() { setCurrScreen(settings); }
  void gotoSettingsScreen() { setCurrScreen(settings); }
  void showAlert(const char* text, int duration_millis);
  int  getMsgCount() const { return _msgcount; }
  int  getUnreadTotal() const;
  int  getUnreadMentionCount() const;   // # of threads with an unread @mention of me
  void markThreadRead(int idx);   // clear one thread's unread count (persisted)
  void markActiveThreadRead();    // clear the currently-open thread's unread (viewing == read)
  void markAllThreadsRead();      // clear every thread's unread count
  bool threadHasMention(int idx) const;   // unread @mention of me in this thread
  int  getThreadCount(bool channel_mode, int out_indexes[], int max_out) const;
  /** Inbox list: channels (any used) + DMs that have at least one stored message, sorted by recency. */
  int  getCombinedInboxCount(int out_indexes[], int max_out) const;
  bool threadHasMessageHistory(int thread_idx) const;
  bool getThreadInfo(int idx, bool& channel, uint16_t& unread, uint32_t& ts, char* name, size_t name_len) const;
  int  getActiveThreadMessageCount(int out_indexes[], int max_out, bool newest_first) const;
  bool getMessageByIndex(int msg_idx, UIMessage& out) const;
  const char* getComposerBuffer() const { return _compose_buf; }
  bool isComposerMode() const { return _composer_mode; }
  int  getComposerCharIndex() const { return _composer_char_idx; }
  int  getComposerActionIndex() const { return _composer_action_idx; }
  void stepComposerChar(int delta);
  void stepComposerAction(int delta);
  void setComposerMode(bool enabled) { _composer_mode = enabled; }
  void composerReset() { resetComposer(); }
  void composerAppendChar(char c) { appendComposerChar(c); }
  void composerAppendText(const char* text) { appendComposerText(text); }
  void composerBackspace() { backspaceComposerChar(); }
  bool composerSend() { return sendComposerToActiveThread(); }
  void composerTypingMode() { _composer_action_idx = -1; }
  /** Open DM thread for mesh contact index (e.g. Chats thread list / external hooks). */
  void openMeshContactDm(uint32_t mesh_contact_index);
  /** Reset outbound path for active DM thread (maps to CMD_RESET_PATH companion use-case). */
  void resetActiveDmPath();
  /** LVGL: persist tab index and run light per-screen hooks. */
  void onLvTabChanged(int tab_index);
  void appendDiag(const char* message) override;
  TouchUiScreen touchScreen() const { return _touch_screen; }
  void enterThread(bool channel_mode, int idx);
  /** Block the sender of the active thread's tapped message: resolve to a pubkey
   *  (DM = the thread's contact; channel = sender-name → contact lookup) and add
   *  it to the persisted ignore list. False if no pubkey could be resolved. */
  bool ignoreSenderInActiveThread(const char* sender_name);
  bool hasActiveThread() const { return _active_thread_idx >= 0; }
  bool activeThreadIsChannel() const { return _active_thread_is_channel; }
  // Active channel's mesh slot (-1 when the open thread isn't a channel). For the
  // status-bar channel-settings gear (per-channel region scope).
  int16_t activeChannelSlot() const {
    if (!_active_thread_is_channel || _active_thread_idx < 0 || _active_thread_idx >= MAX_UI_THREADS) return -1;
    return _ui_threads[_active_thread_idx].mesh_channel_slot;
  }
  int  activeThreadIdx() const { return _active_thread_idx; }
  int  threadScroll() const { return _thread_scroll; }
  void setThreadScroll(int v) { _thread_scroll = v; }
  bool hasDisplay() const { return _display != NULL; }
  int displayWidth() const { return _display ? _display->width() : 240; }
  int displayHeight() const { return _display ? _display->height() : 320; }
  bool isButtonPressed() const;

  bool isBuzzerQuiet() {
#ifdef PIN_BUZZER
    return buzzer.isQuiet();
#else
    // No piezo buzzer pin (touch boards). The touch UI plays I2S tones on the
    // T-Deck and gates them on the persisted buzzer_quiet pref, so reflect that
    // here rather than always reporting "quiet".
    return _node_prefs ? (_node_prefs->buzzer_quiet != 0) : true;
#endif
  }

  void toggleBuzzer();
  bool getGPSState();
  void toggleGPS();
  /** True if the GPS currently reports a valid fix. */
  bool getGpsFix();
  /** Satellites currently in view, or -1 if unknown / no GPS hardware. */
  int  getGpsSats();
  /** True once a valid fix has been seen this session. */
  bool getGpsHadFix() const { return _gps_had_fix; }
  double getNodeLat() const { return _sensors ? _sensors->node_lat : 0.0; }
  double getNodeLon() const { return _sensors ? _sensors->node_lon : 0.0; }
  const char* getNodeNameCstr() const { return (_node_prefs && _node_prefs->node_name[0]) ? _node_prefs->node_name : ""; }

  /** Settings: persist node display name (max 31 chars + NUL). */
  bool setNodeName(const char* s);
  /** Settings: persist advert lat/lon (stored with prefs). */
  bool setPosition(double lat, double lon);
  /** Settings: radio params; clamps to firmware limits, savePrefs, applies RF without reboot when possible. */
  bool setRadioParams(float freq_mhz, float bw_khz, uint8_t sf, uint8_t cr, int8_t tx_dbm, float airtime_factor);
  /** Bitmask uses same bits as firmware `AUTO_ADD_*` in MyMesh.cpp. */
  void setAutoAddConfig(uint8_t mask, uint8_t max_hops, uint8_t manual_add);
  void setAdvertLocationPolicy(uint8_t policy);
  void setPathHashMode(uint8_t mode);
  void setExperimentalFlags(uint8_t multi_acks, uint8_t client_repeat, uint8_t rx_boosted);
  /** Meshcomod CLI on device: `wifi on` / `wifi off`. */
  bool setWifiRadio(bool on);
  bool isTcpEnabled() const { return _serial && _serial->isTcpEnabled(); }
  void enableTcp() { if (_serial) _serial->enableTcp(); }
  void disableTcp() { if (_serial) _serial->disableTcp(); }
  bool hasBleCapability() const { return _serial && _serial->hasBleCapability(); }
  bool isBleEnabled() const { return _serial && _serial->isBleEnabled(); }
  void enableBle() { if (_serial) _serial->enableBle(); }
  void disableBle() { if (_serial) _serial->disableBle(); }
  int getWsConnectedCount() const { return _serial ? _serial->getWsConnectedCount() : 0; }
  void setDeviceTimeFromSystemClock();
  /** Mark recent user input — call when touch / hw button is detected. */
  void noteUserInput();
  /** Get / set the screen-off-after-idle timeout (0 = never). Persists in NVS. */
  uint16_t getScreenTimeoutSecs() const;
  bool setScreenTimeoutSecs(uint16_t seconds);
  /** Force the panel back on (clears _screen_off and updates last-input). */
  void wakeScreen();
  /** Turn the panel off and manually lock it (touch ignored until a deliberate
   *  unlock — a trackball/BOOT-button press). Same state the V4 lock button sets. */
  void lockScreen();
  /** Reveal the lock screen (light the panel) without unlocking — for a key /
   *  trackball press while hard-locked. No-op unless manually locked. */
  void lockscreenReveal();
  /** Release a manual lock: hide the lock screen and turn the panel back on. */
  void unlockScreen();
  /** True while the panel backlight is off (idle-dimmed or manually locked). */
  bool isScreenOff() const { return _screen_off; }
  /** True while hard-locked (manual lock engaged), whether lit or dark. */
  bool isManualLock() const { return _manual_lock; }
  bool sendAdvertNow();         // legacy: zero-hop
  bool sendAdvertFlood();       // multi-hop flood
  bool sendAdvertZeroHop();     // explicit zero-hop (same as sendAdvertNow)
  void rebootDevice();
  // Synchronously persist chat history to flash. Call before any path that
  // restarts the device (Wi-Fi/BLE mode switch, etc.) so recent chat isn't
  // lost — the periodic flush is off-thread and rate-capped.
  void persistHistoryNow();

  // from AbstractUITask
  void msgRead(int msgcount) override;
  void newMsgFromPub(uint8_t path_len, const uint8_t* from_pub, const char* from_name, const char* text, int msgcount) override;
  void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) override;
  // Meta-aware variant — MyMesh calls this from the packet receive path so the
  // chat bubble can persist hops/SNR/RSSI for the per-message Info popup.
  // The base newMsgFromPub still works for callers that don't have metadata
  // (history loads, etc).
  void newMsgFromPubWithMeta(uint8_t path_len, bool is_flood,
                              const uint8_t* from_pub, const char* from_name,
                              const char* text, int msgcount,
                              int8_t snr_q4, int8_t rssi) override;
  void notify(UIEventType t = UIEventType::none) override;
  void logRxFrame(float snr, float rssi, const uint8_t* raw, int len) override;
  void discoveredContact(const ContactInfo& contact, bool is_new, uint8_t path_len) override;
  void onPingReply(const ContactInfo& contact, const uint8_t* data, size_t len) override;
  void onTelemetryReply(const ContactInfo& contact, const uint8_t* data, size_t len) override;
  void onAdminLoginResult(const ContactInfo& contact, bool success, uint8_t perms) override;
  void onAdminCommandReply(const ContactInfo& contact, const char* text) override;
  void onThreadsChanged() override;
  void loop() override;

  void shutdown(bool restart = false);
};
