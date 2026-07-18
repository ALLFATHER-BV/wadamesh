# FriendMeshOS features for WadaMesh

Status: optional feature-layer documentation; no firmware behavior changed  
Documented: 2026-07-18  
Repository snapshot: branch `friendmeshOS-proposal`, commit `3fbc3e3`  
Primary implementation target: LilyGo T-Deck / T-Deck Plus  
Compatibility target: Heltec WiFi LoRa 32 V4 + TFT, including deliberate awareness of V4-R8

## 1. Purpose and scope

This repository remains WadaMesh. FriendMeshOS is a set of optional features developed within the existing WadaMesh project—not a replacement product, repo migration, fork-wide rename, or takeover of WadaMesh's architecture and release system.

The feature direction is:

- add selected FriendMeshOS identity, trust, security, recovery, and social features to WadaMesh's mature MeshCore companion-radio runtime and LVGL interface;
- develop and validate those features on the LilyGo T-Deck first;
- keep shared FriendMeshOS features accessible to the Heltec V4 family through capability-based code and board adapters;
- preserve the useful FriendMesh security, identity, recovery, and verification work from the earlier implementation without importing its Meshtastic-specific architecture wholesale;
- keep ordinary WadaMesh and MeshCore behavior working when FriendMeshOS features are disabled or unavailable;
- treat this first pass as documentation only. Protocol work, security porting, feature code, and firmware builds come later.

WadaMesh remains the host project's name, user experience, repository structure, upstream relationship, distribution identity, and default behavior. FriendMeshOS features should live behind explicit service, UI, build, and runtime boundaries. They must not silently rename existing WadaMesh concepts or redirect WadaMesh users into a separate product flow.

“T-Deck first” means the T-Deck is the reference experience and first hardware gate for FriendMeshOS features. It does **not** mean hard-coding those features to the T-Deck. New shared features should compile with the T-Deck and Heltec V4 environments, expose a safe reduced experience where hardware differs, and reserve direct board checks for real hardware quirks.

## 2. What this repository is

WadaMesh is an ESP32/ESP-IDF application layer around MeshCore. It provides a full on-device interface, MeshCore companion protocol support, persistence, radio/board glue, remote access, and release infrastructure.

The repository owns:

- the companion-radio application lifecycle in `src/main.cpp`;
- the local MeshCore application subclass and companion command handling in `src/MyMesh.*`;
- the persistent application data adapter in `src/DataStore.*`;
- the LVGL application in `src/ui-touch/`;
- ESP32 transport, preference, web, and OTA helpers in `src/helpers/`;
- device-specific targets in `variants/`;
- PlatformIO board environments in `platformio.ini`;
- ESP-IDF builds for the P4 devices in `tanmatsu/` and `tdisplay_p4/`;
- web flasher, website, firmware feeds, map-tile infrastructure, and release tooling in `deploy/` and `scripts/`.

The repository does **not** vendor its MeshCore implementation. Every PlatformIO touch environment currently resolves:

```ini
https://github.com/ALLFATHER-BV/meshcomod.git#core-v1.16.5
```

That tag is a source-only MeshCore/meshcomod core snapshot. `MC_VENDORED_TOUCH_APP` prevents app-owned touch helpers from also compiling out of the dependency.

This dependency boundary is strategically important. FriendMeshOS UI and services can be added inside this repository, but any required changes below `BaseChatMesh`, transport primitives, identity storage hooks, radio behavior, or the companion protocol may require a coordinated update to the core tag. A green local app change is not proof that the external core remains replaceable or upstream-compatible.

## 3. Licensing boundary

WadaMesh is GPL-3.0-or-later. The MeshCore-derived dependency remains MIT and individual derived files retain their original notices.

For FriendMeshOS feature work this means:

- distributed WadaMesh builds containing FriendMeshOS features must satisfy the GPL source-availability requirements;
- new application/UI work in this repository should use the repository's GPL SPDX convention;
- changes intended for the MeshCore dependency should remain separable and use the core repository's license;
- third-party libraries and assets must be reflected in `NOTICE` when required;
- code should not be copied from the old Meshtastic-based FriendMeshOS checkout without reviewing its license and architectural fit.

## 4. System architecture

