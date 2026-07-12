#pragma once

/* NVS-backed touch-UI prefs (screen timeout etc). Kept separate from
 * NodePrefs (which is file-persisted and shared across firmware variants)
 * so we don't risk breaking on-disk layout. */

#if defined(ESP32)

#include <stdint.h>
#include <stddef.h>   // size_t (blob helpers below)

void touchPrefsBegin();
// Force a fresh load of the settings blob. Call after SdNvsPrefs::useFile() at
// boot: an earlier pref read may have cached the blob from the legacy backend.
void touchPrefsReload();

/** Screen timeout in seconds; 0 = never sleep. Default 20. */
uint16_t touchPrefsGetScreenTimeoutSecs();
bool touchPrefsSetScreenTimeoutSecs(uint16_t seconds);

/** Display backlight brightness, 5–100 %. Default 100. */
uint8_t touchPrefsGetBrightness();
bool    touchPrefsSetBrightness(uint8_t pct);

/** Keyboard backlight mode: 0 = off, 1 = on, 2 = auto (on while typing). Default auto. */
uint8_t touchPrefsGetKbBacklight();
bool    touchPrefsSetKbBacklight(uint8_t mode);

/** Currently active keyboard layout. 0 = English, 1 = Bulgarian phonetic.
 *  Persisted so the device boots back into the last-used layout. */
uint8_t touchPrefsGetKeyboardLayout();
bool    touchPrefsSetKeyboardLayout(uint8_t layout);

/** Secondary keyboard preference. 0 = None (default), 1 = Bulgarian phonetic.
 *  When None, double-space does nothing. When set, double-space toggles
 *  between English and the selected secondary layout. */
uint8_t touchPrefsGetSecondaryKeyboard();
bool    touchPrefsSetSecondaryKeyboard(uint8_t secondary);

/** Enabled-layout bitmask for the keyboard space-cycle. Bit (1<<KeyboardLayoutId)
 *  marks a layout as part of the cycle; English (bit 0) is always implicit.
 *  Supersedes the single-secondary pref above — when the mask has never been
 *  written, the getter migrates the legacy secondary value into a one-bit mask. */
uint16_t touchPrefsGetEnabledLayouts();
bool     touchPrefsSetEnabledLayouts(uint16_t mask);

/** Keyboard accent-popup picker. When ON (default), typing a Latin letter that
 *  has accented variants pops up a tap-to-pick box; OFF means plain typing. */
bool touchPrefsGetAccentPopups();
bool touchPrefsSetAccentPopups(bool on);

/** Web control panel: serve the live UI to a phone browser over the WebSocket
 *  server and inject taps back (remote control). Opt-in, default OFF. */
bool touchPrefsGetWebMirror();
bool touchPrefsSetWebMirror(bool on);

/** Remote mode: on boot, render the UI to an off-screen web resolution (480x800)
 *  instead of the physical panel, served to a browser (headless/remote use). It is
 *  a boot mode (reboot to apply). Opt-in, default OFF. */
bool touchPrefsGetRemoteMode();
bool touchPrefsSetRemoteMode(bool on);

/** Remote-mode orientation: landscape (800x480, for desktop) vs portrait (480x800, for
 *  phones). Applied at boot (reboots to change). Default portrait. */
bool touchPrefsGetRemoteLandscape();
bool touchPrefsSetRemoteLandscape(bool on);
bool touchPrefsGetWebTerminal();
bool touchPrefsSetWebTerminal(bool on);

/** UI language index (UiLang enum in i18n.h; 0 = English). Read at boot. */
uint8_t touchPrefsGetUiLang();
bool    touchPrefsSetUiLang(uint8_t lang);

/** User-configurable quick-reply macros: up to 6 short strings the user can
 *  drop into the composer with a single tap (e.g. "ok", "on the way",
 *  "stuck — wait"). idx is 0..5; max length 31 chars + null. Returns the
 *  text length actually written into `out` (0 if the slot is empty or idx
 *  is out of range). `out` is always null-terminated when out_cap > 0. */
