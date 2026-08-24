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
  static const int MAX_UI_MESSAGES = 500;
  /** Deep ring for devices whose chat history lives on an SD card (T-Deck with a
   *  card, Tanmatsu SD_MMC): 10x the internal-flash ring. Chosen at begin() into
   *  _ui_msg_cap; PSRAM cost ~5000 * sizeof(UIMessage) ≈ 1.3 MB (of 8 MB). */
  static const int MAX_UI_MESSAGES_SD = 5000;
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
    // The scope code is an HMAC of THIS packet's payload under the sender's
    // region key, so it differs for every message and cannot be read as a region
    // id (#259). The only thing a receiver can say about it is whether it
    // verifies against a key we hold — which we check at RX, while the packet is
    // still around, and record here.
    MSG_META_SCOPE_HOME = (1u << 3),  // in_scope verified against OUR region key
    // Bits 4-7: which REGISTERED region in_scope verified against (#271), as a
    // RegionRegistry slot, where 0 = none/unknown, 15 = several regions matched
    // and 1..14 name one. Packed into the spare top nibble on purpose: meta_flags is
    // already persisted at a fixed offset, so this costs no record growth and no
    // history version bump, and messages written before a region was registered
    // read back 0, which is exactly the honest "unknown" state. A slot is stable
    // for the life of the history and is never a list index, so deleting or
    // reordering regions cannot relabel old messages.
    MSG_META_SCOPE_SLOT_SHIFT = 4,
    MSG_META_SCOPE_SLOT_MASK  = 0xF0,
  };
  static uint8_t metaScopeSlot(uint8_t meta_flags) {
    return (uint8_t)((meta_flags & MSG_META_SCOPE_SLOT_MASK) >> MSG_META_SCOPE_SLOT_SHIFT);
  }
  static uint8_t metaWithScopeSlot(uint8_t meta_flags, uint8_t slot) {
    return (uint8_t)((meta_flags & ~MSG_META_SCOPE_SLOT_MASK)
                     | ((slot & 0x0F) << MSG_META_SCOPE_SLOT_SHIFT));
  }

  struct UIMessage {
    uint32_t ts;
    // Monotonic per-record sequence number, assigned at append and persisted by
    // the segmented store (its record-to-segment mapping keys on seq ranges).
    // NEVER reset mid-session; the boot loader seeds the generator from
    // max(loaded seq)+1. Distinct from _msgcount, which the companion protocol
    // may overwrite (msgRead) and therefore cannot be a unique key.
    uint32_t seq;
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
  // Burn-in guard for the lit lock screen (#55): stamped when the lock screen goes from off->lit;
  // the loop dims a hard-locked LIT panel a bounded time after this, independent of the screen-timeout
  // setting and of held/ghost touches that keep re-revealing it. 0 = not currently lit-locked.
  uint32_t _lock_lit_ms = 0;
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
  /** Next UIMessage::seq to hand out. Seeded by the loader (max loaded seq + 1),
   *  strictly monotonic for the session. See UIMessage::seq. */
  uint32_t _ui_seq_next = 1;
  int _ui_msg_count;
  int _ui_msg_head;
  /** Runtime ring capacity: MAX_UI_MESSAGES_SD when history lives on an SD card,
   *  else MAX_UI_MESSAGES. Fixed for the whole boot (chosen before the PSRAM
   *  alloc in begin()); the loader linearizes files written under another cap. */
  int _ui_msg_cap = MAX_UI_MESSAGES;
  /** "Does this thread have any stored message?", answered in O(1).
   *
   *  threadHasMessageHistory() used to answer it by walking the message ring looking for a
   *  match, and the inbox/unread paths call it once PER THREAD. A thread that HAS recent
   *  messages exits that scan early; a thread with none scans every record in the ring doing
   *  a strncmp each — and "no history" is exactly what those callers are testing for. With a
   *  busy public channel filling the ring (a field report: 3800 messages) each inbox refresh
   *  became tens of thousands of string compares, which is the reported "interface slows down
   *  drastically", and why it was still felt at only ~300 messages.
   *
   *  Appends set the owning thread's flag directly, so the common path never scans. Anything
   *  that REMOVES messages (ring eviction, delete, clear, a fresh load) can only invalidate
   *  the flags, so it just marks them dirty and the next reader rebuilds all threads in ONE
   *  ring pass instead of one pass per thread. */
  mutable uint16_t _thread_msgs[MAX_UI_THREADS] = { 0 };
  mutable bool     _thread_hist_dirty = true;
  void rebuildThreadHistoryFlags() const;
  /** Drop this thread's oldest stored messages until it is back within the per-chat cap
   *  (touchPrefsGetHistPerChat; 0 = uncapped). Called after an append. */
  void enforceHistoryCap(int thread_idx);
  unsigned long _next_thread_seed;
  UIMessage* _ui_msgs   = nullptr;   // ring of recent messages — PSRAM-allocated in begin()
  UIThread*  _ui_threads = nullptr;  // thread table — PSRAM-allocated in begin()
  void allocMessageStore();          // ring + thread table; also used by console mode
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
  char _compose_buf[MAX_MSG_TEXT + 1];   // full LoRa text length; was 128 -> silently chopped sends at 127 bytes (GH #119)
  unsigned long _next_mesh_thread_refresh;
  TouchUiScreen _touch_screen;
  bool _active_dm_contact_set;
  uint8_t _active_dm_contact_pub[32];
  bool _threads_dirty;
  unsigned long _next_threads_flush_ms;
  bool _msgs_dirty;
  unsigned long _next_msgs_flush_ms;

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
                  uint8_t meta_flags, int8_t snr_q4, int8_t rssi,
                  const char* sender_override = nullptr);
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
  void setActiveThread(int idx, bool channel_mode);
  void resetComposer();
  void appendComposerChar(char c);
  void appendComposerText(const char* text);
  void backspaceComposerChar();
  bool sendComposerToActiveThread(const char* override_text);
  void markThreadsDirty(unsigned long delay_ms = 200);
  void markMsgsDirty(unsigned long delay_ms = 2000);
  void flushHistoryIfDue(unsigned long now);
  bool loadHistoryFromStorage();
  bool loadThreadsFromStorage();
  bool loadMsgsFromStorage();
  bool loadLegacyHistoryFromStorage();
  bool loadMsgsFromSegments();     // segmented store (generation 3) loader
  bool migrateRingToSegments();    // one-time old-format -> segments migration (verify-then-delete)
  bool saveThreadsToStorage();
  bool saveMsgsToStorage();

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
    _threads_dirty = false;
    _next_threads_flush_ms = 0;
    _msgs_dirty = false;
    _next_msgs_flush_ms = 0;
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
  // Console mode: list threads with their unread counts (read-only), and clear
  // one deliberately. Kept separate so the monitor cannot mark anything read.
  int  consoleThreadAt(int idx, char* name, size_t cap, int* unread, bool* is_channel);
  bool consoleMarkThreadRead(const char* name);
  int  consoleHistoryAt(const char* thread, int back, char* sender, size_t sc,
                        char* text, size_t tc, uint32_t* ts, bool* outgoing);
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
  int  getThreadMessageIndexes(int thread_idx, int out_indexes[], int max_out, bool newest_first) const;   // read any thread's message ring slots (no active-thread side effect)
  bool deleteMessageBySlot(int msg_idx);          // tombstone one ring slot (long-press Delete)
  int  clearThreadHistory(int thread_idx);        // tombstone every message of a thread; returns count
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
  // override_text != nullptr resends that exact text (msg action menu "Resend"); nullptr = composer draft.
  bool composerSend(const char* override_text = nullptr) { return sendComposerToActiveThread(override_text); }
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
    // _ui_threads is null in console mode (allocated later in begin()).
    if (!_ui_threads) return -1;
    if (!_active_thread_is_channel || _active_thread_idx < 0 || _active_thread_idx >= MAX_UI_THREADS) return -1;
    return _ui_threads[_active_thread_idx].mesh_channel_slot;
  }
  int  activeThreadIdx() const { return _active_thread_idx; }
  /** Any thread's pinned mesh-channel slot (-1 when not a channel / out of range).
   *  chatDeleteApply prefers this (name-validated) over a pure name scan so
   *  deleting a channel actually drops its mesh-table entry — otherwise
   *  refreshThreadsFromMesh() recreates the thread from the surviving channel. */
  int16_t threadMeshChannelSlot(int idx) const {
    if (!_ui_threads) return -1;                       // console mode: no table
    if (idx < 0 || idx >= MAX_UI_THREADS || !_ui_threads[idx].used || !_ui_threads[idx].channel) return -1;
    return _ui_threads[idx].mesh_channel_slot;
  }
  /** Message-ring capacity this boot (500, or 5000 with SD-backed history). */
  int  msgCap() const { return _ui_msg_cap; }
  /** DM thread's full contact pubkey (32 B) — for the chat sheet's "Reset path".
   *  False for channels, unused slots, or a thread with no pubkey mapping yet. */
  bool getThreadContactPub(int idx, uint8_t out[32]) const {
    if (idx < 0 || idx >= MAX_UI_THREADS) return false;
    if (!_ui_threads[idx].used || _ui_threads[idx].channel) return false;
    const uint8_t* p = _ui_threads[idx].mesh_contact_pub;
    uint8_t any = 0;
    for (int i = 0; i < 32; i++) { out[i] = p[i]; any |= p[i]; }
    return any != 0;
  }
  /** Newest message of a thread (chat-list preview): scans the ring by the
   *  thread's name + kind and returns the latest by timestamp. False when the
   *  thread has no messages in the ring. */
  bool getThreadLastMessage(int idx, char* sender, size_t sender_cap,
                            char* text, size_t text_cap, bool* outgoing) const {
    if (idx < 0 || idx >= MAX_UI_THREADS || !_ui_threads[idx].used || !_ui_msgs) return false;
    const bool ch  = _ui_threads[idx].channel;
    const char* nm = _ui_threads[idx].name;
    // Newest by RING ORDER, not by timestamp. m.ts is taken from the ESP32 system clock,
    // which is not monotonic across reboots: it restarts from ESP32RTCClock::begin()'s
    // power-on seed every boot and only climbs with uptime until a real time source lands.
    // Picking max(ts) therefore let a message received hours into an EARLIER boot outrank
    // every message from this one, so the chat-list preview stuck on an old message and
    // never updated (reported on the T-Display P4, whose system clock stayed on that seed
    // because it has an RTC chip — see the mirror in ClockFloorRTC). Ring order is the
    // actual arrival order and cannot be wrong, so walk back from the head and take the
    // first match; that also exits immediately instead of scanning the whole ring.
    for (int i = 0; i < _ui_msg_count; ++i) {
      const int slot = (_ui_msg_head - 1 - i + _ui_msg_cap) % _ui_msg_cap;
      const UIMessage& m = _ui_msgs[slot];
      if (!m.text[0] || m.channel != ch) continue;
      if (strncmp(m.thread, nm, MAX_THREAD_NAME) != 0) continue;
      if (sender && sender_cap) { strncpy(sender, m.sender, sender_cap - 1); sender[sender_cap - 1] = '\0'; }
      if (text && text_cap)     { strncpy(text,   m.text,   text_cap - 1);   text[text_cap - 1] = '\0'; }
      if (outgoing) *outgoing = m.outgoing;
      return true;
    }
    return false;
  }
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
#if defined(HAS_EXPANSION_KIT)
  // Heltec V4 Expansion Kit: snapshot of the locally-attached sensor rail
  // (battery, BME280, GXHTV3/SHT4X) plus the GPS/buzzer module presence.
  struct LocalEnvSnapshot {
    bool query_ok = false;
    bool have_batt = false;
    float batt_v = 0.0f;
    bool have_bme_temp = false;
    bool have_bme_hum = false;
    bool have_bme_pressure = false;
    bool have_bme_alt = false;
    float bme_temp_c = 0.0f;
    float bme_hum_pct = 0.0f;
    float bme_pressure_hpa = 0.0f;
    int16_t bme_alt_m = 0;
    bool have_gxhtv3_temp = false;
    bool have_gxhtv3_hum = false;
    float gxhtv3_temp_c = 0.0f;
    float gxhtv3_hum_pct = 0.0f;
    bool gps_present = false;
    bool gps_enabled = false;
    bool gps_fix = false;
    int  gps_sats = -1;
    bool buzzer_available = false;
    bool buzzer_quiet = true;
  };
  bool getLocalEnvSnapshot(LocalEnvSnapshot& out) const;
  bool getLocalEnvSummary(char* buf, size_t cap) const;
