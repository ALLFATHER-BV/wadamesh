#include "TouchPrefsStore.h"
#include "TouchPrefsSchema.h"

#if defined(ESP32)

#include "WifiRuntimeStore.h"

#include "SdNvsPrefs.h"   // NVS, or SD /meshcomod fallback when NVS is unusable (Launcher)

#include <Preferences.h>
#include <SPIFFS.h>
#include <stddef.h>   // offsetof
#include <string.h>   // memcpy

static const char* TOUCH_NS = "touch";

static SdNvsPrefs s_prefs;
static bool s_begun = false;

// ---------------------------------------------------------------------------
// Packed scalar config blob ("cfg")
// ---------------------------------------------------------------------------
//
// NVS is a tiny (~20 KB) partition shared across firmwares; every distinct key
// costs an entry and the namespace was filling up. The many per-key SCALAR
// settings (brightness, kb backlight, accent colour, language, …) are therefore
// packed into ONE versioned blob keyed "cfg". Strings (tile_srv, rgn_scope,
// lk_wall, channel-scopes, quick-replies), the byte blobs (fav / ign / rpw) and
// the Wi-Fi slots keep their own keys — and "use_sd" / "setup_ok" stay standalone
// keys too. "use_sd" is mirrored to NVS on every UI toggle (main.cpp reads it at
// boot via touchPrefsReadUseSdAtBoot); "setup_ok" is still NVS-only.
//
// On first run with the blob absent we read every legacy per-key into s_cfg, add
// "cfg", and remove the superseded file keys in the same queued RAM snapshot.
// The released .kv remains the fallback until that complete A/B snapshot verifies,
// making the migration crash-safe and idempotent. `magic` rejects a garbage /
// short read (→ treat as absent → defaults); `ver` lets later builds add fields.
static const char* KEY_CFG = "cfg";
static const uint16_t TOUCH_CFG_MAGIC = TouchPrefsSchema::MAGIC;
static const uint8_t  TOUCH_CFG_VER   = TouchPrefsSchema::CURRENT_VERSION;  // v2 sig_probe/poll; v3 tz_zone; v4 hide_node_name; v5 map_night/map_zoom; v6 map text/marker visibility; v7 app_grid_large; v8 ui_scale; v9 tb_keypad; v10 sleep_idle; v11 nav_keys; v12 map_zoom_buttons; v13 nav_dir_keys; v14 home_is_drawer; v15 kbd_nav default ON (one-time migrate); v16 nav_scroll_keys; v17 notify_new_contact; v18 kbd_nav OFF by default (reverses v15; T-Deck/V4 only, Tanmatsu stays on); v19 show_sensors_tab; v20 map_show_links; v21 map_style (0=OSM default, 1=OpenTopoMap); v22 tb_nav; v23 scope_direct (opt-in: scope direct/login floods to the region); v24 tb_nav default OFF (experimental); v25 fem_lna (Heltec V4.3 high-gain FEM LNA, opt-in); v26 msg_flash (flash keyboard backlight + wake screen on a new message, opt-in); v27 flood_adv_hrs + local_adv_min (periodic self-advert intervals, the standard MeshCore flood/local advert on a timer); v28 beta_updates (opt-in to test/beta firmware on the OTA update check + install); v29 ui_scale default -> Large/150% (Tanmatsu; bumps the old 100% default, leaves an explicit Large/Huge choice); v30 boot_advert (opt-in one-shot flood self-advert ~6s after boot, all boards, #76); v31 compact_chat (opt-in IRC-style dense chat rows instead of bubbles); v32 clock_floor (highest epoch handed out — monotonic send-timestamp floor across reboots, #89); v33 rx_queue (buffered LoRa receive: drain task + packet ring, experimental, default OFF); v34 web_mirror (web control panel: mirror the live UI to a phone browser + inject taps, opt-in, default OFF); v35 remote_mode (render the UI off-screen at a web resolution instead of the panel; boot mode, default OFF); v36 remote_landscape (remote mode orientation: landscape 800x480 vs portrait 480x800); v37 remote_landscape now defaults ON (remote mode = landscape/desktop by default; one-time flip of existing installs, portrait stays a toggle); v38 web_terminal (web mesh CLI terminal served on the device IP; runtime toggle, mutually exclusive with VNC, default OFF); v40 hist_sync_after (chat-history flush: consecutive off-thread write failures before the blocking loop-task fallback, 0 = never); v41 p4_antenna (T-Display P4 antenna select; now RESERVED/unused - the choice is session-only so every boot comes up on the on-board antenna); v42 hist_per_chat (max stored messages PER chat, default 250 - a busy public channel used to be able to fill the whole shared ring and drag the UI down); v43 Pager UI-size presets (reset the previously ignored large-screen default to Small once); v44 broken retry_echo mid-struct insertion; v45 moves retry_echo to the actual tail and resets the ambiguous v44 suffix; v46 app_hide; v47 MQTT hidden by default; v48 lang_file; v49 fem_lna default ON on the V4-R8 (KCT8103L FEM; one-time flip of existing installs, no new field); v50 map_show_tilexyz default OFF (tile z/x/y line hidden; one-time flip, no new field)

// Defaults (kept identical to the historical per-key defaults).
static const uint16_t DEFAULT_SCREEN_TIMEOUT_S = 20;
static const uint8_t  DEFAULT_BRIGHTNESS       = 100;
static const uint8_t  DEFAULT_KB_BL            = 2;          // auto
static const uint8_t  DEFAULT_KB_LAYOUT        = 0;          // English
static const uint8_t  DEFAULT_KB_SECONDARY     = 0;          // None
static const uint32_t DEFAULT_LOCK_COLOR       = 0xE6F2FFu;  // soft white
static const uint32_t DEFAULT_ACCENT           = 0x15B6A6u;  // brand teal
static const bool     DEFAULT_DC_SHOW          = true;
static const uint8_t  DEFAULT_SIG_PROBE_EN     = 1;          // signal discover probe ON
static const uint16_t DEFAULT_SIG_POLL_MIN     = 5;          // minutes between probes

using TouchCfg = TouchPrefsSchema::Config;

static TouchCfg s_cfg;
static bool     s_cfg_loaded = false;

// Legacy per-key names — only referenced by the one-time migration below.
static const char* LK_SCR_TO       = "scr_to_s";
static const char* LK_DC_SHOW      = "dc_show";
static const char* LK_BRIGHTNESS   = "bright";
static const char* LK_KB_BL        = "kb_bl";
static const char* LK_KB_LAYOUT    = "kblang";
static const char* LK_KB_SECONDARY = "kbsec";
static const char* LK_KB_ENABLED   = "kbenab";
static const char* LK_LOCK_COLOR   = "lk_col";
static const char* LK_CLR_BUBBLES  = "clr_bub";
static const char* LK_KB_ACCENT    = "kb_accent";
static const char* LK_ACCENT       = "accent";
static const char* LK_TIME_OFFS    = "time_offs";
static const char* LK_USE_MILES    = "use_miles";
static const char* LK_TILES_FROM_SD= "tiles_sd";
static const char* LK_UI_LANG      = "ui_lang";
static const char* LK_UI_ROTATION  = "uirot";
static const char* LK_BATT_FULL    = "battfull";
static const char* LK_GPS_BAUD     = "gps_baud";

static void cfgSetDefaults(TouchCfg& c) {
  c.magic         = TOUCH_CFG_MAGIC;
  c.ver           = TOUCH_CFG_VER;
  c.bright        = DEFAULT_BRIGHTNESS;
  c.kb_bl         = DEFAULT_KB_BL;
  c.kb_layout     = DEFAULT_KB_LAYOUT;
  c.kb_secondary  = DEFAULT_KB_SECONDARY;
  c.ui_lang       = 0;
  c.ui_rotation   = 0;
  c.dc_show       = DEFAULT_DC_SHOW ? 1 : 0;
  c.use_miles     = 0;
  c.tiles_from_sd = 0;
  c.clr_bubbles   = 1;          // default ON
  c.kb_accent     = 1;          // default ON
  c.time_offs     = 0;
  c.scr_to_s      = DEFAULT_SCREEN_TIMEOUT_S;
  c.kb_enabled    = 0;
  c.batt_full_mv  = 0;
  c.lock_color    = DEFAULT_LOCK_COLOR;
  c.accent        = DEFAULT_ACCENT;
  c.gps_baud      = 0;          // 0 sentinel -> getter returns caller fallback
  c.hist_per_chat = 250;        // keep the newest 250 per chat: enough to scroll back, small enough to stay quick
  c.p4_antenna    = 0;          // reserved, unused: the P4 antenna choice is never persisted
  c.sig_probe_en  = DEFAULT_SIG_PROBE_EN;
  c.sig_poll_min  = DEFAULT_SIG_POLL_MIN;
  c.tz_zone       = 0;          // 0 = Europe (CET/CEST) — preserves prior behaviour
  c.hide_node_name = 0;         // default: show the device name
  c.map_night     = 0;          // default: normal (light) tiles
  c.map_zoom      = 0;          // 0 = unset -> auto-snap on first map open
  c.map_show_coords   = 1;      // default: show coords / tile line / contacts
  c.map_show_tilexyz  = 0;      // v50: the "z12  12/2105/1376" tile-path line is developer clutter on the map; opt-in via Map options
  c.map_show_contacts = 1;
  c.app_grid_large    = 0;      // default: compact app grid (T-Deck 4 cols / V4 3 cols)
#if defined(TLORA_PAGER)
  c.ui_scale          = 0;      // Pager: Small/current typography by default
#else
  c.ui_scale          = 1;      // large-screen boards keep their existing 150% default
#endif
#if defined(HAS_TANMATSU)
  c.kbd_nav           = 1;      // Tanmatsu: no touchscreen — keyboard nav is the only input, always on
#else
  c.kbd_nav           = 0;      // T-Deck / V4: keyboard navigation OFF by default (opt-in; persists once toggled on)
#endif
  c.tb_nav            = 0;      // T-Deck trackball: soft-cursor by default. D-pad UI nav is EXPERIMENTAL (opt-in)
  c.scope_direct      = 0;      // OFF: direct/login floods stay unscoped (cross-region safe). Opt-in per issue #64.
#if defined(HELTEC_LORA_V4_R8)
  c.fem_lna           = 1;      // ON (v49): the V4-R8 is a V4.3.1-generation board with the KCT8103L FEM, whose
                                // switchable ~17 dB LNA is the whole point of that FEM revision — shipping it
                                // bypassed left RX sensitivity on the table. Toggle stays in Radio & Mesh.
#else
  c.fem_lna           = 0;      // OFF: V4.3 FEM LNA bypassed (matches the hardware default). Opt-in high-gain RX.
#endif
  c.msg_flash         = 0;      // OFF: opt-in new-message keyboard/screen flash
  c.flood_adv_hrs     = 0;      // OFF: no periodic flood self-advert (advertise manually)
  c.local_adv_min     = 0;      // OFF: no periodic zero-hop self-advert
  c.beta_updates      = 0;      // OFF: stable update channel (opt-in to beta/test firmware)
  c.boot_advert       = 0;      // OFF: no automatic advert on boot — opt-in (#76)
  c.console_mode      = 0;      // OFF: boot into the graphical UI (CONSOLE_MODE.md)
  c.console_monitor   = 1;      // ON: the console shows messages as they arrive
  c.kb_force_legacy   = 0;      // OFF: detect the keyboard protocol automatically
  c.compact_chat      = 0;      // OFF: bubble chat layout (opt-in IRC-style dense rows)
  c.clock_floor       = 0;      // no persisted send-timestamp floor yet
  c.rx_queue          = 1;      // ON: buffered receive (test-channel default; opt-out toggle in Radio & Mesh)
  c.retry_echo        = 0;      // OFF: auto-retry is opt-in (toggle in Radio & Mesh)
  c.app_hide          = (1u << 12);  // APPHIDE_MQTT: the MQTT bridge starts hidden (experimental + privacy)
  memset(c.lang_file, 0, sizeof c.lang_file);   // no file language: built-in ui_lang column
  c.sleep_idle        = 0;      // default: idle light-sleep OFF
  { const char* d = "ertui"; for (int i = 0; i < 5; i++) c.nav_keys[i] = (uint8_t)d[i]; }  // default tab hotkeys E/R/T/U/I
  c.map_zoom_buttons  = 0;      // default: map zoom = slider
#if defined(HAS_TANMATSU)
  { const char* d = "wxads"; for (int i = 0; i < 6; i++) c.nav_dir_keys[i] = (uint8_t)d[i]; }  // Tanmatsu: W up/X down/A left/D right/S select; no Back letter (Esc/F-key), d[5]='\0'
#else
  { const char* d = "wzadsq"; for (int i = 0; i < 6; i++) c.nav_dir_keys[i] = (uint8_t)d[i]; }  // default W/Z/A/D/S/Q
#endif
  c.home_is_drawer    = 0;      // default: Home = Commander screen
#if defined(HAS_TANMATSU)
  c.nav_scroll_keys[0] = 'f';  c.nav_scroll_keys[1] = 'v';   // Tanmatsu scroll-up F / scroll-down V
#else
  c.nav_scroll_keys[0] = 'f';  c.nav_scroll_keys[1] = 'c';   // default scroll-up F / scroll-down C
#endif
  c.notify_new_contact = 1;     // default: show the new-contact toast (preserve prior behaviour)
  c.show_sensors_tab   = 1;     // default: show the V4 Expansion-Kit Sensors tab + Home env widget
  c.map_show_links     = 1;     // default: show self->contact link lines (PR #61)
  c.map_style          = 0;     // default: OpenStreetMap (OpenTopoMap is opt-in)
  c.web_mirror         = 0;     // OFF: web control panel is opt-in (remote control over the LAN)
  c.remote_mode        = 0;     // OFF: render to the physical panel (remote mode is opt-in, reboots to apply)
  c.remote_landscape   = 1;     // landscape 800x480 by default (remote mode = desktop/browser); portrait is a toggle
  c.web_terminal       = 0;     // OFF: web mesh terminal is opt-in (runtime; mutually exclusive with VNC)
  c.map_tile_debug     = 0;     // OFF: map tile-pipeline diagnostic overlay is opt-in (developer)
  c.hist_sync_after    = 2;     // chat flush: 2 failed background writes -> synchronous loop-task fallback
}

