#include "TouchPrefsStore.h"

#if defined(ESP32)

#include "WifiRuntimeStore.h"

#include "SdNvsPrefs.h"   // NVS, or SD /meshcomod fallback when NVS is unusable (Launcher)

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
// keys too, because main.cpp reads (and for use_sd, writes) them directly via a
// raw Preferences open at boot, BEFORE the touch prefs load.
//
// On first run with the blob absent we read every legacy per-key into s_cfg, write
// "cfg" ONCE, and only after that durable write do we remove() the legacy keys to
// reclaim their entries. The write-before-remove ordering makes the migration
// crash-safe and idempotent (a power-cut before the write just re-migrates next
// boot; one after it finds "cfg" present and skips). `magic` rejects a garbage /
// short read (→ treat as absent → defaults); `ver` lets later builds add fields.
static const char* KEY_CFG = "cfg";
static const uint16_t TOUCH_CFG_MAGIC = 0x5743;   // 'WC' (WadaCfg)
static const uint8_t  TOUCH_CFG_VER   = 8;   // v2 sig_probe/poll; v3 tz_zone; v4 hide_node_name; v5 map_night/map_zoom; v6 map text/marker visibility; v7 app_grid_large; v8 sleep_idle

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

struct __attribute__((packed)) TouchCfg {
  uint16_t magic;            // TOUCH_CFG_MAGIC — rejects a garbage/short read
  uint8_t  ver;              // TOUCH_CFG_VER   — schema version
  uint8_t  bright;           // "bright"     1..100 (clamped 5..100 on get/set)
  uint8_t  kb_bl;            // "kb_bl"      0..2
  uint8_t  kb_layout;        // "kblang"
  uint8_t  kb_secondary;     // "kbsec"
  uint8_t  ui_lang;          // "ui_lang"
  uint8_t  ui_rotation;      // "uirot"      0..3
  uint8_t  dc_show;          // "dc_show"    bool
  uint8_t  use_miles;        // "use_miles"  bool
  uint8_t  tiles_from_sd;    // "tiles_sd"   bool
  uint8_t  clr_bubbles;      // "clr_bub"    bool
  uint8_t  kb_accent;        // "kb_accent"  bool
  int8_t   time_offs;        // "time_offs"  -23..23
  uint16_t scr_to_s;         // "scr_to_s"
  uint16_t kb_enabled;       // "kbenab"     bitmask
  uint16_t batt_full_mv;     // "battfull"
  uint32_t lock_color;       // "lk_col"     0xRRGGBB
  uint32_t accent;           // "accent"     0xRRGGBB
  uint32_t gps_baud;         // "gps_baud"   0 = unset -> caller fallback
  uint8_t  sig_probe_en;     // signal auto-discover probe on/off (bool) — v2
  uint16_t sig_poll_min;     // minutes between signal probes, 1..1440  — v2
  uint8_t  tz_zone;          // selected time-zone index (0 = Europe/CET)  — v3
  uint8_t  hide_node_name;   // hide the device name in the status bar + park clock left (bool) — v4
  uint8_t  map_night;        // invert map tile colours at render time (bool) — v5
  uint8_t  map_zoom;         // last map zoom level (0 = unset, auto-snap) — v5
  uint8_t  map_show_coords;  // show the coords read-out on the map (bool) — v6
  uint8_t  map_show_tilexyz; // show the zoom + tile z/x/y line on the map (bool) — v6
  uint8_t  map_show_contacts;// show contact markers on the map (bool) — v6
  uint8_t  app_grid_large;   // app drawer: large grid (one fewer column, bigger icons) — v7
  uint8_t  sleep_idle;       // idle light-sleep feature on/off (bool) — v8 (trailing after beta_9's v7 app_grid_large)
};

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
  c.sig_probe_en  = DEFAULT_SIG_PROBE_EN;
  c.sig_poll_min  = DEFAULT_SIG_POLL_MIN;
  c.tz_zone       = 0;          // 0 = Europe (CET/CEST) — preserves prior behaviour
  c.hide_node_name = 0;         // default: show the device name
  c.map_night     = 0;          // default: normal (light) tiles
  c.map_zoom      = 0;          // 0 = unset -> auto-snap on first map open
  c.map_show_coords   = 1;      // default: show coords / tile line / contacts
  c.map_show_tilexyz  = 1;
  c.map_show_contacts = 1;
  c.app_grid_large    = 0;      // default: compact app grid (T-Deck 4 cols / V4 3 cols)
  c.sleep_idle    = 0;          // default: idle light-sleep OFF
}