#endif
  /** True if the GPS currently reports a valid fix. */
  bool getGpsFix();
  /** Satellites currently in view, or -1 if unknown / no GPS hardware. */
  int  getGpsSats();
  /** UTC epoch the GPS itself has decoded, or 0 if it has none yet. A receiver decodes TIME from
   *  the satellite stream well before it can solve a POSITION, so a valid time with no fix is
   *  positive proof the module is alive and tracking, not dead. That is the distinction the GPS
   *  page could not previously show, and it is what "acquiring..." was hiding. */
  uint32_t getGpsTime();
  /** Altitude in metres from the last fix (0 when there is no fix). */
  int  getGpsAltitude();
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
  void setTelemetryAllow(bool on);   // answer mesh telemetry requests (battery+env; location stays separate)
  void setLocationTelemetryMode(uint8_t mode);   // TELEM_MODE_* — position on request (#266)
  /** Meshcomod CLI on device: `wifi on` / `wifi off`. */
  bool setWifiRadio(bool on);
  bool isTcpEnabled() const { return _serial && _serial->isTcpEnabled(); }
  void enableTcp() { if (_serial) _serial->enableTcp(); }
  void disableTcp() { if (_serial) _serial->disableTcp(); }
  bool hasBleCapability() const { return _serial && _serial->hasBleCapability(); }
  bool isBleEnabled() const { return _serial && _serial->isBleEnabled(); }
  // Live BLE enable. The concrete transport applies its heap guard only for a
  // cold NimBLE allocation; re-enabling a pre-created stack is allocation-free.
  // Returns false when a required cold start cannot be made safely.
  bool enableBle();
  void disableBle() { if (_serial) _serial->disableBle(); }
  int getWsConnectedCount() const { return _serial ? _serial->getWsConnectedCount() : 0; }
  /** Push the ESP32 system clock into the mesh RTC. false = never synced, mesh clock untouched. */
  bool setDeviceTimeFromSystemClock();
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
  bool isManualLocked() const { return _manual_lock; }   // hard screen lock engaged
  void toggleScreenLock();                                // Tanmatsu Vol- long-press: lock <-> unlock
  void sleepScreen();                                     // soft screen sleep (backlight off, not locked)
  /** True while hard-locked (manual lock engaged), whether lit or dark. */
  bool isManualLock() const { return _manual_lock; }
  bool sendAdvertNow();         // legacy: zero-hop
  bool sendAdvertFlood();       // multi-hop flood
  bool sendAdvertZeroHop();     // explicit zero-hop (same as sendAdvertNow)
  bool sendSignalProbe();       // trace-ping the nearest repeater (non-flooding); falls back to zero-hop advert
  void rebootDevice();
  // Synchronously persist chat history to flash. Call before any path that
  // restarts the device (Wi-Fi/BLE mode switch, etc.) so recent chat isn't
  // lost — the periodic flush is off-thread and rate-capped. Overrides the
  // AbstractUITask hook so the companion CMD_REBOOT path flushes too.
  void persistHistoryNow() override;
  void flushHistorySoon();   // arm an immediate OFF-THREAD flush (worker) — no min-delay clamp, no UI stall

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
  void newRoomMsgFromPubWithMeta(uint8_t path_len, bool is_flood,
                                 const uint8_t* from_pub, const char* from_name,
                                 const char* author_name,
                                 const char* text, int msgcount,
                                 int8_t snr_q4, int8_t rssi) override;
  void appSentMsgToContact(const uint8_t* to_pub, const char* to_name, const char* text,
                           uint32_t ack_hash, uint32_t sent_fp = 0) override;
  void appSentMsgToChannel(const char* channel_name, const char* text, uint32_t sent_fp = 0) override;
  void notify(UIEventType t = UIEventType::none) override;
  void logRxFrame(float snr, float rssi, const uint8_t* raw, int len) override;
  void discoveredContact(const ContactInfo& contact, bool is_new, uint8_t path_len) override;
  void onPingReply(const ContactInfo& contact, const uint8_t* data, size_t len) override;
  void onTelemetryReply(const ContactInfo& contact, const uint8_t* data, size_t len) override;
  void onAdminLoginResult(const ContactInfo& contact, bool success, uint8_t perms) override;
  void onServerClock(const ContactInfo& contact, uint32_t server_epoch) override;
  void onAdminCommandReply(const ContactInfo& contact, const char* text) override;
  void onThreadsChanged() override;
  void loop() override;

  void shutdown(bool restart = false);
};

// True while the touch-UI "Spectrum" RF-analyzer app owns the radio. main.cpp's
// loop() checks this and SKIPS the_mesh.loop() while it's true, so the mesh never
// re-tunes / re-arms RX on the home channel while the analyzer sweeps the band.
// Defined in UITask.cpp (returns a static flag set on open / cleared after the
// radio is restored to the mesh config on close).
bool spectrumOwnsRadio();