// Update the whole blob using the same end()/begin(RW)/put/end()/begin(RO)
// discipline every setter in this file uses. File mode queues a coalesced write.
static bool cfgFlush() {
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) { s_begun = false; return false; }
  bool ok = s_prefs.putBytes(KEY_CFG, &s_cfg, sizeof(s_cfg)) == sizeof(s_cfg);
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

// Load "cfg" once; on absence run the one-time legacy migration. Must be called
// with the namespace already open (RO is fine for the load + legacy reads; the
// migration write reopens RW via cfgFlush). Idempotent: a no-op after the first
// successful run because "cfg" then exists.
static void cfgLoadOrMigrate() {
  if (s_cfg_loaded) return;
  cfgSetDefaults(s_cfg);

  // isKey() does NOT emit the [E] NOT_FOUND log that getBytes() would on a miss,
  // so probe first to keep the (USB-CDC) console clean on a fresh device.
  if (s_prefs.isKey(KEY_CFG)) {
    uint8_t blob[sizeof(TouchCfg)] = {};
    size_t n = s_prefs.getBytes(KEY_CFG, blob, sizeof(blob));
    uint8_t stored_version = 0;
    // Need at least magic(2)+ver(1) to trust the header; reject anything shorter
    // (a half-written / garbage blob) and re-derive from legacy keys / defaults.
    if (TouchPrefsSchema::overlayStored(s_cfg, blob, n, &stored_version)) {
      // Older blobs overlay their established prefix on the defaults. Broken
      // beta-57 v44 blobs stop at rx_queue: their suffix is byte-shifted and
      // ambiguous, so web/remote/history tuning intentionally returns to safe
      // defaults instead of guessing and silently enabling another boot mode.
      if (stored_version == TouchPrefsSchema::BROKEN_MID_INSERT_VERSION)
        Serial.println("[PREFS] repairing beta_57 v44 suffix with safe defaults");
      if (stored_version < TOUCH_CFG_VER) {
        // v2->v3: a manual hour offset used to mean "CET base + offset". Preserve
        // that under the new zone picker by mapping such users onto the Custom
        // (UTC-offset) zone, so their clock doesn't jump to CET. 0xFE is resolved
        // to the real Custom index on the first touchPrefsGetTimezone() call (the
        // zone count isn't known here). offset 0 stays zone 0 (Europe) = unchanged.
        if (stored_version < 3 && s_cfg.time_offs != 0) s_cfg.tz_zone = 0xFE;
        // v18: keyboard navigation is now OFF by default (it was force-enabled at v15, but it
        // complicated the touch UX more than it helped). Reset existing installs to off ONCE so
        // they match the new default; the user's later explicit on/off then persists (fires only
        // for ver < 18, never again). The Tanmatsu is exempt — it has no touchscreen, so keyboard
        // nav is its only input and must stay on.
#if !defined(HAS_TANMATSU)
        if (stored_version < 18) s_cfg.kbd_nav = 0;
#endif
        // v22: new trailing field — default the T-Deck trackball to D-pad UI nav on existing installs.
        if (stored_version < 22) s_cfg.tb_nav = 1;
        // v23: new trailing field — scope-direct-floods OFF on existing installs (opt-in).
        if (stored_version < 23) s_cfg.scope_direct = 0;
        // v24: trackball D-pad UI nav demoted to EXPERIMENTAL — default OFF (was on at v22). Flip
        // existing installs back to the soft cursor; the toggle lets users opt back in.
        if (stored_version < 24) s_cfg.tb_nav = 0;
        // v25: new trailing field — V4.3 FEM LNA OFF on existing installs (matches hardware default).
        if (stored_version < 25) s_cfg.fem_lna = 0;
#if defined(HELTEC_LORA_V4_R8)
        // v49: FEM LNA ON by default on the V4-R8 (KCT8103L). One-time flip of existing
        // installs so they match the new default; an explicit later off/on persists.
        if (stored_version < 49) s_cfg.fem_lna = 1;
#endif
        // v50: map tile z/x/y overlay line OFF by default (one-time flip; the Map-options toggle persists afterwards).
        if (stored_version < 50) s_cfg.map_show_tilexyz = 0;
        if (stored_version < 26) s_cfg.msg_flash = 0;
        if (stored_version < 27) { s_cfg.flood_adv_hrs = 0; s_cfg.local_adv_min = 0; }
        if (stored_version < 28) s_cfg.beta_updates = 0;
        if (stored_version < 29 && s_cfg.ui_scale == 0) s_cfg.ui_scale = 1;   // bump old 100% default -> Large (150%)
        if (stored_version < 30) s_cfg.boot_advert = 0;   // #76 new trailing field: advert-on-boot off by default
        // v51 new trailing field. Anything older than 51 never stored it, so it
        // must be forced OFF rather than inherited from whatever byte was there:
        // a garbage 1 would boot a user into a console they did not ask for.
        if (stored_version < 51) s_cfg.console_mode = 0;
        if (stored_version < 52) s_cfg.console_monitor = 1;   // new trailing field: on by default
        if (stored_version < 53) s_cfg.kb_force_legacy = 0;   // new trailing field: auto-detect
        if (stored_version < 31) s_cfg.compact_chat = 0;  // new trailing field: compact chat rows off by default
        if (stored_version < 32) s_cfg.clock_floor = 0;   // new trailing field: no send-timestamp floor persisted yet (#89)
        if (stored_version < 33) s_cfg.rx_queue = 1;      // buffered LoRa receive ON for the test channel (opt-out toggle in Radio & Mesh)
        if (stored_version < 45) s_cfg.retry_echo = 0;    // v45: correctly appended auto-retry preference; opt-in per user feedback
        // v45 also lands Hungarian INSERTED at UiLang slot 1 (#227), which shifts every
        // stored non-English choice by one. Remap once; RO (old max 12) becomes 13.
        if (stored_version < 45 && s_cfg.ui_lang >= 1 && s_cfg.ui_lang <= 12) s_cfg.ui_lang += 1;
        if (stored_version < 46) s_cfg.app_hide = 0;             // v46 trailing field: nothing hidden
        if (stored_version < 47) s_cfg.app_hide |= (1u << 12);   // v47: MQTT bridge starts hidden (experimental + privacy)
        if (stored_version < 48) memset(s_cfg.lang_file, 0, sizeof s_cfg.lang_file);   // v48 trailing field: no file language
        if (stored_version < 34) s_cfg.web_mirror = 0;    // new trailing field: web control panel off by default (opt-in remote control)
        if (stored_version < 35) s_cfg.remote_mode = 0;   // new trailing field: remote mode off by default (opt-in, reboots to apply)
        if (stored_version < 36) s_cfg.remote_landscape = 0;
        if (stored_version < 37) s_cfg.remote_landscape = 1;   // remote mode = landscape/desktop by default (one-time flip; portrait stays a toggle)
        if (stored_version < 38) s_cfg.web_terminal = 0;       // new trailing field: web mesh terminal off by default (opt-in)
        if (stored_version < 39) s_cfg.map_tile_debug = 0;     // new trailing field: tile diagnostic overlay off by default
#if defined(TLORA_PAGER)
        // ui_scale existed before the Pager exposed the selector, so every old
        // Pager inherited the unrelated large-screen default (1) while ignoring
        // it. Reset it once so upgrading cannot enlarge the UI without consent.
        if (stored_version < 43) s_cfg.ui_scale = 0;
#endif
        s_cfg.ver = TOUCH_CFG_VER;
        s_cfg.magic = TOUCH_CFG_MAGIC;
        cfgFlush();                // rewrite with new fields defaulted-in
      }
      s_cfg_loaded = true;
      return;
    }
    // Garbage / short / wrong-magic read -> fall through and re-derive from
    // legacy keys (or defaults), overwriting the bad blob.
  }

  // No (valid) "cfg" yet: build it from the legacy per-key values, applying the
  // exact same defaults the old getters used. On a fresh device every isKey()
  // is false, so this just keeps the defaults set above.
  if (s_prefs.isKey(LK_SCR_TO))       s_cfg.scr_to_s     = s_prefs.getUShort(LK_SCR_TO, DEFAULT_SCREEN_TIMEOUT_S);
  if (s_prefs.isKey(LK_BRIGHTNESS))   s_cfg.bright       = s_prefs.getUChar(LK_BRIGHTNESS, DEFAULT_BRIGHTNESS);
  if (s_prefs.isKey(LK_KB_BL))        s_cfg.kb_bl        = s_prefs.getUChar(LK_KB_BL, DEFAULT_KB_BL);
  if (s_prefs.isKey(LK_KB_LAYOUT))    s_cfg.kb_layout    = s_prefs.getUChar(LK_KB_LAYOUT, DEFAULT_KB_LAYOUT);
  if (s_prefs.isKey(LK_KB_SECONDARY)) s_cfg.kb_secondary = s_prefs.getUChar(LK_KB_SECONDARY, DEFAULT_KB_SECONDARY);
  if (s_prefs.isKey(LK_TIME_OFFS))    s_cfg.time_offs    = s_prefs.getChar(LK_TIME_OFFS, 0);
  if (s_prefs.isKey(LK_LOCK_COLOR))   s_cfg.lock_color   = s_prefs.getUInt(LK_LOCK_COLOR, DEFAULT_LOCK_COLOR) & 0xFFFFFFu;
  if (s_prefs.isKey(LK_ACCENT))       s_cfg.accent       = s_prefs.getUInt(LK_ACCENT, DEFAULT_ACCENT) & 0xFFFFFFu;
  if (s_prefs.isKey(LK_CLR_BUBBLES))  s_cfg.clr_bubbles  = s_prefs.getBool(LK_CLR_BUBBLES, true) ? 1 : 0;
  if (s_prefs.isKey(LK_KB_ACCENT))    s_cfg.kb_accent    = s_prefs.getBool(LK_KB_ACCENT, true) ? 1 : 0;
  if (s_prefs.isKey(LK_DC_SHOW))      s_cfg.dc_show      = s_prefs.getBool(LK_DC_SHOW, DEFAULT_DC_SHOW) ? 1 : 0;
  if (s_prefs.isKey(LK_USE_MILES))    s_cfg.use_miles    = s_prefs.getBool(LK_USE_MILES, false) ? 1 : 0;
  if (s_prefs.isKey(LK_TILES_FROM_SD))s_cfg.tiles_from_sd= s_prefs.getBool(LK_TILES_FROM_SD, false) ? 1 : 0;
  if (s_prefs.isKey(LK_UI_LANG))      s_cfg.ui_lang      = s_prefs.getUChar(LK_UI_LANG, 0);
  if (s_prefs.isKey(LK_UI_ROTATION))  s_cfg.ui_rotation  = s_prefs.getUChar(LK_UI_ROTATION, 0);
  if (s_prefs.isKey(LK_BATT_FULL))    s_cfg.batt_full_mv = s_prefs.getUShort(LK_BATT_FULL, 0);
  if (s_prefs.isKey(LK_GPS_BAUD))     s_cfg.gps_baud     = s_prefs.getUInt(LK_GPS_BAUD, 0);
  // Enabled-layout mask: legacy used 0xFFFF as the "never written" sentinel and
  // derived a one-bit mask from the secondary layout. Reproduce that here.
  {
    uint16_t v = s_prefs.isKey(LK_KB_ENABLED) ? s_prefs.getUShort(LK_KB_ENABLED, 0xFFFF) : 0xFFFF;
    if (v == 0xFFFF) {
      uint8_t sec = s_cfg.kb_secondary;
      s_cfg.kb_enabled = (sec != 0 && sec < 16) ? (uint16_t)(1u << sec) : 0;
    } else {
      s_cfg.kb_enabled = v;
    }
  }

  // Add "cfg" and retire the old file keys in one RAM transaction. The A/B
  // worker commits the complete result; until then the released .kv is intact.
  if (cfgFlush()) {
    s_prefs.end();
    if (s_prefs.begin(TOUCH_NS, false)) {
      const char* legacy[] = {
        LK_SCR_TO, LK_DC_SHOW, LK_BRIGHTNESS, LK_KB_BL, LK_KB_LAYOUT,
        LK_KB_SECONDARY, LK_KB_ENABLED, LK_LOCK_COLOR, LK_CLR_BUBBLES,
        LK_KB_ACCENT, LK_ACCENT, LK_TIME_OFFS, LK_USE_MILES, LK_TILES_FROM_SD,
        LK_UI_LANG, LK_UI_ROTATION, LK_BATT_FULL, LK_GPS_BAUD,
      };
      for (const char* k : legacy) {
        if (s_prefs.isKey(k)) s_prefs.remove(k);
      }
      s_prefs.end();
    }
    s_begun = s_prefs.begin(TOUCH_NS, true);
  }
  s_cfg_loaded = true;
}