constexpr int TOUCH_QUICK_REPLY_COUNT  = 6;
constexpr int TOUCH_QUICK_REPLY_MAXLEN = 32;
int  touchPrefsGetQuickReply(int idx, char* out, int out_cap);
bool touchPrefsSetQuickReply(int idx, const char* text);

/** Show the live duty-cycle meter on the Home tab? Default true. The meter
 *  surfaces the regulator-imposed TX budget so the user can see when they're
 *  about to be throttled (LoRa duty caps in the 868/915 MHz ISM bands hit
 *  ~10%/50% depending on region). */
bool touchPrefsGetDutyMeterShown();
bool touchPrefsSetDutyMeterShown(bool show);

/** Distance units for the UI (contact distance badge, map, etc.).
 *  false = kilometres (default), true = miles. Pure display preference. */
bool touchPrefsGetUseMiles();
bool touchPrefsSetUseMiles(bool use_miles);

/** Map tile source: false = tile server + on-device cache (default), true = read tiles off the
 *  microSD card (/tiles/<z>/<x>/<y>.jpg). T-Deck only (the V4 TFT has no SD slot). */
bool touchPrefsGetTilesFromSd();
bool touchPrefsSetTilesFromSd(bool from_sd);

/** Map night mode: invert tile colours at render time (cache stays untouched).
 *  Default false. */
bool touchPrefsGetMapNight();
bool touchPrefsSetMapNight(bool on);

/** Last map zoom level, persisted so the map reopens where the user left it.
 *  0 = unset (let the auto-snap pick a level for the available tile pack). */
uint8_t touchPrefsGetMapZoom();
bool    touchPrefsSetMapZoom(uint8_t z);

/** Per-element visibility of the map's on-screen text/markers. All default true
 *  (shown), so existing installs are unchanged. */
bool touchPrefsGetMapShowCoords();
bool touchPrefsSetMapShowCoords(bool on);
bool touchPrefsGetMapShowTileXYZ();
bool touchPrefsSetMapShowTileXYZ(bool on);
bool touchPrefsGetMapTileDebug();
bool touchPrefsSetMapTileDebug(bool on);
bool touchPrefsGetMapShowContacts();
bool touchPrefsSetMapShowContacts(bool on);
bool touchPrefsGetMapShowLinks();
bool touchPrefsSetMapShowLinks(bool on);

/** Map tile style: 0 = OpenStreetMap (default), 1 = OpenTopoMap (topographic, opt-in). */
uint8_t touchPrefsGetMapStyle();
bool    touchPrefsSetMapStyle(uint8_t style);

/* App drawer: large grid (one fewer column → bigger icons + labels, for low vision).
 * Default false = the compact grid (T-Deck 4 cols / Heltec V4 3 cols). */
bool touchPrefsGetAppGridLarge();
bool touchPrefsSetAppGridLarge(bool on);
/** Idle light-sleep feature: when ON, the device enters light sleep while the
 *  screen is off and all activity gates pass. Default OFF. */
bool touchPrefsGetSleepIdle();
bool touchPrefsSetSleepIdle(bool on);

/* UI resolution scale (Tanmatsu): 0=100% (native 800x480), 1=150%, 2=200%. Applied at boot —
 * LVGL renders at a lower resolution and the flush upscales to the panel. Reboot to apply. */
uint8_t touchPrefsGetUiScale();
bool    touchPrefsSetUiScale(uint8_t scale);

/* T-Deck keyboard navigation: false (default) = keys type / jump tabs as usual;
 * true = when no text field is focused, the WASDZ cluster moves focus — W up, A
 * left, Z down, D right, S select, Q back — and the programmable tab hotkeys
 * (touchPrefsGetNavKey) jump straight to a tab. The trackball always stays a soft
 * mouse cursor. Applied live (no reboot). */
bool    touchPrefsGetKbdNav();
bool    touchPrefsSetKbdNav(bool on);

/* T-Deck trackball mode: 1 = drive the focus-group D-pad nav (up/down/left/right move the
 * selection, centre click selects — same logic as the Tanmatsu keypad); 0 = the soft mouse
 * cursor. Default 1. Independent of the keyboard ESDFX nav above. Applied live (no reboot). */