```text
User input and screen
  T-Deck: touch + keyboard + trackball
  Heltec: touch + on-screen keyboard
                  |
                  v
          src/ui-touch/UITask
       screens, settings, apps, history
                  |
        AbstractUITask event seam
                  |
                  v
            src/MyMesh
  BaseChatMesh subclass + companion commands
                  |
                  v
   external MeshCore/meshcomod core-v1.16.5
   packet model, routing, crypto, radio helpers
                  |
                  v
       target.cpp + board variant
     SX1262, RTC, sensors, display, power

Parallel services:
  DataStore/SdNvsPrefs <-> SPIFFS, SD, NVS
  MultiTransportCompanionInterface <-> USB, TCP, WebSocket, BLE
  MqttBridge <-> MQTT broker
  WebMirror/WebSocket server <-> browser remote UI and terminal
  update/tile clients <-> WadaMesh distribution infrastructure
```

### 4.1 Boot and runtime flow

`src/main.cpp` is the composition root.

1. Static target objects provide the board, radio wrapper, RTC, sensors, and display.
2. On ESP32, the large `MyMesh` and multi-transport objects are allocated in PSRAM when possible to preserve internal RAM for Wi-Fi/BLE and DMA.
3. `setup()` initializes the board, display, radio, filesystem/storage backend, `DataStore`, and `MyMesh`.
4. Wi-Fi configuration and the USB/TCP/WebSocket/BLE companion interface are prepared.
5. Sensors and `UITask` start after the core mesh state is ready.
6. `loop()` services the UI first, then Wi-Fi/TCP/WebSocket, MeshCore, MQTT, sensors, RTC, OTA, and the touch sleep gate.
7. The spectrum analyzer temporarily owns the radio; the normal mesh loop is intentionally skipped while a spectrum sweep is active.

The loop is cooperative. Blocking filesystem, network, audio, radio, or rendering work can freeze both the UI and mesh processing. Existing code contains worker tasks, watchdog guards, deferred saves, PSRAM buffers, and stall tracing because this has been a recurring field risk.

### 4.2 Core application: `MyMesh`

`MyMesh` extends `BaseChatMesh` and implements `DataStoreHost`. It owns or coordinates:

- local MeshCore identity and node preferences;
- contacts, channels, paths, discovery, adverts, ACKs, and message dispatch;
- companion command frames and protocol responses;
- direct messages, channel messages, room/server messages, telemetry, traces, and admin replies;
- automatic contact-add rules and contact-table persistence;
- settings backup import/export;
- the local `Meshcomod` command contact and terminal command execution;
- UI event delivery through `AbstractUITask`.

The companion protocol reports `FIRMWARE_VER_CODE 27`, preserving the meshcomod protocol lineage rather than stock upstream protocol 13. Client compatibility must be treated as a contract if this value or any frame handlers change.

`AbstractUITask` is a useful core-to-UI event seam, but the opposite direction is not fully abstracted: `UITask.cpp` also calls the global `the_mesh` directly in many places. FriendMesh work should not assume this is a clean service boundary. Prefer adding narrow, explicit FriendMesh service interfaces rather than increasing the number of unrelated global calls.

### 4.3 UI application: `UITask`

`src/ui-touch/UITask.cpp` is approximately 44,000 lines and currently contains most product behavior in one translation unit. It includes:

- LVGL display/input initialization and layout scaling;
- bottom-tab navigation and hardware focus navigation;
- home command center and app drawer;
- chat inbox, direct/channel thread views, composer, emoji, mentions, quick replies, delivery state, and message metadata;
- contacts, discovered contacts, favorites, telemetry, trace/ping, repeater administration, and contact actions;
- map tiles, contact markers, routing overlays, online/offline sources, and map options;
- setup wizard and all settings pages;
- control center, status bar, notification behavior, lock screen, sleep behavior, and sound;
- file browser, backups, terminal, RF monitor, spectrum analyzer, remote UI, browser screen mirror, on-device text browser, and Snake;
- history storage, map storage, background workers, diagnostics, and documentation screenshot capture.

This file is the largest development-risk surface. New FriendMesh features should be introduced behind small service/state classes where practical, with the LVGL code acting as a view/controller. A large up-front rewrite is not required, but adding all FriendMesh state, cryptography, persistence, and transport logic directly to `UITask.cpp` would repeat the coupling problem.