void touchPrefsBegin() {
  if (s_begun) {
    if (!s_cfg_loaded) cfgLoadOrMigrate();
    return;
  }
  s_begun = s_prefs.begin(TOUCH_NS, true);
  if (!s_begun) {
    /* Namespace may not exist yet — open RW once to create it, then reopen RO. */
    if (s_prefs.begin(TOUCH_NS, false)) {
      s_prefs.end();
      s_begun = s_prefs.begin(TOUCH_NS, true);
    }
  }
  if (s_begun) cfgLoadOrMigrate();
}

// Re-read settings from scratch. Used at boot AFTER SdNvsPrefs::useFile() flips
// the backend to files: an earlier pref read (the boot-wordmark rotation) had
// already loaded + cached the cfg blob from legacy NVS, so without this the
// file-saved values (theme accent, brightness, language, …) would be ignored
// until a later boot, and a theme change would appear to "revert" on restart.
void touchPrefsReload() {
  s_prefs.end();
  s_begun = false;
  s_cfg_loaded = false;
  touchPrefsBegin();
}

void touchPrefsTick(uint32_t now_ms) { SdNvsPrefs::tick(now_ms); }
bool touchPrefsFlush(uint32_t timeout_ms) { return SdNvsPrefs::flush(timeout_ms); }
bool touchPrefsIoBusy() { return SdNvsPrefs::busy(); }

// Arduino's Preferences::getString()/getBytes() emit an [E] nvs_get_* "NOT_FOUND"
// log every time a key is absent — which floods the (USB-CDC) console on a fresh
// device and on every empty Wi-Fi-slot read. isKey() (getType → raw nvs probes)
// does NOT log, so probe with it before reading an optional string key.
static String prefsGetStr(const char* key, const String& def) {
  if (!s_begun) touchPrefsBegin();
  return s_prefs.isKey(key) ? s_prefs.getString(key, def) : def;
}

uint16_t touchPrefsGetScreenTimeoutSecs() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.scr_to_s;
}

bool touchPrefsSetScreenTimeoutSecs(uint16_t seconds) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.scr_to_s = seconds;
  return cfgFlush();
}

// --- Mesh signal auto-discover probe (toggle + poll interval) ---------------
// The interval is entered in whole minutes; clamp 1 min .. 1 day so a bad or blank entry
// can't make the probe run hot or effectively never run.
//
// NOT A FLOOD. This comment used to describe the probe as a flood, which is where the
// "wadamesh spams the mesh every 5 minutes" worry came from (issue #80). It is a ZERO-HOP
// CTL_TYPE_NODE_DISCOVER_REQ (MyMesh::uiSendSignalProbe) — the same node-discovery packet
// the other MeshCore GUIs use. Repeaters answer it DIRECTLY and never re-broadcast it, so
// nothing propagates beyond our immediate neighbours; the fallback when no repeater path is
// known is sendAdvert(false), also zero-hop. The caller additionally SKIPS the probe
// whenever a direct neighbour was heard inside the poll window, so the busier the mesh, the
// less this transmits.
static const uint16_t SIG_POLL_MIN_MINS = 1;   // 1 min = 60 s (the old fixed cadence)
static const uint16_t SIG_POLL_MAX_MINS = 1440;

// --- T-Display P4 LoRa antenna select — deliberately NOT persisted -----------
// XL9535 IO1 drives the board's SKY13453 antenna switch (full reasoning in Xl9535.h). The
// getter/setter that used to live here are gone on purpose: the choice is session-only, so
// there is nothing to store. Every boot forces the on-board antenna, in two places — the park
// in Xl9535::powerOnSequence() and the re-assert in UITask::begin() — because the external
// MMCX may have no antenna fitted, and keying a PA into an open connector damages it. That
// safety property only holds if a power cycle cannot restore "external", which means the
// choice must never reach flash. p4_antenna stays as a reserved trailing byte: dropping it
// would rewind TOUCH_CFG_VER on devices already carrying a v41 blob, for no gain.

// --- Per-chat history cap -----------------------------------------------------
// A single busy channel could previously fill the entire shared message ring, which both
// starved every other chat of history and made the inbox slow (see the thread-history cache
// in UITask). This bounds each chat independently. 0 = no per-chat cap, i.e. the old
// behaviour, which the settings UI warns about rather than hiding.
uint16_t touchPrefsGetHistPerChat() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.hist_per_chat;
}
bool touchPrefsSetHistPerChat(uint16_t n) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.hist_per_chat = n;
  return cfgFlush();
}

bool touchPrefsGetSigProbeEnabled() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.sig_probe_en != 0;
}
bool touchPrefsSetSigProbeEnabled(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.sig_probe_en = on ? 1 : 0;
  return cfgFlush();
}
uint16_t touchPrefsGetSigPollMins() {
  if (!s_begun) touchPrefsBegin();
  uint16_t m = s_cfg.sig_poll_min;
  if (m < SIG_POLL_MIN_MINS) m = SIG_POLL_MIN_MINS;
  if (m > SIG_POLL_MAX_MINS) m = SIG_POLL_MAX_MINS;
  return m;
}
bool touchPrefsSetSigPollMins(uint16_t mins) {
  if (mins < SIG_POLL_MIN_MINS) mins = SIG_POLL_MIN_MINS;
  if (mins > SIG_POLL_MAX_MINS) mins = SIG_POLL_MAX_MINS;
  if (!s_begun) touchPrefsBegin();
  s_cfg.sig_poll_min = mins;
  return cfgFlush();
}

uint8_t touchPrefsGetBrightness() {
  if (!s_begun) touchPrefsBegin();
  uint8_t v = s_cfg.bright;
  if (v < 5)   v = 5;
  if (v > 100) v = 100;
  return v;
}

bool touchPrefsSetBrightness(uint8_t pct) {
  if (pct < 5)   pct = 5;
  if (pct > 100) pct = 100;
  if (!s_begun) touchPrefsBegin();
  s_cfg.bright = pct;
  return cfgFlush();
}

uint8_t touchPrefsGetKbBacklight() {
  if (!s_begun) touchPrefsBegin();
  uint8_t v = s_cfg.kb_bl;
  return v > 2 ? DEFAULT_KB_BL : v;
}

bool touchPrefsSetKbBacklight(uint8_t mode) {
  if (mode > 2) mode = DEFAULT_KB_BL;
  if (!s_begun) touchPrefsBegin();
  s_cfg.kb_bl = mode;
  return cfgFlush();
}

uint8_t touchPrefsGetKeyboardLayout() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.kb_layout;
}

bool touchPrefsSetKeyboardLayout(uint8_t layout) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.kb_layout = layout;
  return cfgFlush();
}

uint8_t touchPrefsGetSecondaryKeyboard() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.kb_secondary;
}

bool touchPrefsSetSecondaryKeyboard(uint8_t secondary) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.kb_secondary = secondary;
  return cfgFlush();
}

uint16_t touchPrefsGetEnabledLayouts() {
  if (!s_begun) touchPrefsBegin();
  // The legacy "never written -> derive a one-bit mask from the secondary
  // layout" migration ran once at cfg-migration time (see cfgLoadOrMigrate);
  // the resolved mask now lives in s_cfg.kb_enabled.
  return s_cfg.kb_enabled;
}

bool touchPrefsSetEnabledLayouts(uint16_t mask) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.kb_enabled = mask;
  return cfgFlush();
}

static const char* KEY_TILE_SRV = "tile_srv";
static const char* DEFAULT_TILE_SERVER = "http://tiles.wadamesh.com";

int touchPrefsGetTileServer(char* out, int out_cap) {
  if (!out || out_cap <= 0) return 0;
  out[0] = '\0';
  if (!s_begun) touchPrefsBegin();
  String v = prefsGetStr(KEY_TILE_SRV, String(DEFAULT_TILE_SERVER));
  int n = (int)v.length();
  if (n > out_cap - 1) n = out_cap - 1;
  if (n > TOUCH_TILE_SERVER_MAXLEN - 1) n = TOUCH_TILE_SERVER_MAXLEN - 1;
  memcpy(out, v.c_str(), (size_t)n);
  out[n] = '\0';
  return n;
}

