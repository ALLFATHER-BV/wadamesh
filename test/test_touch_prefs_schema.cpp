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
  c.console_monitor = 1;
  c.boot_wifi_time = 0;
  c.boot_wifi_open = 0;
  return c;
}

int main() {
  constexpr size_t v43_size = offsetof(Config, retry_echo);
  constexpr size_t suffix_offset = offsetof(Config, web_mirror);
  constexpr size_t v43_suffix_size = v43_size - suffix_offset;

  // The tail is append-only, so a blob written by an older firmware is always a
  // strict PREFIX of the current layout. Asserting an exact size delta pins the
  // test to one release and goes stale the moment the next field lands (it did:
  // this used to demand "v45 appends exactly one byte"). Assert the invariant
  // that actually matters instead — every historical blob is shorter, and the
  // fields it never stored keep their defaults.
  static_assert(v43_size < sizeof(Config), "v43 blobs must be a strict prefix of the current layout");

  // A v43 blob has the current layout truncated before the newly appended
  // retry byte. Every established field must survive byte-for-byte, and every
  // field appended since must fail safe to its default.
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
  v43.retry_echo = 0;        // outside the stored v43 extent
  v43.boot_wifi_time = 1;    // ditto — must NOT reach the migrated config
  v43.boot_wifi_open = 1;

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
  assert(migrated.boot_wifi_time == 0);
  assert(migrated.boot_wifi_open == 0);

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

  // #383 appended boot_wifi_time + boot_wifi_open at the tail. A v53 blob is the
  // current layout minus exactly those two bytes: everything before them must
  // survive, and both opt-ins must come back OFF rather than inheriting whatever
  // byte followed the blob — an inherited 1 would spend boot time on a Wi-Fi
  // session the user never asked for.
  constexpr size_t v53_size = offsetof(Config, boot_wifi_time);
  static_assert(v53_size + 2 == sizeof(Config), "v54 appends exactly the two #383 bytes");

  Config v53 = safeDefaults();
  v53.ver = 53;
  v53.bright = 42;
  v53.retry_echo = 0;
  v53.console_mode = 1;
  v53.console_monitor = 0;
  v53.kb_force_legacy = 1;
  v53.boot_wifi_time = 1;   // outside the stored v53 extent
  v53.boot_wifi_open = 1;

  migrated = safeDefaults();
  migrated.boot_wifi_time = 0;
  migrated.boot_wifi_open = 0;
  stored_version = 0;
  assert(TouchPrefsSchema::overlayStored(migrated, &v53, v53_size, &stored_version));
  assert(stored_version == 53);
  assert(migrated.bright == 42);
  assert(migrated.retry_echo == 0);
  assert(migrated.console_mode == 1);
  assert(migrated.console_monitor == 0);
  assert(migrated.kb_force_legacy == 1);
  assert(migrated.boot_wifi_time == 0);
  assert(migrated.boot_wifi_open == 0);

  // Once rewritten at the current version the whole structure is authoritative,
  // including an explicit retry opt-out, a deliberately enabled Remote Mode, and
  // both #383 opt-ins turned on.
  Config current = safeDefaults();
  current.remote_mode = 1;
  current.remote_landscape = 0;
  current.retry_echo = 0;
  current.boot_wifi_time = 1;
  current.boot_wifi_open = 1;
  migrated = safeDefaults();
  assert(TouchPrefsSchema::overlayStored(migrated, &current, sizeof(current), &stored_version));
  assert(stored_version == TouchPrefsSchema::CURRENT_VERSION);
  assert(memcmp(&migrated, &current, sizeof(current)) == 0);

  Config invalid = safeDefaults();
  uint8_t garbage[sizeof(Config)] = {};
  assert(!TouchPrefsSchema::overlayStored(invalid, garbage, sizeof(garbage)));
  assert(memcmp(&invalid, &defaults, sizeof(defaults)) == 0);
  return 0;
}