bool    touchPrefsGetTbNav();
bool    touchPrefsSetTbNav(bool on);
/* Opt-in (issue #64): when true, direct/login/admin unicast floods are tagged with the
 * node's default region scope so a region-scoped repeater that is your ONLY path will
 * re-flood them. Default false (unscoped) — leaving it off keeps cross-region login/DMs. */
bool    touchPrefsGetScopeDirect();
bool    touchPrefsSetScopeDirect(bool on);
/* Heltec V4.3 high-gain FEM LNA (~17 dB external receive amplifier). Default false (bypassed,
 * matching the hardware default). On = better sensitivity in quiet areas; can desensitize in
 * noisy/urban areas. Only meaningful on V4.3 (board.femLnaControllable()). */
bool    touchPrefsGetFemLna();
bool    touchPrefsSetFemLna(bool on);

/* New-message notify flash (T-Deck): briefly light the keyboard backlight + wake the screen when a
 * message arrives, like the Tanmatsu envelope LED. Opt-in (default off). */
bool    touchPrefsGetMsgFlash();
bool    touchPrefsSetMsgFlash(bool on);

/* Advertise on boot (#76): fire one flood self-advert ~6s after boot so peers with auto-add on
 * refresh our pubkey (useful after a reflash wiped storage). Opt-in, default off. All boards. */
bool    touchPrefsGetBootAdvert();
bool    touchPrefsSetBootAdvert(bool on);

/* Compact chat (IRC-style): one dense "HH:MM name: text" row per message instead of
 * bubbles, so far more history fits on screen. Opt-in, default off (bubbles). */
bool    touchPrefsGetCompactChat();
bool    touchPrefsSetCompactChat(bool on);

/* Buffered LoRa receive (experimental): a high-priority drain task lifts each packet
 * out of the radio within ~1 ms so UI-thread stalls can't overwrite unread packets
 * (the missed-messages class). Opt-in, default off = stock receive path. */
bool    touchPrefsGetRxQueue();
bool    touchPrefsSetRxQueue(bool on);
uint32_t touchPrefsGetClockFloor();               // monotonic send-timestamp floor (#89)
bool    touchPrefsSetClockFloor(uint32_t epoch);  // only ever grows; no-op below current

/* Periodic self-advert intervals (the standard MeshCore flood/local advert, on a timer). 0 = off.
 * Flood in hours; local zero-hop in minutes (0 or 60-240). Scheduled in UITask::loop via sendAdvert. */
uint16_t touchPrefsGetFloodAdvHrs();
bool     touchPrefsSetFloodAdvHrs(uint16_t hrs);
uint16_t touchPrefsGetLocalAdvMin();
bool     touchPrefsSetLocalAdvMin(uint16_t mins);

/* OTA update channel: opt-in to test/beta firmware. When on, the on-device update
 * check AND the Install-update download both point at the beta channel (releases/BETA)
 * instead of the stable channel (releases/TOUCH). Default off (stable). */
bool    touchPrefsGetBetaUpdates();
bool    touchPrefsSetBetaUpdates(bool on);

/* Keyboard-nav tab hotkeys: the ASCII key that jumps to each main tab while
 * keyboard navigation is on. `tab` is the tab index 0..4 = chat / contacts / home
 * / map / settings. Defaults E/R/T/U/I. Programmable in Settings → Keyboard. */
uint8_t touchPrefsGetNavKey(int tab);
bool    touchPrefsSetNavKey(int tab, uint8_t ch);

/* Map zoom control style: false (default) = a zoom slider (toggled by the on-map
 * button); true = a +/- button pair like older builds. Set in the Map options popup. */
bool    touchPrefsGetMapZoomButtons();
bool    touchPrefsSetMapZoomButtons(bool on);

/* Keyboard-nav control keys (ASCII), one per action: idx 0..5 = up, down, left,
 * right, select, back; 6..7 = scroll up, scroll down. Defaults W/Z/A/D/S/Q/F/C.
 * Programmable in Settings → Keyboard. */
uint8_t touchPrefsGetNavDirKey(int idx);
bool    touchPrefsSetNavDirKey(int idx, uint8_t ch);

/* Home tab default: false (default) = the Commander screen; true = the app drawer
 * (for users who prefer the launcher as their home). Toggled in the app drawer's cog. */