### 4.4 Persistence

There are three persistence concepts:

1. `DataStore` stores MeshCore identity, node preferences, contacts, channels, and opaque blobs.
2. `TouchPrefsStore` and `SdNvsPrefs` store application/UI preferences, with file-backed storage used to reduce NVS pressure and support Launcher/SD operation.
3. `UITask` owns additional UI state such as chat/thread history, discovered contacts, map data, wallpapers, audio files, and backups.

Important current files include:

- `/identity/_main.id`: the MeshCore local identity;
- `/new_prefs` and recovery `/new_prefs.tmp`: node/radio preferences;
- `/contacts3`: contacts;
- `/channels2`: channels;
- `/adv_blobs` or blob files: advertised data storage;
- `/prefs/<namespace>.kv` on internal storage;
- `/meshcomod/...` on SD when the full store is redirected.

On the T-Deck, SPIFFS is the safe fallback and the microSD card can hold the full store under `/meshcomod`. Startup contains migration and adoption rules designed to avoid silently creating a new identity when a storage root changes. Contacts/channels can also be routed to SD to avoid long SPIFFS garbage-collection stalls.

The T-Deck partition table provides:

| Region | Size | Purpose |
|---|---:|---|
| NVS | 20 KiB | low-level configuration |
| OTA data | 8 KiB | active OTA slot selection |
| app0 | 3.875 MiB | first firmware slot |
| app1 | 3.875 MiB | second firmware slot |
| tiles | 4.75 MiB | LittleFS map tile cache |
| SPIFFS | 3.375 MiB | application/user data |
| coredump | 64 KiB | crash data |

Current WadaMesh persistence is not a substitute for FriendMesh's protected-record design. The prior FriendMeshOS work introduced a separate storage PIN, wrapped master key, authenticated record framing, two-slot recovery, fixed internal targets, a bounded transaction journal, protected Ed25519 identity persistence, explicit creation/unlock/lock flows, and fail-closed status. Those concepts should be ported as a reviewed subsystem. Do not place FriendMesh signing seeds or other FriendMesh secrets directly into `_main.id`, generic UI preferences, backups, MQTT payloads, diagnostics, or unprotected SD files.

### 4.5 Connectivity and remote surfaces

The shared ESP32 build uses `MultiTransportCompanionInterface`:

- USB serial companion frames;
- TCP companion server, default port 5000;
- WebSocket companion server, default port 8765;
- NimBLE companion transport with a configurable six-digit pairing code;
- response broadcasting so multiple clients can observe updates.

Additional network surfaces include:

- Wi-Fi configuration and runtime reconnect logic;
- local browser control/terminal and display mirroring;
- on-device text browsing on boards with enough internal heap;
- MQTT bridge with optional payload encryption derived from its configured PSK;
- map-tile downloads;
- firmware version checks and HTTP OTA.

These are all FriendMesh threat-model inputs. “Local network” is not an authorization policy. Before FriendMesh secrets or privileged actions are exposed through USB, BLE, TCP, WebSocket, web UI, terminal, MQTT, QR export, or backup JSON, each surface needs an explicit allow/deny decision and authentication rule.

## 5. Existing product surface

### 5.1 Main tabs

- Chats
- Contacts
- Home / command center
- Map
- Settings
- optional Sensors tab on supported expansion hardware

### 5.2 Home apps

The app drawer currently routes to Chats, Contacts, Map, Mentions, Advertise, VNC/browser mirror, Remote browser app, Web reader, Signal, RF Monitor, Spectrum, Settings, Terminal, Files, Snake, and Power. Availability is capability-gated.

### 5.3 Settings categories

The settings grid currently includes:

- Profile
- Radio & Mesh
- Auto-add
- Wi-Fi
- Bluetooth
- GPS
- Clock & time
- Battery
- Sensors where supported
- Display
- Keyboard where supported
- Sound where supported
- Quick replies
- Lock screen where supported
- General and storage
- Backups and factory reset
- Language
- MQTT bridge
- About, updates, live system information, and diagnostics

### 5.4 Messaging and mesh functions

The current application already provides a large part of the product shell FriendMesh needs:

- direct and channel chat with unread state and delivery indicators;
- room/server messages and author attribution;
- persistent chat history, including deeper SD-backed history on T-Deck;
- contact discovery, favorites, blocking, manual add, QR sharing, and auto-add policies;
- adverts, path information, per-message RF metadata, signal probes, trace results, repeater administration, and telemetry polling;
- radio preset and manual LoRa configuration;
- official MeshCore companion-app interoperability.

FriendMesh-specific trust, group, identity, verification, and safety semantics must be layered deliberately onto these screens. Existing MeshCore “contact,” “channel,” “identity,” and “delivered” concepts should not be relabeled as stronger FriendMesh guarantees unless the underlying protocol proves them.

## 6. T-Deck-first, Heltec-aware hardware model

### 6.1 Comparison

| Capability | LilyGo T-Deck / Plus | Heltec V4 + TFT | FriendMesh rule |
|---|---|---|---|
| MCU/radio | ESP32-S3, SX1262 | ESP32-S3, SX1262 + board FEM | shared mesh service; board radio hooks stay in variants |
| Flash/PSRAM | 16 MB flash, 8 MB PSRAM | 16 MB flash, 2 MB PSRAM on regular V4 | budget shared features for Heltec internal RAM; use optional richer T-Deck presentation |
| Display | 320x240 fixed landscape | 240x320, rotatable | responsive LVGL layout; no fixed T-Deck coordinates in shared features |
| Touch | GT911 | CHSC6x | use pointer capability, not touch-controller type |
| Physical keyboard | yes | no | every required action needs an on-screen path |
| Trackball/focus nav | yes | no | useful enhancement, never the sole route |
| microSD | yes | no on regular V4; yes on V4-R8 expansion | internal-storage fallback must remain valid |
| GPS | T-Deck Plus hardware; build supports GPS | supported through V4 wiring/expansion | report unavailable/no-fix honestly; do not assume a receiver exists |
| Sensors | GPS only in current env | expansion environmental sensors enabled | FriendMesh core cannot require local sensors |
| Sound | I2S speaker, tones and WAV files | expansion piezo buzzer | notification semantics shared; implementation and controls vary |
| Lock screen | supported | currently not exposed | security cannot depend on lock-screen availability |
| On-device web reader | supported | disabled on regular 2 MB V4 due TLS heap; V4-R8 eligible | treat as optional application capability |
| Companion transports | USB/TCP/WS/BLE | USB/TCP/WS/BLE | shared policy and protocol tests on both |
| OTA | dual-slot | dual-slot | maintain per-environment image and recovery validation |
| USB mode | hardware CDC configuration | TinyUSB CDC retained because HW CDC lost larger companion frames | preserve board-specific USB configuration |
| Radio power/FEM | 22 dBm configured, DIO2 antenna switch | 10 dBm default, 22 dBm max, GC1109/KCT8103L control | never copy RF settings or FEM behavior between boards blindly |

### 6.2 Capability rules

`src/ui-touch/device_caps.h` is the intended UI capability registry. Every `CAP_*` value is always defined to `0` or `1`, so call sites must use `#if CAP_NAME`, not `#if defined(CAP_NAME)`.

For new FriendMesh work:

1. Gate reusable behavior on a capability or runtime service result.
2. Use `LILYGO_TDECK`, `HAS_TDECK_*`, or `HELTEC_*` only for true board/controller quirks.
3. Required FriendMesh operations must work with touch and an on-screen keyboard on Heltec.
4. Keyboard shortcuts, trackball navigation, SD depth, WAV alerts, and lock-screen polish may enhance T-Deck without becoming protocol requirements.
5. Compile-time exclusion is acceptable for impossible hardware functions; degraded state must be visible and safe.
6. The regular V4 is the tight-memory compatibility floor. PSRAM allocations do not remove Wi-Fi/BLE/DMA internal-heap constraints.
7. V4-R8 is not interchangeable with regular V4. It has 8 MB octal PSRAM, different pins, LovyanGFX, shared TFT/SD SPI, and hardware validation items still called out in `platformio.ini`.

### 6.3 T-Deck implementation anchors

