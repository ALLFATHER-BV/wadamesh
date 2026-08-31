// SPDX-License-Identifier: GPL-3.0-or-later

#include <assert.h>
#include <string.h>

#include "helpers/esp32/TouchPrefsSchema.h"

using TouchPrefsSchema::Config;

static Config safeDefaults() {
  Config c = {};
  c.magic = TouchPrefsSchema::MAGIC;
  c.ver = TouchPrefsSchema::CURRENT_VERSION;
  c.bright = 100;
  c.rx_queue = 1;
  c.web_mirror = 0;
  c.remote_mode = 0;
  c.remote_landscape = 1;
  c.web_terminal = 0;
  c.map_tile_debug = 0;
  c.hist_sync_after = 2;
  c.hist_per_chat = 250;
  c.p4_antenna = 0;
  c.retry_echo = 1;
  c.theme_mode = 0;
  return c;
}

int main() {
  constexpr size_t v43_size = offsetof(Config, retry_echo);
  constexpr size_t suffix_offset = offsetof(Config, web_mirror);
  constexpr size_t v43_suffix_size = v43_size - suffix_offset;
  static_assert(offsetof(Config, retry_echo) ==
                    offsetof(Config, p4_antenna) + sizeof(Config::p4_antenna),
                "v45 retry field moved");

  // A v43 blob has the current layout minus the newly appended retry byte.
  // Every established field must survive byte-for-byte and retry defaults on.
  Config v43 = safeDefaults();
  v43.ver = 43;
  v43.bright = 67;
  v43.rx_queue = 0;
  v43.web_mirror = 1;
  v43.remote_mode = 1;
  v43.remote_landscape = 0;
  v43.web_terminal = 1;
  v43.map_tile_debug = 1;
  v43.hist_sync_after = 7;
  v43.hist_per_chat = 0x1234;
  v43.p4_antenna = 0x56;
  v43.retry_echo = 0;   // outside the stored v43 extent

  Config migrated = safeDefaults();
  uint8_t stored_version = 0;
  assert(TouchPrefsSchema::overlayStored(migrated, &v43, v43_size, &stored_version));
  assert(stored_version == 43);
  assert(migrated.bright == 67);
  assert(migrated.rx_queue == 0);
  assert(migrated.web_mirror == 1);
  assert(migrated.remote_mode == 1);
  assert(migrated.remote_landscape == 0);
  assert(migrated.web_terminal == 1);
  assert(migrated.map_tile_debug == 1);
  assert(migrated.hist_sync_after == 7);
  assert(migrated.hist_per_chat == 0x1234);
  assert(migrated.p4_antenna == 0x56);
  assert(migrated.retry_echo == 1);

  // Reproduce beta 57 exactly: retry was inserted before the old suffix, then
  // the v43 bytes were copied and the full shifted v44 structure was written.
  uint8_t broken_v44[sizeof(Config)] = {};
  memcpy(broken_v44, &v43, suffix_offset);
  broken_v44[offsetof(Config, ver)] = TouchPrefsSchema::BROKEN_MID_INSERT_VERSION;
  broken_v44[suffix_offset] = 1;   // inserted retry_echo, overwriting old web_mirror
  memcpy(broken_v44 + suffix_offset + 1,
         reinterpret_cast<const uint8_t*>(&v43) + suffix_offset,
         v43_suffix_size);

  const Config defaults = safeDefaults();
  migrated = defaults;
  stored_version = 0;
  assert(TouchPrefsSchema::overlayStored(migrated, broken_v44,
                                         sizeof(broken_v44), &stored_version));
  assert(stored_version == 44);
  assert(migrated.bright == 67);       // unambiguous prefix is preserved
  assert(migrated.rx_queue == 0);
  assert(memcmp(reinterpret_cast<const uint8_t*>(&migrated) + suffix_offset,
                reinterpret_cast<const uint8_t*>(&defaults) + suffix_offset,
                sizeof(Config) - suffix_offset) == 0);  // ambiguous suffix fails safe

  // Once rewritten as v45, every field through retry_echo is authoritative;
  // fields appended by later versions retain their safe defaults.
  constexpr size_t v45_size = offsetof(Config, app_hide);
  Config v45 = safeDefaults();
  v45.ver = 45;
  v45.remote_mode = 1;
  v45.remote_landscape = 0;
  v45.retry_echo = 0;
  migrated = safeDefaults();
  assert(TouchPrefsSchema::overlayStored(migrated, &v45, v45_size, &stored_version));
  assert(stored_version == 45);
  assert(migrated.remote_mode == 1);
  assert(migrated.remote_landscape == 0);
  assert(migrated.retry_echo == 0);
  assert(migrated.app_hide == safeDefaults().app_hide);
  assert(migrated.theme_mode == 0);

  // v54 appends only theme_mode. A v53 blob preserves every older field and
  // leaves the new byte at the caller-provided Night default.
  constexpr size_t v53_size = offsetof(Config, theme_mode);
  static_assert(v53_size + sizeof(Config::theme_mode) == sizeof(Config),
                "v54 theme field must remain last");
  Config v53 = safeDefaults();
  v53.ver = 53;
  v53.kb_force_legacy = 1;
  v53.theme_mode = 1;   // outside the stored v53 extent
  migrated = safeDefaults();
  assert(TouchPrefsSchema::overlayStored(migrated, &v53, v53_size, &stored_version));
  assert(stored_version == 53);
  assert(migrated.kb_force_legacy == 1);
  assert(migrated.theme_mode == 0);

  Config invalid = safeDefaults();
  uint8_t garbage[sizeof(Config)] = {};
  assert(!TouchPrefsSchema::overlayStored(invalid, garbage, sizeof(garbage)));
  assert(memcmp(&invalid, &defaults, sizeof(defaults)) == 0);
  return 0;
}