bool    touchPrefsGetHomeIsDrawer();
bool    touchPrefsSetHomeIsDrawer(bool on);

/** Hide the device/profile name in the status bar and move the clock to the
 *  left where the name used to be. Default false (name shown). */
bool touchPrefsGetHideNodeName();
bool touchPrefsSetHideNodeName(bool hide);

/** Show the low-key toast/chip when a contact is auto-discovered/added.
 *  Default true (toast shown). Off keeps the diag-log line + tab badge. */
bool touchPrefsGetNewContactToast();
bool touchPrefsSetNewContactToast(bool on);

/** Heltec V4 Expansion Kit only: show the bottom "Sensors" tab + the Home
 *  environment widget. Default true. Combined with a runtime check for any
 *  ENVIRONMENT sensor actually being present, so a bare V4 hides the UI even
 *  with this on. Read in buildUiTree → applies after a restart. */
bool touchPrefsGetShowSensorsTab();
bool touchPrefsSetShowSensorsTab(bool on);

/** Store all device data (identity/prefs/contacts/channels) on the SD card under
 *  /meshcomod instead of internal SPIFFS. T-Deck only; read at boot before data
 *  loads, so changing it requires a reboot. Default false (SPIFFS). */
bool touchPrefsGetUseSdStorage();
bool touchPrefsSetUseSdStorage(bool use_sd);
/** Boot-time read for main.cpp's storage decision (before SdNvsPrefs::useFile).
 *  Checks NVS, then SPIFFS /prefs/touch.kv for a file-only write. */
bool touchPrefsReadUseSdAtBoot();

/** Global UI orientation, applied at boot before the screens are built so the
 *  whole layout reflows to the rotated resolution. Stored as the raw LVGL
 *  rotation code: 0 = portrait (LV_DISP_ROT_NONE), 1 = 90° (LV_DISP_ROT_90),
 *  2 = 180°, 3 = 270° (LV_DISP_ROT_270). Default 0. Distinct from the
 *  transient keyboard-landscape toggle ("kbrot"); changing it reboots the
 *  device so the UI rebuilds at the new orientation. */
uint8_t touchPrefsGetUiRotation();
bool    touchPrefsSetUiRotation(uint8_t rot);

/** Calibrated battery "full" voltage in mV — the reading captured when the pack
 *  was fully charged, treated as 100%. 0 = not calibrated (use the 4200 mV
 *  default Li-ion full point). Lets custom batteries / builds read 100%. */
uint16_t touchPrefsGetBattFullMv();
bool     touchPrefsSetBattFullMv(uint16_t mv);

/** Saved Wi-Fi profile slots. The "active" credentials still live in the
 *  meshcomod NVS namespace (WifiRuntimeStore.cpp) and are what
 *  wifiConfigApply() uses; these slots are a touch-UI convenience so the
 *  operator can switch between home / office / hotspot without retyping the
 *  passphrase. Activating a slot copies its ssid+pwd into the active store
 *  and requests an apply. idx is 0..2.
 *  Each slot holds a short user-chosen label (e.g. "home"), the ssid, and
 *  the pwd. All strings are zero-terminated; pass empty string to clear. */
constexpr int TOUCH_WIFI_SLOT_COUNT  = 3;
constexpr int TOUCH_WIFI_LABEL_MAX   = 16;
bool touchPrefsGetWifiSlot(int idx, char* label, int label_cap,
                           char* ssid, int ssid_cap,
                           char* pwd, int pwd_cap);
bool touchPrefsSetWifiSlot(int idx, const char* label,
                           const char* ssid, const char* pwd);
/** Apply slot idx as the active Wi-Fi credentials and request a reconnect.
 *  Returns false if idx out of range or the slot is empty. */
bool touchPrefsActivateWifiSlot(int idx);