// Persist the whole blob using the same end()/begin(RW)/put/end()/begin(RO)
// discipline every setter in this file uses. Returns true on a durable write.
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
    TouchCfg tmp;
    memset(&tmp, 0, sizeof(tmp));
    size_t n = s_prefs.getBytes(KEY_CFG, &tmp, sizeof(tmp));
    // Need at least magic(2)+ver(1) to trust the header; reject anything shorter
    // (a half-written / garbage blob) and re-derive from legacy keys / defaults.
    if (n >= offsetof(TouchCfg, bright) && tmp.magic == TOUCH_CFG_MAGIC) {
      // Copy whatever was stored over the defaults (a shorter, older blob leaves
      // newer trailing fields at their default), then version-upgrade if needed.
      memcpy(&s_cfg, &tmp, n < sizeof(s_cfg) ? n : sizeof(s_cfg));
      if (s_cfg.ver < TOUCH_CFG_VER) {
        // v2->v3: a manual hour offset used to mean "CET base + offset". Preserve
        // that under the new zone picker by mapping such users onto the Custom
        // (UTC-offset) zone, so their clock doesn't jump to CET. 0xFE is resolved
        // to the real Custom index on the first touchPrefsGetTimezone() call (the
        // zone count isn't known here). offset 0 stays zone 0 (Europe) = unchanged.
        if (s_cfg.ver < 3 && s_cfg.time_offs != 0) s_cfg.tz_zone = 0xFE;
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

  // Write "cfg" ONCE. Only after a durable write do we reclaim the legacy keys.
  // If the write fails (e.g. NVS full / SD missing) we keep the legacy keys
  // intact and retry the whole migration on the next boot.
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
// The interval is entered in whole minutes; clamp >1 min (one flood a minute is
// already aggressive on shared airtime) .. 1 day so a bad/blank entry can't make
// the probe hammer the mesh or effectively never run.
static const uint16_t SIG_POLL_MIN_MINS = 1;   // 1 min = 60 s (the old fixed cadence)
static const uint16_t SIG_POLL_MAX_MINS = 1440;

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

bool touchPrefsGetMapNight() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.map_night != 0;
}
bool touchPrefsSetMapNight(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.map_night = on ? 1 : 0;
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
bool touchPrefsGetAppGridLarge() {
  if (!s_begun) touchPrefsBegin();
  return s_cfg.app_grid_large != 0;
}
bool touchPrefsSetAppGridLarge(bool on) {
  if (!s_begun) touchPrefsBegin();
  s_cfg.app_grid_large = on ? 1 : 0;
  return cfgFlush();
}

// Store ALL device data (identity, prefs, contacts, channels) on the SD card
// under /meshcomod instead of internal SPIFFS. Read at boot (main.cpp) BEFORE
// the data loads, so changing it needs a reboot. Key "use_sd" in the "touch"
// namespace — main.cpp reads the same key directly.
static const char* KEY_USE_SD_STORAGE = "use_sd";

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
  return ok;
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
bool touchPrefsGetScrollReverse()   { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("tb_rev", 0) != 0; }
void touchPrefsSetScrollReverse(bool on)   { if (!s_begun) touchPrefsBegin(); prefsPutUChar("tb_rev", on ? 1 : 0); }
bool touchPrefsGetLockOnScreenOff() { if (!s_begun) touchPrefsBegin(); return s_prefs.getUChar("lock_off", 0) != 0; }
void touchPrefsSetLockOnScreenOff(bool on) { if (!s_begun) touchPrefsBegin(); prefsPutUChar("lock_off", on ? 1 : 0); }

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
  uint8_t v = s_prefs.getUChar("snd_vol", 70);
  return v > 100 ? 100 : v;
}
void touchPrefsSetSoundVolume(uint8_t vol) { if (vol > 100) vol = 100; if (!s_begun) touchPrefsBegin(); prefsPutUChar("snd_vol", vol); }

// Per-channel mute (name-keyed NVS blob "chm" + tiny RAM cache) --------------
static const char* KEY_CHM = "chm";
static const int   CHM_ENTRY = TOUCH_CHMUTE_NAME + 1;   // 32-byte name + 1 flag byte
static uint8_t     s_chm[TOUCH_CHMUTE_MAX * CHM_ENTRY];
static int         s_chm_n = -1;   // -1 = not loaded yet
static void chmLoad() {
  if (s_chm_n >= 0) return;
  s_chm_n = 0;
  if (!s_begun) touchPrefsBegin();
  if (!s_prefs.isKey(KEY_CHM)) return;
  size_t n = s_prefs.getBytes(KEY_CHM, s_chm, sizeof(s_chm));
  if (n == 0 || n > sizeof(s_chm)) { s_chm_n = 0; return; }
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