bool touchPrefsSetTileServer(const char* url) {
  if (!url) return false;
  if (!s_begun) touchPrefsBegin();
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  bool ok = s_prefs.putString(KEY_TILE_SRV, url) > 0;
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

static const char* KEY_RGN_SCOPE = "rgn_scope";

int touchPrefsGetRegionScope(char* out, int out_cap) {
  if (!out || out_cap <= 0) return 0;
  out[0] = '\0';
  if (!s_begun) touchPrefsBegin();
  String v = prefsGetStr(KEY_RGN_SCOPE, String(""));
  int n = (int)v.length();
  if (n > out_cap - 1) n = out_cap - 1;
  if (n > TOUCH_REGION_SCOPE_MAXLEN - 1) n = TOUCH_REGION_SCOPE_MAXLEN - 1;
  memcpy(out, v.c_str(), (size_t)n);
  out[n] = '\0';
  return n;
}

bool touchPrefsSetRegionScope(const char* name) {
  if (!name) return false;
  if (!s_begun) touchPrefsBegin();
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  bool ok = s_prefs.putString(KEY_RGN_SCOPE, name) > 0;
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

// Per-channel region-scope override, keyed by channel slot (0..63). Overrides the
// default flood scope for that channel's outgoing messages. Blank = inherit the
// default. Stored as "csc<slot>" -> region name.
static void chanScopeKey(int slot, char out[8]) {
  snprintf(out, 8, "csc%d", slot & 0x3F);
}
int touchPrefsGetChannelScope(int slot, char* out, int out_cap) {
  if (!out || out_cap <= 0) return 0;
  out[0] = '\0';
  if (slot < 0) return 0;
  if (!s_begun) touchPrefsBegin();
  char k[8]; chanScopeKey(slot, k);
  String v = prefsGetStr(k, String(""));
  int n = (int)v.length();
  if (n > out_cap - 1) n = out_cap - 1;
  if (n > TOUCH_REGION_SCOPE_MAXLEN - 1) n = TOUCH_REGION_SCOPE_MAXLEN - 1;
  if (n > 0) memcpy(out, v.c_str(), (size_t)n);
  out[n] = '\0';
  return n;
}
bool touchPrefsSetChannelScope(int slot, const char* name) {
  if (slot < 0) return false;
  if (!s_begun) touchPrefsBegin();
  char k[8]; chanScopeKey(slot, k);
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  bool ok = s_prefs.putString(k, name ? name : "") > 0;
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

static const char* KEY_LOCK_WALL = "lk_wall";
static const char* DEFAULT_LOCK_WALL = "/lock/placeholder.jpg";

int touchPrefsGetLockWallpaper(char* out, int out_cap) {
  if (!out || out_cap <= 0) return 0;
  out[0] = '\0';
  if (!s_begun) touchPrefsBegin();
  String v = prefsGetStr(KEY_LOCK_WALL, String(DEFAULT_LOCK_WALL));
  int n = (int)v.length();
  if (n > out_cap - 1) n = out_cap - 1;
  if (n > TOUCH_LOCK_WALLPAPER_MAXLEN - 1) n = TOUCH_LOCK_WALLPAPER_MAXLEN - 1;
  memcpy(out, v.c_str(), (size_t)n);
  out[n] = '\0';
  return n;
}

bool touchPrefsSetLockWallpaper(const char* path) {
  if (!path) return false;
  if (!s_begun) touchPrefsBegin();
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  bool ok = s_prefs.putString(KEY_LOCK_WALL, path) > 0;
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

static const char* soundFileKey(int slot) {
  // Distinct from the on/off keys snd_msg/snd_dm/snd_men (those are uchar) —
  // a uchar and a String must not share an NVS key.
  switch (slot) {
    case TOUCH_SND_DM:  return "sndf_dm";
    case TOUCH_SND_MEN: return "sndf_men";
    default:            return "sndf_msg";
  }
}
int touchPrefsGetSoundFile(int slot, char* out, int out_cap) {
  if (!out || out_cap <= 0) return 0;
  out[0] = '\0';
  if (!s_begun) touchPrefsBegin();
  String v = prefsGetStr(soundFileKey(slot), String(""));
  int n = (int)v.length();
  if (n > out_cap - 1) n = out_cap - 1;
  if (n > TOUCH_SOUND_PATH_MAXLEN - 1) n = TOUCH_SOUND_PATH_MAXLEN - 1;
  memcpy(out, v.c_str(), (size_t)n);
  out[n] = '\0';
  return n;
}
bool touchPrefsSetSoundFile(int slot, const char* path) {
  if (!s_begun) touchPrefsBegin();
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  bool ok;
  if (!path || !path[0]) { s_prefs.remove(soundFileKey(slot)); ok = true; }   // empty => built-in chime
  else                     ok = s_prefs.putString(soundFileKey(slot), path) > 0;
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

int touchPrefsGetTimeOffsetHours() {
  if (!s_begun) touchPrefsBegin();
  int v = (int)s_cfg.time_offs;
  if (v < -23) v = -23;
  if (v >  23) v =  23;
  return v;
}
bool touchPrefsSetTimeOffsetHours(int hours) {
  if (hours < -23) hours = -23;
  if (hours >  23) hours =  23;
  if (!s_begun) touchPrefsBegin();
  s_cfg.time_offs = (int8_t)hours;
  return cfgFlush();
}
// Curated time zones, each with the correct POSIX DST rules for that region, so
// non-European users get the right time year-round instead of the CET base + EU
// DST dates. Index 0 (Europe/CET) is the default and matches the old behaviour.
struct TzZone { const char* label; const char* posix; };
static const TzZone TZ_ZONES[] = {
  { "Europe (CET/CEST)",   "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "UK (GMT/BST)",        "GMT0BST,M3.5.0/1,M10.5.0" },
  { "UTC",                 "UTC0" },
  { "US Eastern",          "EST5EDT,M3.2.0,M11.1.0" },
  { "US Central",          "CST6CDT,M3.2.0,M11.1.0" },
  { "US Mountain",         "MST7MDT,M3.2.0,M11.1.0" },
  { "US Arizona (no DST)", "MST7" },
  { "US Pacific",          "PST8PDT,M3.2.0,M11.1.0" },
  { "US Alaska",           "AKST9AKDT,M3.2.0,M11.1.0" },
  { "US Hawaii",           "HST10" },
  { "Canada Atlantic",     "AST4ADT,M3.2.0,M11.1.0" },
  { "Brazil (Brasilia)",   "<-03>3" },
  { "India (IST)",         "IST-5:30" },
  { "China (CST)",         "CST-8" },
  { "Japan (JST)",         "JST-9" },
  { "Sydney (AEST/AEDT)",  "AEST-10AEDT,M10.1.0,M4.1.0/3" },
};
static const int TZ_ZONE_N = (int)(sizeof(TZ_ZONES) / sizeof(TZ_ZONES[0]));

int touchPrefsTimezoneCount() { return TZ_ZONE_N + 1; }   // +1 = "Custom (UTC offset)"

const char* touchPrefsTimezoneLabel(int idx) {
  if (idx >= 0 && idx < TZ_ZONE_N) return TZ_ZONES[idx].label;
  if (idx == TZ_ZONE_N)            return "Custom (UTC offset)";
  return "";
}

uint8_t touchPrefsGetTimezone() {
  if (!s_begun) touchPrefsBegin();
  uint8_t z = s_cfg.tz_zone;
  if (z == 0xFE) {   // v2->v3 migration sentinel: had a manual offset -> Custom zone
    z = (uint8_t)(touchPrefsTimezoneCount() - 1);
    s_cfg.tz_zone = z;
    cfgFlush();
  }
  if (z >= (uint8_t)touchPrefsTimezoneCount()) z = 0;   // stale/garbage -> default
  return z;
}

void touchPrefsSetTimezone(uint8_t idx) {
  if (!s_begun) touchPrefsBegin();
  if (idx >= (uint8_t)touchPrefsTimezoneCount()) idx = 0;
  s_cfg.tz_zone = idx;
  cfgFlush();
}

void touchPrefsBuildLocalTz(char* out, int out_cap) {
  if (!out || out_cap <= 0) return;
  const uint8_t z = touchPrefsGetTimezone();
  if (z < (uint8_t)TZ_ZONE_N) {
    snprintf(out, out_cap, "%s", TZ_ZONES[z].posix);
    return;
  }
  // "Custom (UTC offset)": a fixed offset, no DST. POSIX std-offset is the
  // negation of the UTC offset (it's the time to ADD to local to reach UTC), so
  // UTC-7 -> "<-07>7", UTC+2 -> "<+02>-2".
  const int off = touchPrefsGetTimeOffsetHours();
  if (off == 0) { snprintf(out, out_cap, "UTC0"); return; }
  snprintf(out, out_cap, "<%+03d>%d", off, -off);
}

uint32_t touchPrefsGetLockTextColor() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.lock_color & 0xFFFFFFu;
}

bool touchPrefsSetLockTextColor(uint32_t rgb) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.lock_color = rgb & 0xFFFFFFu;
  return cfgFlush();
}

// Colourful chat bubbles: colour each bubble + sender name by a hash of the
// sender's display name (same name -> same colour). Default ON.
bool touchPrefsGetColorfulBubbles() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.clr_bubbles != 0;
}
bool touchPrefsSetColorfulBubbles(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.clr_bubbles = on ? 1 : 0;
  return cfgFlush();
}

// Keyboard accent-popup picker: when a typed Latin letter has accented variants,
// a tap-to-pick box appears. Default ON. (Distinct from the accent THEME colour.)
bool touchPrefsGetAccentPopups() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.kb_accent != 0;
}
bool touchPrefsSetAccentPopups(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.kb_accent = on ? 1 : 0;
  return cfgFlush();
}

// Web control panel: mirror the live UI to a phone browser (Settings > Wi-Fi).
bool touchPrefsGetWebMirror() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.web_mirror != 0;
}
bool touchPrefsSetWebMirror(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.web_mirror = on ? 1 : 0;
  return cfgFlush();
}

// Remote mode: render the UI off-screen at a web resolution (boot mode; REMOTE app).
bool touchPrefsGetRemoteMode() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.remote_mode != 0;
}
bool touchPrefsSetRemoteMode(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.remote_mode = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetRemoteLandscape() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.remote_landscape != 0;
}
bool touchPrefsSetRemoteLandscape(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.remote_landscape = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetWebTerminal() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.web_terminal != 0;
}
bool touchPrefsSetWebTerminal(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.web_terminal = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetMapTileDebug() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.map_tile_debug != 0;
}
bool touchPrefsSetMapTileDebug(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.map_tile_debug = on ? 1 : 0;
  return cfgFlush();
}

// UI accent colour (buttons, active tab, keyboard, highlights) as 0xRRGGBB.
// Default = the WADAMESH brand teal (the logo dots). The picker clamps it dark
// enough that the off-white button text stays readable on any hue.
uint32_t touchPrefsGetAccentColor() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.accent & 0xFFFFFFu;
}
bool touchPrefsSetAccentColor(uint32_t rgb) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.accent = rgb & 0xFFFFFFu;
  return cfgFlush();
}

// Quick-reply macros. Stored as NVS strings keyed "qr0".."qr5".
// Default factory set on first read so the picker isn't useless out of the
// box and the user has examples to edit.
// Factory defaults skew tactical / radio-comms style — mesh radios get used
// for field ops a lot more than for "calling now" social texting, so seed
// the picker with phrases that actually pull weight on the air. ASCII-only
// so they render identically with or without the extras font fallback.
static const char* k_qr_defaults[TOUCH_QUICK_REPLY_COUNT] = {
  "copy",          // generic acknowledgment
  "wilco",         // will comply
  "stand by",      // wait one
  "moving to RP",  // en route to rally point
  "ETA 5 min",     // arrival estimate
  "RTB",           // returning to base
};

static void qrKeyFor(int idx, char out[8]) {
  out[0] = 'q'; out[1] = 'r';
  out[2] = (char)('0' + (idx & 0x07));
  out[3] = '\0';
}

int touchPrefsGetQuickReply(int idx, char* out, int out_cap) {
  if (!out || out_cap <= 0) return 0;
  out[0] = '\0';
  if (idx < 0 || idx >= TOUCH_QUICK_REPLY_COUNT) return 0;
  if (!s_begun) touchPrefsBegin();
  char key[8];
  qrKeyFor(idx, key);
  String v = prefsGetStr(key, String(k_qr_defaults[idx]));
  int n = (int)v.length();
  if (n > out_cap - 1) n = out_cap - 1;
  if (n > TOUCH_QUICK_REPLY_MAXLEN - 1) n = TOUCH_QUICK_REPLY_MAXLEN - 1;
  memcpy(out, v.c_str(), (size_t)n);
  out[n] = '\0';
  return n;
}

bool touchPrefsGetDutyMeterShown() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.dc_show != 0;
}

bool touchPrefsSetDutyMeterShown(bool show) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.dc_show = show ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetUseMiles() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.use_miles != 0;   // default = km
}

bool touchPrefsSetUseMiles(bool use_miles) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.use_miles = use_miles ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetTilesFromSd() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.tiles_from_sd != 0;   // default = tile server
}

bool touchPrefsSetTilesFromSd(bool from_sd) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.tiles_from_sd = from_sd ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetHideNodeName() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.hide_node_name != 0;   // default = show the name
}

bool touchPrefsSetHideNodeName(bool hide) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.hide_node_name = hide ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetNewContactToast() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.notify_new_contact != 0;   // default = show the toast
}

bool touchPrefsSetNewContactToast(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.notify_new_contact = on ? 1 : 0;
  return cfgFlush();
}

// Heltec V4 Expansion Kit: show the Sensors tab + the Home env widget. Default
// ON. The UI also requires an ENVIRONMENT sensor to actually be present (checked
// at runtime in buildUiTree), so a bare V4 hides the UI even with this on.
bool touchPrefsGetShowSensorsTab() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.show_sensors_tab != 0;   // default = show the Sensors tab
}

bool touchPrefsSetShowSensorsTab(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.show_sensors_tab = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetMapNight() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.map_night != 0;
}
bool touchPrefsSetMapNight(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.map_night = on ? 1 : 0;
  return cfgFlush();
}
uint8_t touchPrefsGetHistSyncAfter() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.hist_sync_after > 9 ? 9 : s_cfg.hist_sync_after;
}
bool touchPrefsSetHistSyncAfter(uint8_t n) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.hist_sync_after = n > 9 ? 9 : n;
  return cfgFlush();
}
uint8_t touchPrefsGetMapZoom() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.map_zoom;
}
bool touchPrefsSetMapZoom(uint8_t z) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.map_zoom = z;
  return cfgFlush();
}
bool touchPrefsGetSleepIdle() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.sleep_idle != 0;
}
bool touchPrefsSetSleepIdle(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.sleep_idle = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetMapShowCoords() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.map_show_coords != 0;
}
bool touchPrefsSetMapShowCoords(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.map_show_coords = on ? 1 : 0;
  return cfgFlush();
}
bool touchPrefsGetMapShowTileXYZ() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.map_show_tilexyz != 0;
}
bool touchPrefsSetMapShowTileXYZ(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.map_show_tilexyz = on ? 1 : 0;
  return cfgFlush();
}
bool touchPrefsGetMapShowContacts() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.map_show_contacts != 0;
}
bool touchPrefsSetMapShowContacts(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.map_show_contacts = on ? 1 : 0;
  return cfgFlush();
}
bool touchPrefsGetMapShowLinks() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.map_show_links != 0;
}
bool touchPrefsSetMapShowLinks(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.map_show_links = on ? 1 : 0;
  return cfgFlush();
}
uint8_t touchPrefsGetMapStyle() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.map_style;
}
bool touchPrefsSetMapStyle(uint8_t style) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.map_style = style;
  return cfgFlush();
}
bool touchPrefsGetAppGridLarge() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.app_grid_large != 0;
}
bool touchPrefsSetAppGridLarge(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.app_grid_large = on ? 1 : 0;
  return cfgFlush();
}

uint8_t touchPrefsGetUiScale() {
  if (!s_begun) touchPrefsBegin();
#if defined(TLORA_PAGER)
  return s_cfg.ui_scale > 3 ? 0 : s_cfg.ui_scale;
#else
  return s_cfg.ui_scale > 2 ? 0 : s_cfg.ui_scale;
#endif
}
bool touchPrefsSetUiScale(uint8_t scale) {
  if (!s_begun) touchPrefsBegin();
#if defined(TLORA_PAGER)
  s_cfg.ui_scale = scale > 3 ? 0 : scale;
#else
  s_cfg.ui_scale = scale > 2 ? 0 : scale;
#endif
  return cfgFlush();
}

bool touchPrefsGetKbdNav() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.kbd_nav != 0;
}
bool touchPrefsSetKbdNav(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.kbd_nav = on ? 1 : 0;
  return cfgFlush();
}
bool touchPrefsGetTbNav() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.tb_nav != 0;
}
bool touchPrefsSetTbNav(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.tb_nav = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetScopeDirect() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.scope_direct != 0;
}
bool touchPrefsSetScopeDirect(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.scope_direct = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetFemLna() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.fem_lna != 0;
}
bool touchPrefsSetFemLna(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.fem_lna = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetMsgFlash() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.msg_flash != 0;
}
bool touchPrefsSetMsgFlash(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.msg_flash = on ? 1 : 0;
  return cfgFlush();
}