/** Saved "known networks" (iPhone/Android-style) — the reworked Wi-Fi store.
 *  Up to 8 networks, each {ssid, passphrase, auto-join flag, recency rank}.
 *  Rank is a monotonic counter bumped on connect: highest = most recently used
 *  (drives the saved-list order + which entry is evicted when full). The "active"
 *  credentials still live in the meshcomod NVS namespace; connecting copies a
 *  saved network into it + requests an apply. ssid/pwd sizes mirror
 *  WIFI_CONFIG_SSID_MAX (32) / WIFI_CONFIG_PWD_MAX (64). */
constexpr int TOUCH_WIFI_NET_COUNT = 8;
struct TouchWifiNet {
  bool     used;
  bool     auto_join;
  uint32_t rank;
  char     ssid[33];
  char     pwd[65];
};
/** Read saved network idx (0..7). out.used == false when the slot is empty. */
bool touchPrefsGetWifiNet(int idx, TouchWifiNet& out);
/** Index of the saved network with this exact ssid, or -1. */
int  touchPrefsFindWifiNet(const char* ssid);
/** Add or update a network by ssid (preserves the old passphrase when pwd is
 *  empty), bumping its recency. Evicts the least-recent when full. -> idx / -1. */
int  touchPrefsSaveWifiNet(const char* ssid, const char* pwd, bool auto_join);
/** Delete saved network idx. */
bool touchPrefsForgetWifiNet(int idx);
/** Set the auto-join flag on saved network idx. */
bool touchPrefsSetWifiNetAutoJoin(int idx, bool on);
/** Load saved network idx into the active credentials + request a reconnect,
 *  and bump its recency so a reboot reconnects to it. Returns false if empty. */
bool touchPrefsConnectWifiNet(int idx);

/** Favorite contacts. Identified by the first 6 bytes of their pubkey — same
 *  prefix the firmware uses for short-key contact lookups — so 16 favorites
 *  fit in a 96-byte NVS blob. Pure UI metadata; the firmware contact table
 *  is untouched.
 *  • touchPrefsIsFavorite: returns true if the 6-byte prefix is stored.
 *  • touchPrefsSetFavorite: add/remove; returns the new state (true = is
 *    favorite after the call). Silently ignores adds past the cap (16). */
constexpr int TOUCH_FAVORITES_MAX = 16;
constexpr int TOUCH_FAVORITE_KEY_BYTES = 6;
bool touchPrefsIsFavorite(const uint8_t* pub_key6);
bool touchPrefsSetFavorite(const uint8_t* pub_key6, bool fav);

/** Copy all stored favorite prefixes into `out_buf` in one NVS read. Returns
 *  the number of records actually copied (0 .. TOUCH_FAVORITES_MAX). Used
 *  by the contact-list refresh which would otherwise hit NVS once per
 *  contact — measurably slow when the contact table grows past a couple
 *  dozen entries.
 *  `out_buf` must be at least TOUCH_FAVORITES_MAX * TOUCH_FAVORITE_KEY_BYTES
 *  bytes (96). Pair with touchPrefsFavoritesSnapshotContains() for the
 *  membership check. */
int  touchPrefsCopyFavorites(uint8_t* out_buf);
bool touchPrefsFavoritesSnapshotContains(const uint8_t* snapshot, int count,
                                          const uint8_t* pub_key6);

/** Ignored / blocked senders. Same 6-byte-prefix scheme as favorites. Incoming
 *  messages from a stored prefix are dropped (no chat entry, no notification).
 *  Managed from the chat "Blocked users" sheet; long-press a message to block.
 *  • touchPrefsIsIgnored: true if the 6-byte prefix is stored.
 *  • touchPrefsSetIgnored: add/remove; returns the new ignored state.
 *  • touchPrefsCopyIgnored: copy all stored prefixes (for the manager UI). */
constexpr int TOUCH_IGNORED_MAX = 32;
constexpr int TOUCH_IGNORE_KEY_BYTES = 6;
bool touchPrefsIsIgnored(const uint8_t* pub_key6);
bool touchPrefsSetIgnored(const uint8_t* pub_key6, bool ignored);
int  touchPrefsCopyIgnored(uint8_t* out_buf);