- Build environment: `LilyGo_TDeck_companion_radio_touch`
- Board definition: `boards/t-deck.json`
- PlatformIO flags: `platformio.ini`
- Target composition: `variants/lilygo_tdeck/target.*`
- Board power/battery behavior: `variants/lilygo_tdeck/TDeckBoard.*`
- Touch: `src/helpers/input/TDeckTouch.cpp`
- Keyboard: `src/helpers/input/TDeckKeyboard.*`
- Trackball: `src/helpers/input/TDeckTrackball.*`
- Partition table: `variants/lilygo_tdeck/partitions_tdeck_touch.csv`

### 6.4 Heltec implementation anchors

- Build environment: `heltec_v4_tft_companion_radio_usb_tcp_touch`
- V4-R8 environment: `heltec_v4_r8_tft_companion_radio_usb_tcp_touch`
- Board definitions: `boards/heltec_v4.json`, `boards/heltec_v4_r8.json`
- Target composition: `variants/heltec_v4/target.*`
- Board power, battery, and sleep behavior: `variants/heltec_v4/HeltecV4Board.*`
- FEM and V4.3 LNA behavior: `variants/heltec_v4/LoRaFEMControl.*`
- Display: shared ST7789 on V4; `variants/heltec_v4/LGFXDisplay.*` on V4-R8
- Touch: `src/helpers/input/HeltecV4CapTouch.*`

## 7. FriendMeshOS feature boundary

The previous FriendMeshOS checkout remains a design/evidence source, not the codebase to merge wholesale into this repository. Port only the bounded behavior needed for an approved WadaMesh feature.

### 7.1 Preserve as product requirements

- explicit FriendMesh identity lifecycle;
- protected at-rest signing identity and future replay/group records;
- independent storage PIN rather than reusing a display-lock PIN or BLE PIN;
- read-only startup probe and no automatic secret provisioning;
- authenticated record framing and fail-closed corruption handling;
- power-loss-aware two-slot/journal recovery;
- secret-free diagnostic states;
- explicit distinction between implemented, host/build verified, and physically verified behavior;
- hard transmit disable until identity, migration, recovery, replay, and user-consent gates are satisfied;
- no silent plaintext fallback when protected storage is unavailable.

### 7.2 Re-evaluate for MeshCore

The following were designed around Meshtastic and must be re-derived against WadaMesh/MeshCore:

- packet types, routing, channel/group semantics, ACK meaning, and replay handling;
- signing/encryption placement in the transmit and receive pipelines;
- contact/public-key representations and migration mapping;
- durable outbox integration;
- module lifecycle and scheduling;
- device binding source and stability across WadaMesh OTA/Launcher modes;
- interaction between MeshCore's existing local identity and a FriendMesh signing identity;
- companion protocol, web UI, backup, MQTT, and terminal exposure;
- firmware/version/diagnostic presentation.

### 7.3 Do not carry forward automatically

- Meshtastic module APIs, protobufs, Router/NodeDB assumptions, or `meshtastic-device-ui` view classes;
- Meshtastic build environments and test harness paths;
- a claim that previously passed device evidence proves the new MeshCore implementation;
- version numbers or release labels from the old codebase;
- UI code that depends on the previous six-theme T-Deck layout.

### 7.4 Suggested optional feature subsystem

Keep the first implementation small and explicit:

```text
src/friendmesh/
  FriendMeshService.*          optional feature state and lifecycle
  FriendMeshStatus.*           secret-free status snapshot
  protocol/                    MeshCore-facing encode/decode and policy
  security/                    signing identity and verification
  storage/                     protected records, key lifecycle, recovery
  ui/                          small LVGL-facing presenter/controller helpers
```

`UITask` should consume status snapshots and invoke bounded actions. Cryptographic KDFs, filesystem recovery, imports, and large writes must run outside LVGL callbacks. `MyMesh` should call a narrow protocol/service interface at reviewed receive/transmit points rather than becoming the storage/UI implementation.

The subsystem should have an explicit disabled state. When disabled, it must not provision keys, mutate FriendMesh storage, change radio behavior, alter ordinary MeshCore messages, add required setup steps, or replace WadaMesh navigation and terminology.

## 8. Build and development workflow

### 8.1 Prerequisites