// Console mode is a BOOT mode, so this is read before the UI is built. It fails
// safe by construction: the only value that means console is exactly 1, so a
// corrupt or unreadable pref boots the graphical UI, which is the mode everyone
// can use. Never invert this test.
bool touchPrefsGetConsoleMode() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.console_mode == 1;
}
bool touchPrefsSetConsoleMode(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.console_mode = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetKbForceLegacy() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.kb_force_legacy != 0;
}
bool touchPrefsSetKbForceLegacy(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.kb_force_legacy = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetConsoleMonitor() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.console_monitor != 0;
}
bool touchPrefsSetConsoleMonitor(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.console_monitor = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetBootAdvert() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.boot_advert != 0;
}
bool touchPrefsSetBootAdvert(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.boot_advert = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetRxQueue() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.rx_queue != 0;
}
uint32_t touchPrefsGetAppHide() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.app_hide;
}
bool touchPrefsSetAppHide(uint32_t mask) {
  if (!s_begun) touchPrefsBegin();
  if (s_cfg.app_hide == mask) return true;
  s_cfg.app_hide = mask;
  return cfgFlush();
}
bool touchPrefsGetRetryEcho() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.retry_echo != 0;
}
bool touchPrefsSetRetryEcho(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.retry_echo = on ? 1 : 0;
  return cfgFlush();
}
bool touchPrefsSetRxQueue(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.rx_queue = on ? 1 : 0;
  return cfgFlush();
}

bool touchPrefsGetCompactChat() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.compact_chat != 0;
}
bool touchPrefsSetCompactChat(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.compact_chat = on ? 1 : 0;
  return cfgFlush();
}

// Monotonic send-timestamp floor (ClockFloorRTC, issue #89). Written rate-capped
// from UITask::loop + on shutdown; only ever grows between resets.
uint32_t touchPrefsGetClockFloor() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.clock_floor;
}
bool touchPrefsSetClockFloor(uint32_t epoch) {
  if (!s_begun) touchPrefsBegin();
  if (epoch <= s_cfg.clock_floor) return true;   // never regress the persisted floor
  s_cfg.clock_floor = epoch;
  return cfgFlush();
}

// Periodic self-advert intervals (0 = off). Validation mirrors MeshCore: flood in hours (cap 168);
// local zero-hop in minutes, 0 or 60-240 (MeshCore's MIN_LOCAL_ADVERT_INTERVAL is 60).
uint16_t touchPrefsGetFloodAdvHrs() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.flood_adv_hrs;
}
bool touchPrefsSetFloodAdvHrs(uint16_t hrs) {
  if (!s_begun) touchPrefsBegin();
  if (hrs > 168) hrs = 168;
  s_cfg.flood_adv_hrs = (uint8_t)hrs;
  return cfgFlush();
}
uint16_t touchPrefsGetLocalAdvMin() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.local_adv_min;
}
bool touchPrefsSetLocalAdvMin(uint16_t mins) {
  if (!s_begun) touchPrefsBegin();
  if (mins != 0) { if (mins < 60) mins = 60; if (mins > 240) mins = 240; }
  s_cfg.local_adv_min = mins;
  return cfgFlush();
}
bool touchPrefsGetBetaUpdates() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.beta_updates != 0;
}
bool touchPrefsSetBetaUpdates(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.beta_updates = on ? 1 : 0;
  return cfgFlush();
}

uint8_t touchPrefsGetNavKey(int tab) {
  if (!s_begun) touchPrefsBegin();
  if (tab < 0 || tab >= 5) return 0;
  return s_cfg.nav_keys[tab];
}
bool touchPrefsSetNavKey(int tab, uint8_t ch) {
  if (!s_begun) touchPrefsBegin();
  if (tab < 0 || tab >= 5) return false;
  s_cfg.nav_keys[tab] = ch;
  return cfgFlush();
}

bool touchPrefsGetMapZoomButtons() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.map_zoom_buttons != 0;
}
bool touchPrefsSetMapZoomButtons(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.map_zoom_buttons = on ? 1 : 0;
  return cfgFlush();
}

uint8_t touchPrefsGetNavDirKey(int idx) {   // idx 0-5 = move/select/back, 6-7 = scroll up/down
  if (!s_begun) touchPrefsBegin();
  if (idx < 0 || idx >= 8) return 0;
  return (idx < 6) ? s_cfg.nav_dir_keys[idx] : s_cfg.nav_scroll_keys[idx - 6];
}
bool touchPrefsSetNavDirKey(int idx, uint8_t ch) {
  if (!s_begun) touchPrefsBegin();
  if (idx < 0 || idx >= 8) return false;
  if (idx < 6) s_cfg.nav_dir_keys[idx] = ch;
  else         s_cfg.nav_scroll_keys[idx - 6] = ch;
  return cfgFlush();
}

bool touchPrefsGetHomeIsDrawer() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.home_is_drawer != 0;
}
bool touchPrefsSetHomeIsDrawer(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.home_is_drawer = on ? 1 : 0;
  return cfgFlush();
}

// Store ALL device data (identity, prefs, contacts, channels) on the SD card
// under /meshcomod instead of internal SPIFFS. Read at boot (main.cpp) BEFORE
// the data loads, so changing it needs a reboot. Key "use_sd" in the "touch"
// namespace — main.cpp must see the same value the UI toggle writes.
static const char* KEY_USE_SD_STORAGE = "use_sd";
static const char* BOOT_PREFS_NS = "bootprefs";

bool touchPrefsReadUseSdAtBoot() {
  bool file_val = false;
  bool found = SdNvsPrefs::readFileBool((fs::FS*)&SPIFFS, "/prefs", BOOT_PREFS_NS,
                                       KEY_USE_SD_STORAGE, file_val);
  // Once present, the explicit boot namespace is authoritative. This lets a
  // Launcher device recover even if its best-effort NVS mirror is stale.
  if (found) {
    Preferences mirror;
    if (mirror.begin(TOUCH_NS, false)) {
      mirror.putBool(KEY_USE_SD_STORAGE, file_val);
      mirror.end();
    }
    return file_val;
  }

  bool nvs_val = false;
  Preferences p;
  if (p.begin(TOUCH_NS, true)) {
    nvs_val = p.getBool(KEY_USE_SD_STORAGE, false);
    p.end();
  }
  if (nvs_val) return true;
  // One-time migration for builds that stored the boot flag inside touch.kv.
  found = SdNvsPrefs::readFileBool((fs::FS*)&SPIFFS, "/prefs", TOUCH_NS,
                                   KEY_USE_SD_STORAGE, file_val);
  if (found && file_val) {
    Serial.println("[BOOT] use_sd read from SPIFFS boot prefs (syncing to NVS)");
    if (p.begin(TOUCH_NS, false)) {
      p.putBool(KEY_USE_SD_STORAGE, true);
      p.end();
    }
  }
  return found && file_val;
}

bool touchPrefsGetUseSdStorage() {
  if (!s_begun) touchPrefsBegin();
  return s_prefs.getBool(KEY_USE_SD_STORAGE, false);   // default = SPIFFS
}

bool touchPrefsSetUseSdStorage(bool use_sd) {
  if (!s_begun) touchPrefsBegin();
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  bool ok = s_prefs.putBool(KEY_USE_SD_STORAGE, use_sd);
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  // Mirror to raw NVS for the boot read (PR #123). Best effort ONLY: on
  // Launcher installs NVS is unusable and the file write above is the
  // authoritative copy — a failed mirror must not fail the setter there.
  Preferences nvs;
  if (nvs.begin(TOUCH_NS, false)) {
    nvs.putBool(KEY_USE_SD_STORAGE, use_sd);
    nvs.end();
  }
  // The boot storage decision happens before SD is mounted. Keep a tiny,
  // crash-safe SPIFFS A/B namespace as the Launcher-safe fallback regardless
  // of where the rest of the preferences currently live.
  const bool boot_ok = SdNvsPrefs::writeFileBool((fs::FS*)&SPIFFS, "/prefs", BOOT_PREFS_NS,
                                                 KEY_USE_SD_STORAGE, use_sd);
  return ok && boot_ok;
}

// UI language index (UiLang enum in i18n.h; 0 = English). Read at boot to pick
// the active translation language. Packed into the "cfg" blob.
uint8_t touchPrefsGetUiLang() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.ui_lang;   // default = English
}
bool touchPrefsSetUiLang(uint8_t lang) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.ui_lang = lang;
  return cfgFlush();
}
void touchPrefsGetLangFile(char* out, size_t cap) {
  if (!out || !cap) return;
  if (!s_begun) touchPrefsBegin();
  size_t n = strnlen(s_cfg.lang_file, sizeof s_cfg.lang_file);
  if (n >= cap) n = cap - 1;
  memcpy(out, s_cfg.lang_file, n);
  out[n] = 0;
}
bool touchPrefsSetLangFile(const char* code) {
  if (!s_begun) touchPrefsBegin();
  memset(s_cfg.lang_file, 0, sizeof s_cfg.lang_file);
  if (code) strncpy(s_cfg.lang_file, code, sizeof s_cfg.lang_file - 1);
  return cfgFlush();
}

uint8_t touchPrefsGetUiRotation() {
  if (!s_begun) touchPrefsBegin();
  uint8_t r = s_cfg.ui_rotation;   // default = portrait
  return (r <= 3) ? r : 0;
}

bool touchPrefsSetUiRotation(uint8_t rot) {
  if (rot > 3) rot = 0;
  if (!s_begun) touchPrefsBegin();
  s_cfg.ui_rotation = rot;
  return cfgFlush();
}

uint16_t touchPrefsGetBattFullMv() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.batt_full_mv;   // 0 = not calibrated -> default 4200
}

bool touchPrefsSetBattFullMv(uint16_t mv) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.batt_full_mv = mv;
  return cfgFlush();
}

// Wi-Fi profile slots ----------------------------------------------------
//
// NVS keys: "wsl_<idx>_l" (label), "wsl_<idx>_s" (ssid), "wsl_<idx>_p" (pwd).
// 3 slots × 3 strings = 9 small entries; well under the 12 KB NVS default.

static void wifiSlotKey(int idx, char kind, char out[12]) {
  // wsl<idx><kind>  → "wsl0l", "wsl1s", "wsl2p", ...
  int p = 0;
  out[p++] = 'w'; out[p++] = 's'; out[p++] = 'l';
  out[p++] = (char)('0' + (idx & 0x07));
  out[p++] = kind;
  out[p]   = '\0';
}

bool touchPrefsGetWifiSlot(int idx, char* label, int label_cap,
                           char* ssid, int ssid_cap,
                           char* pwd, int pwd_cap) {
  if (idx < 0 || idx >= TOUCH_WIFI_SLOT_COUNT) return false;
  if (label && label_cap > 0) label[0] = '\0';
  if (ssid  && ssid_cap  > 0) ssid[0]  = '\0';
  if (pwd   && pwd_cap   > 0) pwd[0]   = '\0';
  if (!s_begun) touchPrefsBegin();
  char k[12];
  if (label && label_cap > 0) {
    wifiSlotKey(idx, 'l', k);
    String v = prefsGetStr(k, "");
    int n = (int)v.length();
    if (n > label_cap - 1) n = label_cap - 1;
    if (n > 0) memcpy(label, v.c_str(), (size_t)n);
    label[n] = '\0';
  }
  if (ssid && ssid_cap > 0) {
    wifiSlotKey(idx, 's', k);
    String v = prefsGetStr(k, "");
    int n = (int)v.length();
    if (n > ssid_cap - 1) n = ssid_cap - 1;
    if (n > 0) memcpy(ssid, v.c_str(), (size_t)n);
    ssid[n] = '\0';
  }
  if (pwd && pwd_cap > 0) {
    wifiSlotKey(idx, 'p', k);
    String v = prefsGetStr(k, "");
    int n = (int)v.length();
    if (n > pwd_cap - 1) n = pwd_cap - 1;
    if (n > 0) memcpy(pwd, v.c_str(), (size_t)n);
    pwd[n] = '\0';
  }
  return true;
}

bool touchPrefsSetWifiSlot(int idx, const char* label,
                           const char* ssid, const char* pwd) {
  if (idx < 0 || idx >= TOUCH_WIFI_SLOT_COUNT) return false;
  if (!s_begun) touchPrefsBegin();
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  char k[12];
  wifiSlotKey(idx, 'l', k); s_prefs.putString(k, label ? label : "");
  wifiSlotKey(idx, 's', k); s_prefs.putString(k, ssid  ? ssid  : "");
  wifiSlotKey(idx, 'p', k); s_prefs.putString(k, pwd   ? pwd   : "");
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return true;
}

// ---- Saved "known networks" store (reworked iPhone/Android-style Wi-Fi) -----
// Keys per idx: "wn<idx>s" ssid, "wn<idx>p" pwd, "wn<idx>f" flags uchar
// (bit0 used, bit1 auto-join), "wn<idx>r" rank uint32. "wnctr" = global recency.
static void wifiNetKey(int idx, char kind, char out[8]) {
  int p = 0;
  out[p++] = 'w'; out[p++] = 'n';
  out[p++] = (char)('0' + (idx & 0x07));
  out[p++] = kind;
  out[p]   = '\0';
}