/** Ignored / blocked sender NAMES — for channel/room senders who are NOT saved
 *  contacts, so the 6-byte pubkey scheme above can't target them (e.g. a bot
 *  posting inside a joined room: posts carry only a display name, not a pubkey).
 *  Incoming channel/room messages whose parsed sender name is stored here are
 *  dropped (no bubble, no notification, no sound). Case-sensitive exact match.
 *  • touchPrefsIsNameIgnored / touchPrefsSetNameIgnored: membership + add/remove.
 *  • touchPrefsCopyIgnoredNames: copy every stored name into `out_buf`
 *    (>= TOUCH_IGNORED_NAMES_MAX * TOUCH_IGNORED_NAME_LEN bytes); returns count. */
constexpr int TOUCH_IGNORED_NAMES_MAX = 16;
constexpr int TOUCH_IGNORED_NAME_LEN  = 28;   // fixed NUL-padded slot (>= MAX_SENDER_NAME+1)
bool touchPrefsIsNameIgnored(const char* name);
bool touchPrefsSetNameIgnored(const char* name, bool ignored);
int  touchPrefsCopyIgnoredNames(char* out_buf);

/** Notification-sound prefs (the message chime itself stays on the core
 *  buzzer_quiet flag). @-mention chime is a SEPARATE enable + a distinct sound,
 *  so an operator can mute everything but still hear @-mentions. Volume is the
 *  T-Deck I2S amplitude (0..100); the V4 piezo can't vary volume. */
bool    touchPrefsGetSoundMessages();          // message chime on/off (under the master), default true
void    touchPrefsSetSoundMessages(bool on);
bool    touchPrefsGetDiscoveredAutoEvict();    // ring full -> auto-delete the oldest discovered node (default true)
void    touchPrefsSetDiscoveredAutoEvict(bool on);
uint8_t touchPrefsGetDiscoveredMaxHops();      // auto-delete discovered nodes heard via more hops than this (0 = off)
void    touchPrefsSetDiscoveredMaxHops(uint8_t hops);
bool    touchPrefsGetSoundMentions();          // default true
void    touchPrefsSetSoundMentions(bool on);
bool    touchPrefsGetSoundDirect();            // direct/DM chime on/off, default true
void    touchPrefsSetSoundDirect(bool on);
uint8_t touchPrefsGetSoundVolume();            // 0..100, default 70
void    touchPrefsSetSoundVolume(uint8_t vol);

/** Per-event notification sound FILE (empty = built-in chime). Slot:
 *  0 = message, 1 = direct/DM, 2 = @-mention. Stored as a path pref like the
 *  lock wallpaper ("" | "/sounds/x.wav" | "sd:/x.wav"). T-Deck only. */
enum { TOUCH_SND_MSG = 0, TOUCH_SND_DM = 1, TOUCH_SND_MEN = 2 };
constexpr int TOUCH_SOUND_PATH_MAXLEN = 128;
int  touchPrefsGetSoundFile(int slot, char* out, int out_cap);
bool touchPrefsSetSoundFile(int slot, const char* path);

/** Tanmatsu keyboard backlight brightness, 0–100 %. Default 100. Separate from
 *  screen brightness (touchPrefsGet/SetBrightness) and volume (…SoundVolume). */
uint8_t touchPrefsGetKbdBacklight();
void    touchPrefsSetKbdBacklight(uint8_t pct);
bool    touchPrefsGetEnterSends();             // Enter key sends a chat message (default true)
void    touchPrefsSetEnterSends(bool on);
bool    touchPrefsGetClock12h();               // 12-hour clock (default false = 24h)
void    touchPrefsSetClock12h(bool on);
bool    touchPrefsGetNavMenubarKeys();         // show per-tab nav hotkey letters over the menubar (default false = hidden)
void    touchPrefsSetNavMenubarKeys(bool on);
bool    touchPrefsGetScrollReverse();          // invert trackball/scrollball direction (default false)
void    touchPrefsSetScrollReverse(bool on);
bool    touchPrefsGetEdgeScroll();             // push cursor past edge to scroll content (default false)
void    touchPrefsSetEdgeScroll(bool on);
bool    touchPrefsGetLockOnScreenOff();        // idle screen-off auto-locks; only a deliberate hold wakes (default false)
void    touchPrefsSetLockOnScreenOff(bool on);

