# Feature-removal matrix

## Recommendation

Start with a **Mesh Essentials** T-Deck profile that keeps messaging, contacts,
channels, GPS position, SD persistence, the WadaMesh identity, and at least USB
companion access. Strip optional presentation and network extras first. This
preserves the device's central purpose while making each footprint reduction
measurable and reversible.

The values below are not additive. Features share HTTP, TLS, Wi-Fi, LVGL,
filesystem, and crypto code. Only before/after builds can establish a true
subsystem saving.

## Ranked candidates

| Priority | Feature family | Candidate action | Expected value | User-visible cost | Important coupling |
|---|---|---|---|---|---|
| 1 | Default lock wallpaper | Stop embedding the 320x240 RGB565 wallpaper; use a solid/generated background or optional SD asset | High-confidence 153,600-byte asset removal | Default lock screen becomes simpler | Keep lock/unlock behavior; do not remove user-selected SD wallpaper support unless separately approved |
| 1 | Extra languages and fallback fonts | Ship English-only or a smaller language pack; omit unused glyph ranges | High; multiple linked glyph tables plus a 37,804-byte translation table are visible | No translated UI and reduced non-Latin rendering | Keyboard layouts, emoji, and fallback fonts are connected but should have separate gates |
| 1 | Color emoji pack/picker | Replace with text symbols or a small curated set | Medium/high | Fewer expressive glyphs and channel-avatar choices | `emoji_data.c`, LVGL image-font setup, chat composer, channel icon picker |
| 1 | Snake game | Compile out the game and Apps entry | Low/medium, low product risk | No Snake app | `SnakeGame.cpp` and UI navigation hooks |
| 1 | MQTT bridge | Compile out settings, runtime bridge, and `PubSubClient` dependency if nothing else uses it | Medium; exact linked delta requires A/B build | No MQTT forwarding | Wi-Fi remains required by other features unless separately removed |
| 2 | On-device web reader | Remove page reader, fetch task, HTML rendering, and “Open in web” action | Potentially high | Links can only be copied/shared as QR | Shares HTTP/TLS with update checks, OTA, and map tile download |
| 2 | Online map tile engine | Keep simple coordinates/compass, or remove the Map tab entirely | Potentially high | No slippy map, tile cache, or “show on map” | GPS, Friend Compass, group map, HTTP/TLS, LittleFS `tiles` partition, and SD tile packs must be split deliberately |
| 2 | Rich diagnostics | Remove battery charts, LoS plot, crash-export UI, and nonessential device diagnostics | Medium | Less on-device troubleshooting | Preserve minimum recovery/error reporting and decide whether the coredump partition remains useful |
| 2 | File manager/media extras | Remove general browsing, image preview, WAV picker/playback, and optional exports | Medium | SD remains a data backend but not a general file browser | Backup import/export and SD recovery must not disappear accidentally |
| 2 | Web terminal and browser-hosted mirror | Remove embedded terminal/mirror pages while retaining protocol transports if desired | At least the visible 30,442-byte terminal page plus code; actual delta requires A/B build | No browser terminal/mirror UI | WebSocket companion may still be retained for external clients |
| 2 | Browser-based OTA UI | Remove AsyncElegantOTA's embedded web page while retaining signed/release URL OTA if feasible | Visible OTA HTML is 53,715 bytes; shared server delta may be larger | No generic browser upload page | Do not remove the only recoverable update path; test the retained update route on hardware |
| 3 | BLE companion transport | Offer a USB + Wi-Fi profile without NimBLE | Potentially large flash/RAM saving | No BLE setup or phone companion link | FriendMesh BLE presence currently uses NimBLE too; removing transport alone may not eliminate the stack |
| 3 | Wi-Fi, TCP, and WebSocket companion | Offer a USB + BLE offline profile | Potentially very large flash/RAM saving | No Wi-Fi companion, network config, online reader/maps, MQTT, NTP, or HTTP OTA | A product-level decision; it can eliminate AsyncTCP, web server, HTTP, and TLS only after all consumers are gated |
| 3 | QR generation/share | Remove QR widgets and share flows | Low/medium | Manual key exchange becomes harder | Contact sharing and some offline workflows depend on it |
| 3 | Private-key import/export | Disable `ENABLE_PRIVATE_KEY_IMPORT` and `ENABLE_PRIVATE_KEY_EXPORT` | Likely low alone; improves attack surface | No identity transfer through those commands | Backup/recovery policy must be decided first |
| 4 | FriendMesh development families | Gate unfinished BLE presence, safety, group coordination, navigation, and development storage independently | Variable | Removes the corresponding FriendMesh experiments | Do not use one monolithic switch if the goal is to retain a smaller FriendMesh core |
| 4 | GPS | Build without MicroNMEA and location UI | Medium | No local position, maps, telemetry location, or Friend Compass | Core messaging can survive, but location/navigation cannot |
| 4 | Dual-slot OTA layout | Move to one app slot and reclaim the second slot as data space | Reclaims 3.875 MiB of address space, but does **not** shrink `firmware.bin` | No safe A/B self-update | High-risk partition migration; USB recovery and data-preservation procedure required |