- PlatformIO Core with Python support;
- Git and network access for the pinned `lib_deps` sources;
- a USB data cable and explicit serial port for hardware work;
- enough disk space for `.pio` dependencies and build outputs.

The local PlatformIO binary may be outside `PATH`. A known installation on this workstation is:

```bash
/Users/tylersmith/.platformio/penv/bin/pio
```

### 8.2 Focused builds

Build the T-Deck reference target:

```bash
pio run -e LilyGo_TDeck_companion_radio_touch
```

Build the regular Heltec compatibility target:

```bash
pio run -e heltec_v4_tft_companion_radio_usb_tcp_touch
```

Build the V4-R8 variant when work touches the Heltec family, shared SPI/SD, display, or capability logic:

```bash
pio run -e heltec_v4_r8_tft_companion_radio_usb_tcp_touch
```

Use explicit environments during FriendMesh development. A plain `pio run` follows `default_envs`, which currently includes regular Heltec V4, T-Deck, and ThinkNode M9—not only the two FriendMesh focus boards.

### 8.3 Build outputs

`merge-bin.py` copies the application image into `out/` and defines the `mergebin` target. A merged standalone image can be produced with:

```bash
pio run -t mergebin -e LilyGo_TDeck_companion_radio_touch
```

The merged image is useful for a full install. Normal update/app images and merged images are not interchangeable. The repository explicitly warns that a padded merged image can overwrite NVS when flashed incorrectly. Use the normal PlatformIO upload flow or the documented four-component flash chain when preserving device state.

No flash, erase, factory reset, reboot, storage migration, or hardware mutation should be performed without the operator's explicit approval and an exact target/port.

### 8.4 Verification ladder

Every FriendMeshOS feature milestone should report these separately:

1. Source review: source paths, capability gates, data flow, and threat boundary checked.
2. Host/unit checks: deterministic parsers, crypto framing, storage recovery, and policy tests.
3. T-Deck build: reference environment compiles and size is recorded.
4. Heltec build: regular V4 compiles and memory impact is recorded.
5. V4-R8 build where applicable.
6. T-Deck physical gate: flash, input methods, storage mode, RF state, reboot/recovery, and UI evidence.
7. Heltec physical gate: touch/on-screen keyboard, rotation, low-memory behavior, expansion absence/presence, USB companion frames, and RF/FEM behavior.

“Build passes” is not “works on hardware.” A T-Deck physical pass is not a Heltec pass. A previous Meshtastic FriendMesh test is not a MeshCore FriendMesh pass.

### 8.5 Current automated support

- `.github/workflows/release.yml` builds release firmware, but it is a publishing workflow rather than a comprehensive PR test suite.
- `scripts/test_companion_serial.py` exercises the companion framing/commands against a serial device.
- release scripts build multiple board targets and generate manifests.
- no repository-wide native unit-test framework is documented for the application layer.

FriendMeshOS feature work should add host-runnable tests for security/storage/protocol code before it is wired to transmit. Hardware tests should remain separate and serial-port exclusive.

## 9. Release and distribution model

The current project uses immutable `beta_N` identifiers with two channels:

- beta/test builds are compiled from active development and staged under `releases/BETA/<tag>` plus a rolling test feed;
- stable promotion copies the already-tested binary into `releases/TOUCH/<tag>` and the stable feed without rebuilding it.

Distribution includes:

- GitHub releases;
- a web flasher and standalone merged images;
- application images for Launcher on T-Deck;
- on-device update checks and OTA for supported standalone builds;
- firmware manifests/catalogs;
- Tanmatsu app-store publishing through a separate path;
- WadaMesh website and documentation.

FriendMeshOS features do not imply a WadaMesh rebrand. WadaMesh versioning, package names, update endpoints, and promotion policy remain authoritative. Before an experimental build containing FriendMeshOS features is distributed, decide whether it belongs in a private development channel or an explicitly labeled WadaMesh test build. Do not point experimental feature builds at public stable metadata by accident.

## 10. Repository map