/** Per-channel mute, keyed by channel name. Bit 0 = mute messages, bit 1 =
 *  mute @-mentions. Suppresses the notification SOUND for that channel (the
 *  unread badge/divider still work). A small name-keyed NVS blob. */
constexpr int TOUCH_CHMUTE_MAX  = 24;
constexpr int TOUCH_CHMUTE_NAME = 32;
constexpr uint8_t TOUCH_CHMUTE_MSG = 0x1;
constexpr uint8_t TOUCH_CHMUTE_MEN = 0x2;
uint8_t touchPrefsGetChannelMute(const char* name);
void    touchPrefsSetChannelMute(const char* name, uint8_t flags);
bool    touchPrefsGetChannelEmoji(const char* name, char* out, size_t cap);  // chat-list avatar override; false/empty = auto letters
void    touchPrefsSetChannelEmoji(const char* name, const char* utf8);       // empty/null clears back to auto

/** Generic NVS blob (key/value). Used to persist the discovered-nodes list so
 *  it survives a reboot. getBlob returns the byte count copied (0 if absent). */
size_t touchPrefsGetBlob(const char* key, uint8_t* out, size_t maxlen);
bool   touchPrefsSetBlob(const char* key, const uint8_t* data, size_t len);

/** Map tile-server base URL. The device fetches missing map tiles by
 *  HTTP GET against `<base>/<z>/<x>/<y>.png`. Defaults to the meshcomod
 *  proxy. Plain HTTP only — mbedTLS doesn't fit in the ~5 KB of internal
 *  heap that survives Wi-Fi association, so the proxy does the HTTPS
 *  upstream to OSM. The default base is reasonable; expose it in
 *  Settings so the user can point at their own proxy / self-hosted
 *  tile-server if they prefer. */
constexpr int TOUCH_TILE_SERVER_MAXLEN = 80;
int  touchPrefsGetTileServer(char* out, int out_cap);
bool touchPrefsSetTileServer(const char* url);

/** Region scope (display name only). The actual flood-scope key lives in the
 *  mesh NodePrefs (default_scope_key, derived via MyMesh::setDefaultFloodScope);
 *  this just remembers the human-readable "#region" the user typed so the radio
 *  settings field can show it back. Empty = unscoped. */
constexpr int TOUCH_REGION_SCOPE_MAXLEN = 40;
int  touchPrefsGetRegionScope(char* out, int out_cap);
bool touchPrefsSetRegionScope(const char* name);

/** Per-channel region-scope override (overrides the default flood scope for that
 *  channel's outgoing messages). Keyed by channel slot; blank = inherit default. */
int  touchPrefsGetChannelScope(int slot, char* out, int out_cap);
bool touchPrefsSetChannelScope(int slot, const char* name);

/** Remembered repeater admin passwords. Keyed by the first 6 bytes of the
 *  repeater's pubkey, value is the null-terminated password (max 15 chars
 *  to match what sendLogin truncates to). Stored as a single NVS blob of
 *  up to 16 fixed-size records; an empty/cleared entry is removed from the
 *  blob. Pure UI convenience — the firmware doesn't look at this; only
 *  the touch admin login modal reads/writes it. */
constexpr int TOUCH_REPEATER_PW_MAX     = 16;
constexpr int TOUCH_REPEATER_PW_KEY_LEN = 6;
constexpr int TOUCH_REPEATER_PW_LEN     = 16;   // 15 chars + null
int  touchPrefsGetRepeaterPassword(const uint8_t* pub_key6, char* out, int out_cap);
bool touchPrefsSetRepeaterPassword(const uint8_t* pub_key6, const char* password);

/** Lock-screen wallpaper. Either an internal SPIFFS path (e.g.
 *  "/lock/placeholder.jpg") or an SD-card path prefixed with "sd:" (e.g.
 *  "sd:/walls/x.jpg"). The lockscreen falls back to the embedded placeholder
 *  if the file is missing or won't decode. Default = the placeholder path. */
constexpr int TOUCH_LOCK_WALLPAPER_MAXLEN = 128;
int  touchPrefsGetLockWallpaper(char* out, int out_cap);
bool touchPrefsSetLockWallpaper(const char* path);