bool touchPrefsGetWifiNet(int idx, TouchWifiNet& out) {
  memset(&out, 0, sizeof(out));
  if (idx < 0 || idx >= TOUCH_WIFI_NET_COUNT) return false;
  if (!s_begun) touchPrefsBegin();
  char k[8];
  wifiNetKey(idx, 'f', k);
  const uint8_t flags = s_prefs.isKey(k) ? s_prefs.getUChar(k, 0) : 0;
  out.used      = (flags & 0x01) != 0;
  out.auto_join = (flags & 0x02) != 0;
  if (!out.used) return true;
  wifiNetKey(idx, 's', k); { String v = prefsGetStr(k, ""); strlcpy(out.ssid, v.c_str(), sizeof(out.ssid)); }
  wifiNetKey(idx, 'p', k); { String v = prefsGetStr(k, ""); strlcpy(out.pwd,  v.c_str(), sizeof(out.pwd)); }
  wifiNetKey(idx, 'r', k); out.rank = s_prefs.isKey(k) ? s_prefs.getUInt(k, 0) : 0;
  return true;
}

int touchPrefsFindWifiNet(const char* ssid) {
  if (!ssid || !ssid[0]) return -1;
  TouchWifiNet n;
  for (int i = 0; i < TOUCH_WIFI_NET_COUNT; ++i)
    if (touchPrefsGetWifiNet(i, n) && n.used && strcmp(n.ssid, ssid) == 0) return i;
  return -1;
}

int touchPrefsSaveWifiNet(const char* ssid, const char* pwd, bool auto_join) {
  if (!ssid || !ssid[0]) return -1;
  if (!s_begun) touchPrefsBegin();
  TouchWifiNet n;
  int idx = touchPrefsFindWifiNet(ssid);           // update existing by ssid
  if (idx < 0) {                                   // else first free slot
    for (int i = 0; i < TOUCH_WIFI_NET_COUNT && idx < 0; ++i)
      if (touchPrefsGetWifiNet(i, n) && !n.used) idx = i;
  }
  if (idx < 0) {                                   // else evict the least-recent
    uint32_t lo = UINT32_MAX; int loi = 0;
    for (int i = 0; i < TOUCH_WIFI_NET_COUNT; ++i) {
      touchPrefsGetWifiNet(i, n);
      if (n.rank <= lo) { lo = n.rank; loi = i; }
    }
    idx = loi;
  }
  // Preserve the existing passphrase if the caller passed an empty one (e.g. a
  // metadata-only re-save).
  char use_pwd[65];
  if (pwd && pwd[0]) strlcpy(use_pwd, pwd, sizeof(use_pwd));
  else { TouchWifiNet ex; use_pwd[0] = '\0'; if (touchPrefsGetWifiNet(idx, ex) && ex.used) strlcpy(use_pwd, ex.pwd, sizeof(use_pwd)); }
  const uint32_t ctr = (s_prefs.isKey("wnctr") ? s_prefs.getUInt("wnctr", 0) : 0) + 1;
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) { s_begun = s_prefs.begin(TOUCH_NS, true); return -1; }
  char k[8];
  wifiNetKey(idx, 's', k); s_prefs.putString(k, ssid);
  wifiNetKey(idx, 'p', k); s_prefs.putString(k, use_pwd);
  wifiNetKey(idx, 'f', k); s_prefs.putUChar(k, (uint8_t)(0x01 | (auto_join ? 0x02 : 0)));
  wifiNetKey(idx, 'r', k); s_prefs.putUInt(k, ctr);
  s_prefs.putUInt("wnctr", ctr);
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return idx;
}

bool touchPrefsForgetWifiNet(int idx) {
  if (idx < 0 || idx >= TOUCH_WIFI_NET_COUNT) return false;
  TouchWifiNet n;
  const bool had = touchPrefsGetWifiNet(idx, n) && n.used;
  if (!s_begun) touchPrefsBegin();
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) { s_begun = s_prefs.begin(TOUCH_NS, true); return false; }
  char k[8];
  wifiNetKey(idx, 's', k); s_prefs.remove(k);
  wifiNetKey(idx, 'p', k); s_prefs.remove(k);
  wifiNetKey(idx, 'f', k); s_prefs.remove(k);   // clears the used bit
  wifiNetKey(idx, 'r', k); s_prefs.remove(k);
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  // If this was the network the device is using/targeting, also drop the ACTIVE
  // credentials so main.cpp stops trying to (re)connect to it (the reconnect loop
  // is gated on wifiConfigHasRuntime(), which clearing makes false).
  if (had && n.ssid[0]) {
    char active[WIFI_CONFIG_SSID_MAX] = {0};
    wifiConfigGetSsid(active, sizeof active);
    if (strcmp(active, n.ssid) == 0) {
      wifiConfigClear();
      wifiConfigRequestApply();
    }
  }
  return true;
}

bool touchPrefsSetWifiNetAutoJoin(int idx, bool on) {
  TouchWifiNet n;
  if (!touchPrefsGetWifiNet(idx, n) || !n.used) return false;
  if (!s_begun) touchPrefsBegin();
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) { s_begun = s_prefs.begin(TOUCH_NS, true); return false; }
  char k[8];
  wifiNetKey(idx, 'f', k); s_prefs.putUChar(k, (uint8_t)(0x01 | (on ? 0x02 : 0)));
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return true;
}

bool touchPrefsConnectWifiNet(int idx) {
  TouchWifiNet n;
  if (!touchPrefsGetWifiNet(idx, n) || !n.used || !n.ssid[0]) return false;
  if (!wifiConfigSetSsid(n.ssid)) return false;
  if (!wifiConfigSetPwd(n.pwd))   return false;
  wifiConfigSetRadioEnabled(true);
  wifiConfigRequestApply();
  touchPrefsSaveWifiNet(n.ssid, n.pwd, n.auto_join);   // re-save bumps recency
  return true;
}

// Favorites blob (raw bytes: N * 6-byte pub_key prefixes, packed) ----------
static const char* KEY_FAV = "fav";

static int favReadAll(uint8_t out[TOUCH_FAVORITES_MAX * TOUCH_FAVORITE_KEY_BYTES]) {
  if (!s_begun) touchPrefsBegin();
  if (!s_prefs.isKey(KEY_FAV)) return 0;   // absent on a fresh device — skip the [E] NOT_FOUND log
  size_t n = s_prefs.getBytes(KEY_FAV, out, TOUCH_FAVORITES_MAX * TOUCH_FAVORITE_KEY_BYTES);
  if (n == 0 || n > (size_t)(TOUCH_FAVORITES_MAX * TOUCH_FAVORITE_KEY_BYTES)) return 0;
  // Round down to a whole number of entries — guards against NVS returning
  // a half-written blob from a power-cut mid-write.
  return (int)(n / TOUCH_FAVORITE_KEY_BYTES);
}