## Assets confirmed in the current linker output

These are individual linked symbols, not full-feature totals:

| Symbol/asset | Bytes | Likely owner |
|---|---:|---|
| `_ZL27lockscreen_wallpaper_rgb565` | 153,600 | default lock wallpaper |
| `_ZL12ELEGANT_HTML` | 53,715 | AsyncElegantOTA page |
| `_ZL5kI18n` | 37,804 | UI translations |
| `WADAMESH_MARK_RGB565` | 30,184 | WadaMesh mark |
| `_ZL21WS_HTML_TERMINAL_PAGE` | 30,442 | WebSocket terminal page |
| `_ZL17WS_HTTP_INFO_PAGE` | 7,418 | WebSocket information page |

The WadaMesh mark is measurable but is not recommended for removal because the
project identity is an explicit guardrail. Optimizing its encoding or loading
it from SD can be considered later if the visual result and missing-card boot
experience remain acceptable.

## Changes that probably will not save much app flash by themselves

- Deleting disabled T-Deck sensor libraries from `lib_deps`: the T-Deck flags
  already set all environmental sensors except GPS to zero, and linker garbage
  collection generally keeps their unused code out of the final image. Clean up
  the dependency list for build time and clarity only after measuring it.
- Reducing the `tiles` or `spiffs` partition sizes: this changes available data
  capacity but does not reduce the application binary.
- Moving a file from internal flash to SD without removing its embedded source:
  the binary shrinks only when the built-in bytes are no longer linked.
- Hiding a menu row at runtime: the underlying code remains linked. A compile-time
  gate must cover the UI, service, assets, and unique dependencies.

## Suggested centralized gates

Names are proposed, not implemented:

```text
WADAMESH_FEATURE_LOCK_WALLPAPER_BUILTIN
WADAMESH_FEATURE_I18N
WADAMESH_FEATURE_COLOR_EMOJI
WADAMESH_FEATURE_SNAKE
WADAMESH_FEATURE_MQTT
WADAMESH_FEATURE_WEB_READER
WADAMESH_FEATURE_MAP_TILES
WADAMESH_FEATURE_DIAGNOSTICS_RICH
WADAMESH_FEATURE_FILE_MANAGER
WADAMESH_FEATURE_WEB_TERMINAL
WADAMESH_FEATURE_WEB_OTA_UPLOAD
WADAMESH_FEATURE_BLE_COMPANION
WADAMESH_FEATURE_WIFI_COMPANION
WADAMESH_FEATURE_QR
WADAMESH_FEATURE_GPS
```

Each should default to the current behavior so other environments do not change
silently. A later T-Deck slim environment can override only the desired gates.
