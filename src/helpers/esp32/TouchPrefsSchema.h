// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace TouchPrefsSchema {

static constexpr uint16_t MAGIC = 0x5743;   // 'WC' (WadaCfg)
static constexpr uint8_t CURRENT_VERSION = 53;   // v49: fem_lna default flip on the V4-R8; v50: map tile z/x/y line off by default (no new fields); v51: console_mode (boot into the text console; new trailing field, default OFF); v52: console_monitor (show incoming messages in the console, default ON)
static constexpr uint8_t BROKEN_MID_INSERT_VERSION = 44;

// Persisted byte layout. New fields must be appended at the end: older blobs
// are overlaid on a default-initialised current Config during migration.
struct __attribute__((packed)) Config {
  uint16_t magic;
  uint8_t  ver;
  uint8_t  bright;
  uint8_t  kb_bl;
  uint8_t  kb_layout;
  uint8_t  kb_secondary;
  uint8_t  ui_lang;
  uint8_t  ui_rotation;
  uint8_t  dc_show;
  uint8_t  use_miles;
  uint8_t  tiles_from_sd;
  uint8_t  clr_bubbles;
  uint8_t  kb_accent;
  int8_t   time_offs;
  uint16_t scr_to_s;
  uint16_t kb_enabled;
  uint16_t batt_full_mv;
  uint32_t lock_color;
  uint32_t accent;
  uint32_t gps_baud;
  uint8_t  sig_probe_en;
  uint16_t sig_poll_min;
  uint8_t  tz_zone;
  uint8_t  hide_node_name;
  uint8_t  map_night;
  uint8_t  map_zoom;
  uint8_t  map_show_coords;
  uint8_t  map_show_tilexyz;
  uint8_t  map_show_contacts;
  uint8_t  app_grid_large;
  uint8_t  ui_scale;
  uint8_t  kbd_nav;
  uint8_t  sleep_idle;
  uint8_t  nav_keys[5];
  uint8_t  map_zoom_buttons;
  uint8_t  nav_dir_keys[6];
  uint8_t  home_is_drawer;
  uint8_t  nav_scroll_keys[2];
  uint8_t  notify_new_contact;
  uint8_t  show_sensors_tab;
  uint8_t  map_show_links;
  uint8_t  map_style;
  uint8_t  tb_nav;
  uint8_t  scope_direct;
  uint8_t  fem_lna;
  uint8_t  msg_flash;
  uint8_t  flood_adv_hrs;
  uint16_t local_adv_min;
  uint8_t  beta_updates;
  uint8_t  boot_advert;
  uint8_t  compact_chat;
  uint32_t clock_floor;
  uint8_t  rx_queue;
  uint8_t  web_mirror;
  uint8_t  remote_mode;
  uint8_t  remote_landscape;
  uint8_t  web_terminal;
  uint8_t  map_tile_debug;
  uint8_t  hist_sync_after;
  uint16_t hist_per_chat;
  uint8_t  p4_antenna;
  uint8_t  retry_echo;       // v45: actual trailing location; v44 inserted it before web_mirror
  uint32_t app_hide;         // v46: app-drawer hide bitmask (Store page "built-in" toggles)
  char     lang_file[12];    // v48: file-language code ("" = built-in ui_lang; file at <data>/lang/<code>.lang)
  // NEW FIELDS GO HERE, AT THE TAIL. v44 inserted one mid-struct and shifted
  // every field after it, which is what v45 had to undo. Append, never insert.
  uint8_t  console_mode;     // v51: boot into the LVGL-free text console (CONSOLE_MODE.md)
  uint8_t  console_monitor;  // v52: console mode shows incoming messages as they arrive
  // v53: never probe for the raw keyboard protocol, always use the older one.
  // The detection cannot be certain while nobody is typing, and when it guesses
  // wrong the keyboard types nonsense -- which also makes it very hard to reach
  // a setting to fix it. This is reachable by touch, so it is a way out that
  // does not need the keyboard that is broken (#341, #351).
  uint8_t  kb_force_legacy;
};

static constexpr size_t HEADER_SIZE = offsetof(Config, bright);
static constexpr size_t STABLE_V44_PREFIX_SIZE = offsetof(Config, web_mirror);

static_assert(offsetof(Config, web_mirror) == offsetof(Config, rx_queue) + sizeof(Config::rx_queue),
              "the pre-v44 suffix moved");
static_assert(offsetof(Config, kb_force_legacy) + sizeof(Config::kb_force_legacy) == sizeof(Config),
              "new preference fields must remain trailing");
// lang_file must stay immediately before the tail, or a v48-era blob overlays
// onto the wrong bytes. Checking both ends means the next person to append is
// told at compile time instead of shipping another v44.
static_assert(offsetof(Config, console_mode) ==
                  offsetof(Config, lang_file) + sizeof(Config::lang_file),
              "console_mode must follow lang_file");
static_assert(offsetof(Config, console_monitor) ==
                  offsetof(Config, console_mode) + sizeof(Config::console_mode),
              "console_monitor must follow console_mode");

// Overlay a persisted blob on caller-provided defaults. Beta 57 wrote v44 with
// retry_echo inserted before web_mirror, shifting every later value. That blob
// cannot be distinguished from a fresh v44 blob, and the original web_mirror
// byte was overwritten during migration, so preserve only the unambiguous
// prefix through rx_queue and leave the whole suffix at safe defaults.
inline bool overlayStored(Config& current, const void* blob, size_t size,
                          uint8_t* stored_version = nullptr) {
  if (!blob || size < HEADER_SIZE) return false;
  const uint8_t* bytes = static_cast<const uint8_t*>(blob);
  uint16_t magic = 0;
  memcpy(&magic, bytes, sizeof(magic));
  if (magic != MAGIC) return false;

  const uint8_t version = bytes[offsetof(Config, ver)];
  if (stored_version) *stored_version = version;

  size_t copy_size = size < sizeof(Config) ? size : sizeof(Config);
  if (version == BROKEN_MID_INSERT_VERSION && copy_size > STABLE_V44_PREFIX_SIZE)
    copy_size = STABLE_V44_PREFIX_SIZE;
  memcpy(&current, blob, copy_size);
  return true;
}

}  // namespace TouchPrefsSchema