static bool favWriteAll(const uint8_t* buf, int count) {
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  bool ok;
  if (count <= 0) {
    s_prefs.remove(KEY_FAV);
    ok = true;
  } else {
    ok = s_prefs.putBytes(KEY_FAV, buf, (size_t)(count * TOUCH_FAVORITE_KEY_BYTES)) > 0;
  }
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

bool touchPrefsIsFavorite(const uint8_t* pub_key6) {
  if (!pub_key6) return false;
  uint8_t buf[TOUCH_FAVORITES_MAX * TOUCH_FAVORITE_KEY_BYTES];
  int n = favReadAll(buf);
  for (int i = 0; i < n; ++i) {
    if (memcmp(&buf[i * TOUCH_FAVORITE_KEY_BYTES], pub_key6, TOUCH_FAVORITE_KEY_BYTES) == 0) return true;
  }
  return false;
}

int touchPrefsCopyFavorites(uint8_t* out_buf) {
  if (!out_buf) return 0;
  return favReadAll(out_buf);
}

bool touchPrefsFavoritesSnapshotContains(const uint8_t* snapshot, int count,
                                          const uint8_t* pub_key6) {
  if (!snapshot || !pub_key6 || count <= 0) return false;
  for (int i = 0; i < count; ++i) {
    if (memcmp(&snapshot[i * TOUCH_FAVORITE_KEY_BYTES], pub_key6,
               TOUCH_FAVORITE_KEY_BYTES) == 0) return true;
  }
  return false;
}

bool touchPrefsSetFavorite(const uint8_t* pub_key6, bool fav) {
  if (!pub_key6) return false;
  uint8_t buf[TOUCH_FAVORITES_MAX * TOUCH_FAVORITE_KEY_BYTES];
  int n = favReadAll(buf);
  int found = -1;
  for (int i = 0; i < n; ++i) {
    if (memcmp(&buf[i * TOUCH_FAVORITE_KEY_BYTES], pub_key6, TOUCH_FAVORITE_KEY_BYTES) == 0) {
      found = i; break;
    }
  }
  if (fav) {
    if (found >= 0) return true;
    if (n >= TOUCH_FAVORITES_MAX) return false;   // cap reached, silently refuse
    memcpy(&buf[n * TOUCH_FAVORITE_KEY_BYTES], pub_key6, TOUCH_FAVORITE_KEY_BYTES);
    ++n;
    favWriteAll(buf, n);
    return true;
  } else {
    if (found < 0) return false;
    // Shift remaining entries down to keep the blob packed.
    for (int i = found; i < n - 1; ++i) {
      memcpy(&buf[i * TOUCH_FAVORITE_KEY_BYTES],
             &buf[(i + 1) * TOUCH_FAVORITE_KEY_BYTES],
             TOUCH_FAVORITE_KEY_BYTES);
    }
    --n;
    favWriteAll(buf, n);
    return false;
  }
}

// Ignored / blocked senders -------------------------------------------------
//
// Same scheme as favorites: a single NVS blob "ign" of up to TOUCH_IGNORED_MAX
// 6-byte pubkey prefixes. Incoming messages from a stored prefix are dropped
// (no chat entry, no notification). Managed from the chat "Blocked users" sheet.
static const char* KEY_IGN = "ign";

static int ignReadAll(uint8_t out[TOUCH_IGNORED_MAX * TOUCH_IGNORE_KEY_BYTES]) {
  if (!s_begun) touchPrefsBegin();
  if (!s_prefs.isKey(KEY_IGN)) return 0;   // absent on a fresh device — skip the [E] NOT_FOUND log
  size_t n = s_prefs.getBytes(KEY_IGN, out, TOUCH_IGNORED_MAX * TOUCH_IGNORE_KEY_BYTES);
  if (n == 0 || n > (size_t)(TOUCH_IGNORED_MAX * TOUCH_IGNORE_KEY_BYTES)) return 0;
  return (int)(n / TOUCH_IGNORE_KEY_BYTES);
}

static bool ignWriteAll(const uint8_t* buf, int count) {
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  bool ok;
  if (count <= 0) { s_prefs.remove(KEY_IGN); ok = true; }
  else ok = s_prefs.putBytes(KEY_IGN, buf, (size_t)(count * TOUCH_IGNORE_KEY_BYTES)) > 0;
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

bool touchPrefsIsIgnored(const uint8_t* pub_key6) {
  if (!pub_key6) return false;
  uint8_t buf[TOUCH_IGNORED_MAX * TOUCH_IGNORE_KEY_BYTES];
  int n = ignReadAll(buf);
  for (int i = 0; i < n; ++i)
    if (memcmp(&buf[i * TOUCH_IGNORE_KEY_BYTES], pub_key6, TOUCH_IGNORE_KEY_BYTES) == 0) return true;
  return false;
}

int touchPrefsCopyIgnored(uint8_t* out_buf) {
  if (!out_buf) return 0;
  return ignReadAll(out_buf);
}

bool touchPrefsSetIgnored(const uint8_t* pub_key6, bool ignored) {
  if (!pub_key6) return false;
  uint8_t buf[TOUCH_IGNORED_MAX * TOUCH_IGNORE_KEY_BYTES];
  int n = ignReadAll(buf);
  int found = -1;
  for (int i = 0; i < n; ++i)
    if (memcmp(&buf[i * TOUCH_IGNORE_KEY_BYTES], pub_key6, TOUCH_IGNORE_KEY_BYTES) == 0) { found = i; break; }
  if (ignored) {
    if (found >= 0) return true;
    if (n >= TOUCH_IGNORED_MAX) return false;   // cap reached, silently refuse
    memcpy(&buf[n * TOUCH_IGNORE_KEY_BYTES], pub_key6, TOUCH_IGNORE_KEY_BYTES);
    ++n; ignWriteAll(buf, n); return true;
  } else {
    if (found < 0) return false;
    for (int i = found; i < n - 1; ++i)
      memcpy(&buf[i * TOUCH_IGNORE_KEY_BYTES], &buf[(i + 1) * TOUCH_IGNORE_KEY_BYTES], TOUCH_IGNORE_KEY_BYTES);
    --n; ignWriteAll(buf, n); return false;
  }
}

// Ignored / blocked sender NAMES (channel/room senders that aren't contacts) ---
// One NVS blob "ign_nm" of up to TOUCH_IGNORED_NAMES_MAX fixed-width,
// NUL-padded TOUCH_IGNORED_NAME_LEN slots. Same read/replace/write scheme as
// the 6-byte prefix list above.
static const char* KEY_IGN_NAMES = "ign_nm";

static int ignNamesReadAll(char out[TOUCH_IGNORED_NAMES_MAX * TOUCH_IGNORED_NAME_LEN]) {
  if (!s_begun) touchPrefsBegin();
  if (!s_prefs.isKey(KEY_IGN_NAMES)) return 0;   // absent on a fresh device — skip [E] NOT_FOUND
  size_t n = s_prefs.getBytes(KEY_IGN_NAMES, out, TOUCH_IGNORED_NAMES_MAX * TOUCH_IGNORED_NAME_LEN);
  if (n == 0 || n > (size_t)(TOUCH_IGNORED_NAMES_MAX * TOUCH_IGNORED_NAME_LEN)) return 0;
  return (int)(n / TOUCH_IGNORED_NAME_LEN);
}

static bool ignNamesWriteAll(const char* buf, int count) {
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  bool ok;
  if (count <= 0) { s_prefs.remove(KEY_IGN_NAMES); ok = true; }
  else ok = s_prefs.putBytes(KEY_IGN_NAMES, buf, (size_t)(count * TOUCH_IGNORED_NAME_LEN)) > 0;
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

bool touchPrefsIsNameIgnored(const char* name) {
  if (!name || !name[0]) return false;
  char buf[TOUCH_IGNORED_NAMES_MAX * TOUCH_IGNORED_NAME_LEN];
  int n = ignNamesReadAll(buf);
  for (int i = 0; i < n; ++i)
    if (strncmp(&buf[i * TOUCH_IGNORED_NAME_LEN], name, TOUCH_IGNORED_NAME_LEN) == 0) return true;
  return false;
}

int touchPrefsCopyIgnoredNames(char* out_buf) {
  if (!out_buf) return 0;
  return ignNamesReadAll(out_buf);
}

bool touchPrefsSetNameIgnored(const char* name, bool ignored) {
  if (!name || !name[0]) return false;
  char buf[TOUCH_IGNORED_NAMES_MAX * TOUCH_IGNORED_NAME_LEN];
  int n = ignNamesReadAll(buf);
  int found = -1;
  for (int i = 0; i < n; ++i)
    if (strncmp(&buf[i * TOUCH_IGNORED_NAME_LEN], name, TOUCH_IGNORED_NAME_LEN) == 0) { found = i; break; }
  if (ignored) {
    if (found >= 0) return true;
    if (n >= TOUCH_IGNORED_NAMES_MAX) return false;   // cap reached, silently refuse
    char* slot = &buf[n * TOUCH_IGNORED_NAME_LEN];
    memset(slot, 0, TOUCH_IGNORED_NAME_LEN);
    strncpy(slot, name, TOUCH_IGNORED_NAME_LEN - 1);
    ++n; ignNamesWriteAll(buf, n); return true;
  } else {
    if (found < 0) return false;
    for (int i = found; i < n - 1; ++i)
      memcpy(&buf[i * TOUCH_IGNORED_NAME_LEN], &buf[(i + 1) * TOUCH_IGNORED_NAME_LEN], TOUCH_IGNORED_NAME_LEN);
    --n; ignNamesWriteAll(buf, n); return false;
  }
}

// Notification-sound prefs (individual NVS keys — integer getters don't emit the
// [E] NOT_FOUND log that getString/getBytes do on a fresh device) -----------
static void prefsPutUChar(const char* key, uint8_t v) {
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) { s_begun = s_prefs.begin(TOUCH_NS, true); return; }
  s_prefs.putUChar(key, v);
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
}
// Same re-open dance as prefsPutUChar above, for the one 16-bit setting.
static void prefsPutUShort(const char* key, uint16_t v) {
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) { s_begun = s_prefs.begin(TOUCH_NS, true); return; }
  s_prefs.putUShort(key, v);
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
}
bool touchPrefsGetSoundMessages() {
  if (!s_begun) touchPrefsBegin();
  return s_prefs.getUChar("snd_msg", 1) != 0;
}
void touchPrefsSetSoundMessages(bool on) { if (!s_begun) touchPrefsBegin(); prefsPutUChar("snd_msg", on ? 1 : 0); }
bool touchPrefsGetSoundMentions() {
  if (!s_begun) touchPrefsBegin();
  return s_prefs.getUChar("snd_men", 1) != 0;
}
void touchPrefsSetSoundMentions(bool on) { if (!s_begun) touchPrefsBegin(); prefsPutUChar("snd_men", on ? 1 : 0); }
bool touchPrefsGetSoundDirect() {
  if (!s_begun) touchPrefsBegin();
  return s_prefs.getUChar("snd_dm", 1) != 0;
}
void touchPrefsSetSoundDirect(bool on) { if (!s_begun) touchPrefsBegin(); prefsPutUChar("snd_dm", on ? 1 : 0); }
// Drop incoming text messages whose body is a single character. Mesh spam is
// overwhelmingly 1-byte payloads (cheapest possible airtime per message), and a
// 1-character message is never something a person meant to send. Off by default:
// it is a filter on other people's traffic, so it should be a deliberate choice.
bool touchPrefsGetIgnoreTinyMsgs() {
  if (!s_begun) touchPrefsBegin();
  return s_prefs.getUChar("ign_tiny", 0) != 0;
}
void touchPrefsSetIgnoreTinyMsgs(bool on) { if (!s_begun) touchPrefsBegin(); prefsPutUChar("ign_tiny", on ? 1 : 0); }

// Most contact dots to draw on the map at once. 0 = no limit (draw every positioned
// contact in view, up to the firmware's own ceiling), which is the default: a map that
// quietly stops plotting is worse than a slow one. Lower it on a board that struggles.
uint16_t touchPrefsGetMapMarkerCap() {
  if (!s_begun) touchPrefsBegin();
  return s_prefs.getUShort("map_cap", 0);
}
void touchPrefsSetMapMarkerCap(uint16_t n) { if (!s_begun) touchPrefsBegin(); prefsPutUShort("map_cap", n); }

bool touchPrefsGetDiscoveredAutoEvict() {
  if (!s_begun) touchPrefsBegin();
  return s_prefs.getUChar("dsc_evict", 1) != 0;
}
void touchPrefsSetDiscoveredAutoEvict(bool on) { if (!s_begun) touchPrefsBegin(); prefsPutUChar("dsc_evict", on ? 1 : 0); }
uint8_t touchPrefsGetDiscoveredMaxHops() {
  if (!s_begun) touchPrefsBegin();
  return s_prefs.getUChar("dsc_hops", 0);   // 0 = off
}
void touchPrefsSetDiscoveredMaxHops(uint8_t hops) { if (!s_begun) touchPrefsBegin(); prefsPutUChar("dsc_hops", hops); }
bool touchPrefsGetEnterSends()      { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("ent_send", 1) != 0; }
void touchPrefsSetEnterSends(bool on)      { if (!s_begun) touchPrefsBegin(); prefsPutUChar("ent_send", on ? 1 : 0); }
bool touchPrefsGetClock12h()        { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("clk_12h", 0) != 0; }
void touchPrefsSetClock12h(bool on)        { if (!s_begun) touchPrefsBegin(); prefsPutUChar("clk_12h", on ? 1 : 0); }
bool touchPrefsGetNavMenubarKeys()         { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("nav_mbk", 0) != 0; }
void touchPrefsSetNavMenubarKeys(bool on)  { if (!s_begun) touchPrefsBegin(); prefsPutUChar("nav_mbk", on ? 1 : 0); }
bool touchPrefsGetScrollReverse()   { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("tb_rev", 0) != 0; }
void touchPrefsSetScrollReverse(bool on)   { if (!s_begun) touchPrefsBegin(); prefsPutUChar("tb_rev", on ? 1 : 0); }
bool touchPrefsGetEdgeScroll()      { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("tb_edgesc", 0) != 0; }
void touchPrefsSetEdgeScroll(bool on)      { if (!s_begun) touchPrefsBegin(); prefsPutUChar("tb_edgesc", on ? 1 : 0); }
bool touchPrefsGetLockOnScreenOff() { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("lock_off", 0) != 0; }
void touchPrefsSetLockOnScreenOff(bool on) { if (!s_begun) touchPrefsBegin(); prefsPutUChar("lock_off", on ? 1 : 0); }
bool touchPrefsGetGlanceWhenLocked() { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("glance_lck", 0) != 0; }
void touchPrefsSetGlanceWhenLocked(bool on) { if (!s_begun) touchPrefsBegin(); prefsPutUChar("glance_lck", on ? 1 : 0); }
bool touchPrefsGetGlanceEnabled()   { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("glance_en", 1) != 0; }
void touchPrefsSetGlanceEnabled(bool on)    { if (!s_begun) touchPrefsBegin(); prefsPutUChar("glance_en", on ? 1 : 0); }

#if defined(HAS_TANMATSU)   // only the Tanmatsu has the message LED — keep S3 (T-Deck/V4) bins unchanged
bool touchPrefsGetMsgLed() { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("msg_led", 1) != 0; }   // default ON
void touchPrefsSetMsgLed(bool on) { if (!s_begun) touchPrefsBegin(); prefsPutUChar("msg_led", on ? 1 : 0); }
#endif

// Generic blob (used to persist the discovered-nodes list across reboots).
size_t touchPrefsGetBlob(const char* key, uint8_t* out, size_t maxlen) {
  if (!s_begun) touchPrefsBegin();
  if (!key || !out || !s_prefs.isKey(key)) return 0;
  size_t n = s_prefs.getBytes(key, out, maxlen);
  return (n > maxlen) ? 0 : n;
}
bool touchPrefsSetBlob(const char* key, const uint8_t* data, size_t len) {
  if (!key) return false;
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) { s_begun = s_prefs.begin(TOUCH_NS, true); return false; }
  bool ok;
  if (!data || len == 0) { s_prefs.remove(key); ok = true; }
  else                   ok = s_prefs.putBytes(key, data, len) > 0;
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}
uint8_t touchPrefsGetSoundVolume() {
  if (!s_begun) touchPrefsBegin();
#if defined(TLORA_PAGER)
  // The pager's ES8311 codec + NS4150B amp run noticeably louder at a given
  // percentage than the T-Deck's I2S amp/Tanmatsu's ES8156 — 70% clips into
  // uncomfortable territory, so this board gets a quieter first-boot default.
  uint8_t v = s_prefs.getUChar("snd_vol", 50);
#else
  uint8_t v = s_prefs.getUChar("snd_vol", 70);
#endif
  return v > 100 ? 100 : v;
}
void touchPrefsSetSoundVolume(uint8_t vol) { if (vol > 100) vol = 100; if (!s_begun) touchPrefsBegin(); prefsPutUChar("snd_vol", vol); }
bool touchPrefsGetDndEnabled() { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("dnd_en", 0) != 0; }
void touchPrefsSetDndEnabled(bool on) { if (!s_begun) touchPrefsBegin(); prefsPutUChar("dnd_en", on ? 1 : 0); }
uint8_t touchPrefsGetDndStartSlot() {
  if (!s_begun) touchPrefsBegin();
  uint8_t s = s_prefs.getUChar("dnd_ss", 44);
  return s > 47 ? 47 : s;
}
void touchPrefsSetDndStartSlot(uint8_t slot) { if (slot > 47) slot = 47; if (!s_begun) touchPrefsBegin(); prefsPutUChar("dnd_ss", slot); }
uint8_t touchPrefsGetDndEndSlot() {
  if (!s_begun) touchPrefsBegin();
  uint8_t s = s_prefs.getUChar("dnd_es", 12);
  return s > 47 ? 47 : s;
}
void touchPrefsSetDndEndSlot(uint8_t slot) { if (slot > 47) slot = 47; if (!s_begun) touchPrefsBegin(); prefsPutUChar("dnd_es", slot); }
uint8_t touchPrefsGetKbdBacklight() {
  if (!s_begun) touchPrefsBegin();
  uint8_t v = s_prefs.getUChar("kbd_bl", 100);
  return v > 100 ? 100 : v;
}
void touchPrefsSetKbdBacklight(uint8_t pct) { if (pct > 100) pct = 100; if (!s_begun) touchPrefsBegin(); prefsPutUChar("kbd_bl", pct); }