/** Lock-screen text colour (clock + labels) as 0xRRGGBB. Default soft white. */
uint32_t touchPrefsGetLockTextColor();
bool     touchPrefsSetLockTextColor(uint32_t rgb);

/** Colourful chat bubbles: colour every bubble + sender name by a hash of the
 *  sender's display name, so the same name always gets the same colour. Off by
 *  default. */
bool touchPrefsGetColorfulBubbles();
bool touchPrefsSetColorfulBubbles(bool on);

/** Tanmatsu message-notification LED: flash the envelope-icon LED on a new message and breathe it
 *  softly while there are unread messages. Default ON. Tanmatsu-only (no such LED on T-Deck/V4). */
#if defined(HAS_TANMATSU)
bool touchPrefsGetMsgLed();
void touchPrefsSetMsgLed(bool on);
#endif

/** UI accent colour (buttons, active tab, keyboard, highlights) as 0xRRGGBB.
 *  Default = the stock neutral gray; the picker keeps it dark enough that the
 *  off-white text stays readable. */
uint32_t touchPrefsGetAccentColor();
bool     touchPrefsSetAccentColor(uint32_t rgb);

/** Manual clock correction in whole hours, applied on top of the automatic
 *  (NTP / companion / mesh) time when rendering local time. Range -23..+23,
 *  default 0. Display-only — the RTC and mesh timestamps stay UTC. */
int  touchPrefsGetTimeOffsetHours();
bool touchPrefsSetTimeOffsetHours(int hours);

/** Time-zone picker. A curated list of named zones, each carrying the correct
 *  POSIX TZ string WITH that region's DST rules — so non-European users get the
 *  right time year-round instead of being stuck on the CET base + EU DST dates.
 *  Default 0 == "Europe (CET/CEST)" (unchanged behaviour). The LAST index is
 *  "Custom (UTC offset)", which uses touchPrefsGetTimeOffsetHours() as a fixed,
 *  no-DST offset. touchPrefsBuildLocalTz() resolves the selected zone. */
int         touchPrefsTimezoneCount();           // number of pickable entries
const char* touchPrefsTimezoneLabel(int idx);    // display label for the picker
uint8_t     touchPrefsGetTimezone();             // selected index (default 0)
void        touchPrefsSetTimezone(uint8_t idx);

/** Build the POSIX TZ string for local-time display from the selected time zone
 *  (touchPrefsGetTimezone), or a fixed UTC offset for the "Custom" entry. Used by
 *  both the boot-time setenv and the Wi-Fi NTP sync so it's honoured everywhere.
 *  `out` is always null-terminated when out_cap > 0. */
void touchPrefsBuildLocalTz(char* out, int out_cap);

/** First-boot setup wizard completion flag. false until the user finishes (or
 *  skips) the on-device setup flow (welcome → name → region → Wi-Fi); true
 *  thereafter so the wizard never reappears on subsequent boots. */
bool touchPrefsGetSetupDone();
bool touchPrefsSetSetupDone(bool done);

/** GPS serial baud override. The T-Deck Plus GPS runs at 38400; the older
 *  T-Deck v1.0 needs 9600. The build hard-codes GPS_BAUD_RATE, so this NVS
 *  override lets the user match their hardware without a rebuild. Read at GPS
 *  init; `fallback` is the compile-time default. Applied on reboot. */
uint32_t touchPrefsGetGpsBaud(uint32_t fallback);
bool     touchPrefsSetGpsBaud(uint32_t baud);

/** Mesh signal auto-discover probe. When ON (default), the firmware periodically
 *  sends a flood advert so nearby repeaters re-broadcast it and we can measure
 *  the echo's SNR — what drives the top-bar signal bars and the home-graph
 *  readout. The poll interval is in whole minutes, clamped 1..1440 (1 min =
 *  60 s minimum, no higher than a day). Both are exposed in the home-graph
 *  "Signal & traffic" popup (tap the graph). */
bool     touchPrefsGetSigProbeEnabled();
bool     touchPrefsSetSigProbeEnabled(bool on);
uint16_t touchPrefsGetSigPollMins();
bool     touchPrefsSetSigPollMins(uint16_t mins);

#endif