| Path | Role | FriendMesh relevance |
|---|---|---|
| `platformio.ini` | board environments, feature flags, dependencies | both focus builds and capability inputs |
| `boards/` | PlatformIO board definitions | flash/PSRAM/USB facts |
| `src/main.cpp` | application composition and cooperative loop | lifecycle and service wiring |
| `src/MyMesh.*` | MeshCore app, messages, companion protocol | protocol integration boundary |
| `src/AbstractUITask.h` | core-to-UI events | status/event seam |
| `src/DataStore.*` | MeshCore identity/prefs/contacts/channels | migration reference; not protected FriendMesh storage |
| `src/NodePrefs.h` | node/radio preferences | avoid mixing FriendMesh secrets into it |
| `src/ui-touch/UITask.*` | primary UI and most product behavior | FriendMesh screens and actions |
| `src/ui-touch/device_caps.h` | compile-time UI capabilities | T-Deck/Heltec portability policy |
| `src/ui-touch/i18n.*` | translations | new user-visible FriendMesh copy |
| `src/helpers/input/` | touch, keyboard, trackball, encoder adapters | multi-input acceptance |
| `src/helpers/esp32/` | prefs, Wi-Fi, transports, web, MQTT | security and exposure review |
| `variants/lilygo_tdeck/` | T-Deck hardware | reference hardware |
| `variants/heltec_v4/` | V4/V4-R8 hardware | compatibility hardware |
| `include/lv_conf.h` | LVGL configuration | memory/render behavior |
| `lib/ed25519/` | bundled Ed25519 implementation | review before reuse; do not assume it matches prior FriendMesh requirements |
| `scripts/` | builds, release, docs, device probe | development and distribution |
| `.github/workflows/` | release automation | future build gates |
| `deploy/` | site, flasher, feeds, nginx, catalogs | remains WadaMesh-owned; feature builds need an explicit channel decision |
| `release-notes/` | cumulative user-facing changes | WadaMesh history, not FriendMesh verification evidence |
| `tanmatsu/`, `tdisplay_p4/` | ESP-IDF P4 builds | out of current FriendMesh focus; preserve shared compile hygiene |

## 11. Development guardrails

1. T-Deck is the reference hardware; regular Heltec V4 is the compatibility floor.
2. Build both focus environments for shared source changes.
3. Never require a physical keyboard, trackball, microSD, speaker, lock screen, large PSRAM, or on-device web browser for a required FriendMeshOS feature operation.
4. Keep board quirks in variants or capability adapters.
5. Keep FriendMesh secrets out of logs, status exports, backups, companion broadcasts, MQTT, QR codes, and screenshots by default.
6. Do not equate MeshCore identity, BLE PIN, device lock, storage PIN, and FriendMesh signing identity.
7. Do not enable FriendMesh transmit as a side effect of UI/storage scaffolding.
8. Long crypto, network, filesystem, and migration work must not run inside LVGL callbacks or block the main loop.
9. Storage-root changes require explicit migration, verification, rollback behavior, and identity continuity checks.
10. Record build, host-test, and physical evidence independently.
11. Preserve the companion frame stream: ordinary debug text on the same USB serial channel can corrupt binary protocol traffic.
12. Preserve WadaMesh functionality while adding features. FriendMesh additions should not silently break or replace ordinary MeshCore chat, companion use, OTA, recovery, navigation, branding, or Heltec boot.
13. Avoid broad refactors of `UITask.cpp`, `MyMesh`, or the external core during the first vertical slice. Create narrow seams where the slice needs them.
14. Any change to the pinned external core must update the tag deliberately and rebuild both focus environments from a clean dependency resolution.

## 12. Known risks and open decisions