// Per-channel mute (name-keyed NVS blob "chm" + tiny RAM cache) --------------
static const char* KEY_CHM = "chm";
static const int   CHM_ENTRY = TOUCH_CHMUTE_NAME + 1;   // 32-byte name + 1 flag byte
// PSRAM-first (internal fallback), zero-initialized — keeps these keyed tables
// off the scarce internal SRAM (same pattern as UITask's psAlloc).
static void* tpPsAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  if (p) memset(p, 0, n);
  return p;
}
static uint8_t*    s_chm = (uint8_t*)tpPsAlloc(TOUCH_CHMUTE_MAX * CHM_ENTRY);
static int         s_chm_n = -1;   // -1 = not loaded yet
static void chmLoad() {
  if (s_chm_n >= 0) return;
  s_chm_n = 0;
  if (!s_begun) touchPrefsBegin();
  if (!s_prefs.isKey(KEY_CHM)) return;
  size_t n = s_prefs.getBytes(KEY_CHM, s_chm, (size_t)(TOUCH_CHMUTE_MAX * CHM_ENTRY));
  if (n == 0 || n > (size_t)(TOUCH_CHMUTE_MAX * CHM_ENTRY)) { s_chm_n = 0; return; }
  s_chm_n = (int)(n / CHM_ENTRY);
}
static int chmFind(const char* name) {
  chmLoad();
  for (int i = 0; i < s_chm_n; ++i)
    if (strncmp((const char*)&s_chm[i * CHM_ENTRY], name, TOUCH_CHMUTE_NAME) == 0) return i;
  return -1;
}
uint8_t touchPrefsGetChannelMute(const char* name) {
  if (!name || !name[0]) return 0;
  int i = chmFind(name);
  return i < 0 ? 0 : s_chm[i * CHM_ENTRY + TOUCH_CHMUTE_NAME];
}
void touchPrefsSetChannelMute(const char* name, uint8_t flags) {
  if (!name || !name[0]) return;
  chmLoad();
  int i = chmFind(name);
  if (flags == 0) {
    if (i < 0) return;
    for (int j = i; j + 1 < s_chm_n; ++j) memcpy(&s_chm[j * CHM_ENTRY], &s_chm[(j + 1) * CHM_ENTRY], CHM_ENTRY);
    --s_chm_n;
  } else {
    if (i < 0) {
      if (s_chm_n >= TOUCH_CHMUTE_MAX) return;   // cap reached
      i = s_chm_n++;
      memset(&s_chm[i * CHM_ENTRY], 0, CHM_ENTRY);
      strncpy((char*)&s_chm[i * CHM_ENTRY], name, TOUCH_CHMUTE_NAME - 1);
    }
    s_chm[i * CHM_ENTRY + TOUCH_CHMUTE_NAME] = flags;
  }
  s_prefs.end();
  if (s_prefs.begin(TOUCH_NS, false)) {
    if (s_chm_n == 0) s_prefs.remove(KEY_CHM);
    else              s_prefs.putBytes(KEY_CHM, s_chm, (size_t)(s_chm_n * CHM_ENTRY));
    s_prefs.end();
  }
  s_begun = s_prefs.begin(TOUCH_NS, true);
}

// ---- Per-channel avatar emoji (chat-list avatar override) ----
// Same keyed-blob pattern as the mute table above: 32-byte name + 16-byte UTF-8
// glyph (16 covers ZWJ sequences). No entry = auto (the two-letter avatar).
// 24 entries x 48 B = 1152 B, safely under the SdNvsPrefs 2048-byte blob cap.
static const int   CHE_NAME  = TOUCH_CHMUTE_NAME;
static const int   CHE_GLYPH = 16;
static const int   CHE_ENTRY = CHE_NAME + CHE_GLYPH;
static const int   CHE_MAX   = 24;
static const char* KEY_CHE   = "chemoji";
static uint8_t*    s_che = (uint8_t*)tpPsAlloc(CHE_MAX * CHE_ENTRY);
static int         s_che_n = -1;   // -1 = not loaded yet
static void cheLoad() {
  if (s_che_n >= 0) return;
  s_che_n = 0;
  if (!s_begun) touchPrefsBegin();
  if (!s_prefs.isKey(KEY_CHE)) return;
  size_t n = s_prefs.getBytes(KEY_CHE, s_che, (size_t)(CHE_MAX * CHE_ENTRY));
  if (n == 0 || n > (size_t)(CHE_MAX * CHE_ENTRY)) { s_che_n = 0; return; }
  s_che_n = (int)(n / CHE_ENTRY);
}
static int cheFind(const char* name) {
  cheLoad();
  for (int i = 0; i < s_che_n; ++i)
    if (strncmp((const char*)&s_che[i * CHE_ENTRY], name, CHE_NAME) == 0) return i;
  return -1;
}
bool touchPrefsGetChannelEmoji(const char* name, char* out, size_t cap) {
  if (out && cap) out[0] = '\0';
  if (!name || !name[0] || !out || cap == 0) return false;
  int i = cheFind(name);
  if (i < 0) return false;
  const char* g = (const char*)&s_che[i * CHE_ENTRY + CHE_NAME];
  size_t n = strnlen(g, CHE_GLYPH);
  if (n >= cap) n = cap - 1;
  memcpy(out, g, n);
  out[n] = '\0';
  return out[0] != '\0';
}
void touchPrefsSetChannelEmoji(const char* name, const char* utf8) {
  if (!name || !name[0]) return;
  cheLoad();
  int i = cheFind(name);
  if (!utf8 || !utf8[0]) {                       // clear -> back to auto letters
    if (i < 0) return;
    for (int j = i; j + 1 < s_che_n; ++j) memcpy(&s_che[j * CHE_ENTRY], &s_che[(j + 1) * CHE_ENTRY], CHE_ENTRY);
    --s_che_n;
  } else {
    if (i < 0) {
      if (s_che_n >= CHE_MAX) return;            // cap reached
      i = s_che_n++;
      memset(&s_che[i * CHE_ENTRY], 0, CHE_ENTRY);
      strncpy((char*)&s_che[i * CHE_ENTRY], name, CHE_NAME - 1);
    }
    memset(&s_che[i * CHE_ENTRY + CHE_NAME], 0, CHE_GLYPH);
    strncpy((char*)&s_che[i * CHE_ENTRY + CHE_NAME], utf8, CHE_GLYPH - 1);
  }
  s_prefs.end();
  if (s_prefs.begin(TOUCH_NS, false)) {
    if (s_che_n == 0) s_prefs.remove(KEY_CHE);
    else              s_prefs.putBytes(KEY_CHE, s_che, (size_t)(s_che_n * CHE_ENTRY));
    s_prefs.end();
  }
  s_begun = s_prefs.begin(TOUCH_NS, true);
}

// Remembered repeater admin passwords --------------------------------------
//
// Layout: single NVS blob "rpw" of up to TOUCH_REPEATER_PW_MAX records,
// each record = [6-byte pubkey prefix][16-byte null-terminated password].
// Empty/cleared records are removed (the blob is repacked) so reading the
// blob length tells you exactly how many entries exist.
static const char* KEY_RPW = "rpw";
constexpr int RPW_REC_BYTES = TOUCH_REPEATER_PW_KEY_LEN + TOUCH_REPEATER_PW_LEN;  // 6 + 16 = 22

static int rpwReadAll(uint8_t out[TOUCH_REPEATER_PW_MAX * RPW_REC_BYTES]) {
  if (!s_begun) touchPrefsBegin();
  if (!s_prefs.isKey(KEY_RPW)) return 0;   // absent on a fresh device — skip the [E] NOT_FOUND log
  size_t n = s_prefs.getBytes(KEY_RPW, out, TOUCH_REPEATER_PW_MAX * RPW_REC_BYTES);
  if (n == 0 || n > (size_t)(TOUCH_REPEATER_PW_MAX * RPW_REC_BYTES)) return 0;
  return (int)(n / RPW_REC_BYTES);
}

static bool rpwWriteAll(const uint8_t* buf, int count) {
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  bool ok;
  if (count <= 0) {
    s_prefs.remove(KEY_RPW);
    ok = true;
  } else {
    ok = s_prefs.putBytes(KEY_RPW, buf, (size_t)(count * RPW_REC_BYTES)) > 0;
  }
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

int touchPrefsGetRepeaterPassword(const uint8_t* pub_key6, char* out, int out_cap) {
  if (!out || out_cap <= 0) return 0;
  out[0] = '\0';
  if (!pub_key6) return 0;
  uint8_t buf[TOUCH_REPEATER_PW_MAX * RPW_REC_BYTES];
  int n = rpwReadAll(buf);
  for (int i = 0; i < n; ++i) {
    const uint8_t* rec = &buf[i * RPW_REC_BYTES];
    if (memcmp(rec, pub_key6, TOUCH_REPEATER_PW_KEY_LEN) == 0) {
      const char* pw = (const char*)(rec + TOUCH_REPEATER_PW_KEY_LEN);
      int plen = 0;
      while (plen < TOUCH_REPEATER_PW_LEN - 1 && pw[plen] != '\0') ++plen;
      if (plen > out_cap - 1) plen = out_cap - 1;
      memcpy(out, pw, (size_t)plen);
      out[plen] = '\0';
      return plen;
    }
  }
  return 0;
}

bool touchPrefsSetRepeaterPassword(const uint8_t* pub_key6, const char* password) {
  if (!pub_key6) return false;
  uint8_t buf[TOUCH_REPEATER_PW_MAX * RPW_REC_BYTES];
  int n = rpwReadAll(buf);
  int found = -1;
  for (int i = 0; i < n; ++i) {
    if (memcmp(&buf[i * RPW_REC_BYTES], pub_key6, TOUCH_REPEATER_PW_KEY_LEN) == 0) {
      found = i; break;
    }
  }
  // Treat null/empty password as a remove request — saves NVS bytes and
  // avoids confusing "remembered but empty" cases.
  bool remove = !password || password[0] == '\0';
  if (remove) {
    if (found < 0) return true;
    for (int i = found; i < n - 1; ++i) {
      memcpy(&buf[i * RPW_REC_BYTES], &buf[(i + 1) * RPW_REC_BYTES], RPW_REC_BYTES);
    }
    --n;
    return rpwWriteAll(buf, n);
  }
  // Add or overwrite. Cap reached → silently refuse.
  int slot = found;
  if (slot < 0) {
    if (n >= TOUCH_REPEATER_PW_MAX) return false;
    slot = n++;
  }
  uint8_t* rec = &buf[slot * RPW_REC_BYTES];
  memcpy(rec, pub_key6, TOUCH_REPEATER_PW_KEY_LEN);
  // Pad password slot with zeros, then copy up to PW_LEN-1 chars.
  memset(rec + TOUCH_REPEATER_PW_KEY_LEN, 0, TOUCH_REPEATER_PW_LEN);
  int plen = (int)strlen(password);
  if (plen > TOUCH_REPEATER_PW_LEN - 1) plen = TOUCH_REPEATER_PW_LEN - 1;
  memcpy(rec + TOUCH_REPEATER_PW_KEY_LEN, password, (size_t)plen);
  return rpwWriteAll(buf, n);
}

bool touchPrefsActivateWifiSlot(int idx) {
  char label[TOUCH_WIFI_LABEL_MAX];
  char ssid[WIFI_CONFIG_SSID_MAX];
  char pwd[WIFI_CONFIG_PWD_MAX];
  if (!touchPrefsGetWifiSlot(idx, label, sizeof(label),
                             ssid, sizeof(ssid), pwd, sizeof(pwd))) {
    return false;
  }
  if (ssid[0] == '\0') return false;   // refuse to activate an empty slot
  if (!wifiConfigSetSsid(ssid)) return false;
  if (!wifiConfigSetPwd(pwd))   return false;
  wifiConfigSetRadioEnabled(true);
  wifiConfigRequestApply();
  return true;
}

bool touchPrefsSetQuickReply(int idx, const char* text) {
  if (idx < 0 || idx >= TOUCH_QUICK_REPLY_COUNT) return false;
  if (!s_begun) touchPrefsBegin();
  // Open RW. Truncate to TOUCH_QUICK_REPLY_MAXLEN-1 to bound NVS usage.
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  char key[8];
  qrKeyFor(idx, key);
  char buf[TOUCH_QUICK_REPLY_MAXLEN];
  buf[0] = '\0';
  if (text) {
    int n = (int)strlen(text);
    if (n > TOUCH_QUICK_REPLY_MAXLEN - 1) n = TOUCH_QUICK_REPLY_MAXLEN - 1;
    memcpy(buf, text, (size_t)n);
    buf[n] = '\0';
  }
  bool ok = s_prefs.putString(key, buf) > 0 || (buf[0] == '\0');
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

static const char* KEY_SETUP_DONE = "setup_ok";

bool touchPrefsGetSetupDone() {
  if (!s_begun) touchPrefsBegin();
  return s_prefs.getBool(KEY_SETUP_DONE, false);
}

bool touchPrefsSetSetupDone(bool done) {
  if (!s_begun) touchPrefsBegin();
  s_prefs.end();
  if (!s_prefs.begin(TOUCH_NS, false)) return false;
  bool ok = s_prefs.putBool(KEY_SETUP_DONE, done);
  s_prefs.end();
  s_begun = s_prefs.begin(TOUCH_NS, true);
  return ok;
}

uint32_t touchPrefsGetGpsBaud(uint32_t fallback) {
  if (!s_begun) touchPrefsBegin();
  // 0 = never set -> caller's compile-time default. A real configured baud is
  // always non-zero, so this preserves the old "absent key -> fallback" result.
  return s_cfg.gps_baud != 0 ? s_cfg.gps_baud : fallback;
}

bool touchPrefsSetGpsBaud(uint32_t baud) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.gps_baud = baud;
  return cfgFlush();
}

#endif