| Area | Current fact | Decision or evidence needed |
|---|---|---|
| MeshCore fork | app pins meshcomod `core-v1.16.5` | decide long-term FriendMesh core ownership and upstream strategy |
| Identity | MeshCore `_main.id` and prior FriendMesh signing identity are different concepts | define relationship, migration, display, backup, and rotation policy |
| Protected storage | prior design exists in Meshtastic checkout only | port and re-verify against SPIFFS/SD/Launcher/OTA here |
| Replay/group state | incomplete in prior implementation | define MeshCore wire rules before transmit |
| UI coupling | `UITask.cpp` is monolithic and calls `the_mesh` directly | add narrow FriendMesh service/presenter seams |
| Heltec memory | regular V4 has 2 MB PSRAM and tight internal heap | set build budgets and perform Wi-Fi/BLE/TLS coexistence tests |
| Heltec storage | regular V4 has no SD | define protected internal essential records and capacity behavior |
| T-Deck storage | SD is optional and may be absent/fail | preserve SPIFFS fallback without plaintext-secret fallback |
| Backup/export | current JSON can include identity/private key | decide whether and how FriendMesh material is excluded or separately protected |
| Remote surfaces | USB/TCP/WS/BLE/web/MQTT coexist | define authorization and secret-redaction matrix |
| Update channel | current public WadaMesh feeds are live product infrastructure | isolate experimental FriendMeshOS feature builds until approved for a WadaMesh test release |
| Product identity | code, site, local command contact, assets, and feeds are WadaMesh/Meshcomod | preserve them; FriendMeshOS is a feature name, not a repo-wide rename |
| Hardware access | T-Deck is available as the first target; Heltec access must remain possible | record which Heltec model(s) are physically available before claiming device proof |
| Existing docs | some release/channel docs describe older completed state as future TODOs | reconcile separately; do not use stale checkboxes as live evidence |

## 13. Recommended first implementation sequence

This is a starting sequence, not an authorization to implement it in this documentation pass.

### Phase A: freeze the baseline

- build the unmodified T-Deck and regular Heltec V4 environments;
- record firmware sizes and dependency resolution;
- physically smoke-test the current T-Deck WadaMesh build if approved;
- record available Heltec hardware and exact revision;
- create a small FriendMesh status shell with transmit hard-disabled.

### Phase B: port protected storage without protocol transmission

- move the previously reviewed storage primitives into a platform-neutral FriendMesh subsystem;
- add host tests for framing, KDF/key slots, device binding, journal recovery, identity creation, tamper rejection, and secret scanning;
- implement T-Deck UI actions through background workers;
- provide an on-screen-only equivalent flow for Heltec;
- test internal storage first, then optional T-Deck SD provider/migration;
- keep FriendMesh transmit disabled.

### Phase C: define the MeshCore FriendMesh wire contract

- document the packet format, signature scope, encryption relationship, replay state, group semantics, routing behavior, ACK meaning, size/airtime bounds, and downgrade behavior;
- add deterministic host vectors and parser fuzz/error cases;
- review exact `MyMesh` send/receive hook points;
- keep the implementation receive-disabled or test-vector-only until the contract is approved.

### Phase D: first vertical slice

- explicit unlock and identity readiness;
- one bounded FriendMesh contact/trust view;
- one signed local object or loopback/test-vector operation;
- status/diagnostics with no secrets;
- T-Deck input matrix and reboot recovery;
- regular Heltec build plus touch/on-screen-keyboard flow;
- no general transmit until replay, migration, recovery, and consent gates pass.

### Phase E: controlled radio gate

- operator-approved test channel and peers;
- bounded send rate and clear RF state;
- durable replay/outbox behavior;
- T-Deck physical evidence followed by Heltec physical evidence;
- ordinary MeshCore regression tests;
- release-channel isolation and rollback plan.

## 14. Session handoff template

Use this at the end of future FriendMesh sessions:

```text
Date:
Branch / commit:
Scope:
Files changed:

Behavior implemented:
Behavior intentionally disabled:

Host tests:
T-Deck build result and size:
Heltec V4 build result and size:
V4-R8 build result if relevant:

T-Deck physical result:
Heltec physical result:
Storage backend exercised:
Companion/network surfaces exercised:

Secrets/redaction review:
Destructive actions performed, if any:

Known issues:
Next exact action:
```

## 15. Scope conclusion

WadaMesh already supplies a strong MeshCore product shell: high-quality on-device chat and contacts, a map, mature settings, multi-input navigation, companion interoperability, remote/web features, persistence, OTA, board abstractions, and active T-Deck and Heltec targets. FriendMeshOS can add value as a bounded feature set without replacing that product.

The main opportunity is reuse. The main danger is allowing an optional feature effort to redefine the host project or assuming the polished UI also solves FriendMesh's security and protocol requirements. The correct path is to preserve WadaMesh as the working MeshCore product, add FriendMeshOS behind explicit boundaries, make the T-Deck feature experience excellent, and continuously prove that required flows remain accessible and safe on the regular Heltec V4.
