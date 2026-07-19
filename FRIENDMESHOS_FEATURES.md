# FriendMeshOS features for WadaMesh

Status: T-Deck-only functional service clusters and first native WadaMesh integration implemented and build-verified; transmit disabled
Documented: 2026-07-18
Repository branch: `friendmeshOS-proposal`
Primary implementation target: LilyGo T-Deck / T-Deck Plus
Deferred hardware: Heltec targets are documented for later access but are not current build, implementation, or acceptance gates

## 1. Purpose and scope

This repository remains WadaMesh. FriendMeshOS is a set of optional features developed within the existing WadaMesh project—not a replacement product, repo migration, fork-wide rename, or takeover of WadaMesh's architecture and release system.

The feature direction is:

- add selected FriendMeshOS identity, trust, security, recovery, and social features to WadaMesh's mature MeshCore companion-radio runtime and LVGL interface;
- develop and validate the current feature phase only on the LilyGo T-Deck;
- avoid needless coupling that would obstruct later Heltec work, without treating Heltec as a current build or acceptance requirement;
- preserve the useful FriendMesh security, identity, recovery, and verification work from the earlier implementation without importing its Meshtastic-specific architecture wholesale;
- keep ordinary WadaMesh and MeshCore behavior working when FriendMeshOS features are disabled or unavailable;
- keep UI integration narrow, do not provision production storage yet, and do not enable FriendMesh protocol transmission or radio changes.

WadaMesh remains the host project's name, user experience, repository structure, upstream relationship, distribution identity, and default behavior. FriendMeshOS features should live behind explicit service, UI, build, and runtime boundaries. They must not silently rename existing WadaMesh concepts or redirect WadaMesh users into a separate product flow.

The current scope is **T-Deck only**. Only `LilyGo_TDeck_companion_radio_touch` is built and accepted during this phase. Heltec support remains a future direction, so new shared code should avoid gratuitous T-Deck hardware assumptions where practical, but no Heltec source changes, builds, flashes, or physical tests are required until the scope is explicitly expanded.

### 1.1 Non-negotiable UI and branding boundary

FriendMeshOS feature work does not redesign WadaMesh. Preserve:

- the WadaMesh name, wordmark, icons, terminology, boot experience, and release identity;
- the existing main tabs, command center, app drawer, navigation model, settings structure, chat presentation, contacts, map, status bar, and control center;
- the current touch, keyboard, trackball, and on-screen-keyboard behavior;
- the established responsive layouts for T-Deck and Heltec.

Expose FriendMeshOS behavior through the smallest appropriate additions to existing WadaMesh surfaces: a status line, badge, action, settings row, contact detail, chat metadata, confirmation modal, or map option. Do not create a replacement launcher, FriendMesh home screen, parallel navigation system, separate visual language, or repo-wide FriendMesh skin.

Optional additional themes are the maximum broad visual change in scope. New themes must use WadaMesh's existing theme mechanism, retain WadaMesh branding and layout, and remain optional. FriendMeshOS features must work with the existing WadaMesh themes and cannot require a special FriendMesh theme.

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

This dependency boundary is strategically important. FriendMeshOS backend services and minimal existing-UI integration can be added inside this repository, but any required changes below `BaseChatMesh`, transport primitives, identity storage hooks, radio behavior, or the companion protocol may require a coordinated update to the core tag. A green local app change is not proof that the external core remains replaceable or upstream-compatible.

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

This file is the largest development-risk surface. New FriendMesh features should be introduced behind small service/state classes, with only minimal hooks added to the existing LVGL screens. A UI rewrite or redesign is out of scope, and adding FriendMesh state, cryptography, persistence, or transport logic directly to `UITask.cpp` would repeat the coupling problem.

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

FriendMesh-specific trust, group, identity, verification, and safety semantics must be represented through minimal additions to these existing screens. The layout, navigation, visual language, and WadaMesh terminology remain authoritative. Existing MeshCore “contact,” “channel,” “identity,” and “delivered” concepts should not be relabeled as stronger FriendMesh guarantees unless the underlying protocol proves them.

## 6. T-Deck implementation model and deferred Heltec notes

The comparison below is retained as planning context for possible later Heltec access. It does not expand the current T-Deck-only implementation or verification scope.

### 6.1 Comparison

| Capability | LilyGo T-Deck / Plus | Heltec V4 + TFT | FriendMesh rule |
|---|---|---|---|
| MCU/radio | ESP32-S3, SX1262 | ESP32-S3, SX1262 + board FEM | shared mesh service; board radio hooks stay in variants |
| Flash/PSRAM | 16 MB flash, 8 MB PSRAM | 16 MB flash, 2 MB PSRAM on regular V4 | budget shared features for Heltec internal RAM; use optional richer T-Deck presentation |
| Display | 320x240 fixed landscape | 240x320, rotatable | reuse WadaMesh's responsive layouts; no new FriendMesh layout system |
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

For current T-Deck FriendMesh work:

1. Gate reusable behavior on a capability or runtime service result.
2. Use `LILYGO_TDECK`, `HAS_TDECK_*`, or `HELTEC_*` only for true board/controller quirks.
3. Design backend interfaces so a later hardware adapter is possible, but do not add speculative Heltec code during the T-Deck-only phase.
4. Keyboard shortcuts, trackball navigation, SD depth, WAV alerts, and lock-screen polish may enhance T-Deck without becoming backend protocol requirements.
5. Compile-time exclusion is acceptable for impossible hardware functions; degraded state must be visible and safe.
6. If regular V4 support is later approved, re-evaluate its tight internal-memory constraints instead of assuming T-Deck allocations transfer safely.
7. If Heltec enters scope later, re-establish its baseline then; do not claim compatibility from these planning notes.

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

Deferred reference only; do not build or modify these targets in the current phase.

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
  FriendMeshFeatureService.*   current feature state, lifecycle, and hard transmit gate
  status/                      future secret-free diagnostics and reason model
  protocol/                    MeshCore-facing encode/decode and policy
  security/                    signing identity and verification
  storage/                     protected records, key lifecycle, recovery
  ui/                          small LVGL-facing presenter/controller helpers
```

`UITask` should consume status snapshots and invoke bounded actions through existing WadaMesh surfaces. Cryptographic KDFs, filesystem recovery, imports, and large writes must run outside LVGL callbacks. `MyMesh` should call a narrow protocol/service interface at reviewed receive/transmit points rather than becoming the storage/UI implementation.

The subsystem should have an explicit disabled state. When disabled, it must not provision keys, mutate FriendMesh storage, change radio behavior, alter ordinary MeshCore messages, add required setup steps, or replace WadaMesh navigation and terminology.

### 7.5 Current T-Deck foundation

The first implementation slice is intentionally dormant:

- `FRIENDMESH_FEATURES=1` and `src/friendmesh/*.cpp` are enabled only in `LilyGo_TDeck_companion_radio_touch`;
- `FriendMeshFeatureService` starts after the ordinary MeshCore service and exposes a secret-free in-memory `StatusSnapshot`;
- the initial lifecycle is `NotConfigured` and every readiness field is false;
- `canTransmit()` is hard-coded false and there is no FriendMesh send/receive integration;
- startup performs no FriendMesh filesystem, SD, SPIFFS, NVS, identity, protocol, radio, network, or UI work;
- no WadaMesh UI, wording, navigation, theme, asset, or branding file changed.

Build evidence from 2026-07-18:

| T-Deck build | RAM | Flash | Result |
|---|---:|---:|---|
| Unmodified baseline at `95a9f05` | 89,372 bytes (27.3%) | 3,251,813 bytes (80.0%) | pass |
| Backend foundation | 89,380 bytes (27.3%) | 3,251,845 bytes (80.0%) | pass |
| Delta | +8 bytes | +32 bytes | no physical test performed |

### 7.6 Shared application-core foundation

The next slice establishes common code for the complete planned feature set instead of a single MVP path:

- bounded friends, identity references, groups, members, roles, aliases, and membership epochs;
- one event vocabulary and policy table covering trust, membership, chat, sync, location, navigation, markers, meetups, SOS, Help, and control;
- typed feature records for chat, reactions, sync, positions, navigation, markers, meetups, and incidents;
- provider interfaces for storage, security, transport, clock, location, and notifications;
- bounded event history and outbox states, including a deliberate distinction between `RelayedOrObserved` and `Delivered`;
- host checks in `scripts/test-friendmesh-core.sh`;
- no global maximum-sized domain/history/outbox allocation, UI changes, storage behavior, or radio integration.

The T-Deck build passes at 89,404 bytes RAM and 3,252,277 bytes flash: +24 bytes RAM and +432 bytes flash over the dormant-service baseline. FriendMesh transmit remains hard-disabled.

### 7.7 Functional people/groups/membership cluster

The next implemented cluster completes the non-cryptographic behavior shared by
friends and private groups:

- `people/FriendMeshTrustedContacts.*` provides a bounded FriendMesh repository
  over an existing-contact directory interface; it does not replace or copy
  WadaMesh's MeshCore contact store;
- `people/FriendMeshMembership.*` implements development identity lifecycle,
  invitation codes and sessions, verification transcripts, candidate approval,
  rejection/cancellation/expiry, and bounded capacity failures;
- invitations default to a direct-only policy requiring a fresh zero-hop
  observation with forwarding disabled; direct RF evidence is explicitly not
  represented as physical-proximity, identity, or confidentiality proof;
- group transactions cover aliases, roles, join order, blocking, leave/kick
  countdowns, identity replacement, administration transfer, majority
  succession with recorded/deduplicated approvals, and disbanding;
- epoch transactions revoke the excluded identity and require a functional grant
  for every remaining approved member before rekey completion;
- host simulations cover multiple groups, unauthorized actors, conflicts,
  expiry, all declared capacities, and the irreversible rekey boundary.

The provider references and grants in this phase are deliberately development
models, not proofs, keys, or production cryptography. Live `MyMesh` binding,
persistence, UI, protocol/radio integration, and physical verification remain
open. The T-Deck build passes at 89,404 bytes RAM and 3,253,533 bytes flash;
FriendMesh transmission remains hard-disabled.

The complete expected onboarding experience and production security boundary are
specified in `FRIENDMESH_NEARBY_JOIN_PROTOCOL.md`. The important rule is that
identity private keys never move between devices. Production onboarding delivers
only a new group epoch key, inside a transcript-bound, authenticated encrypted
grant addressed to the approved member.

### 7.8 Functional chat/history/synchronization cluster

Phase 3 implements the complete functional cluster behind development-only
providers:

- bounded chat messages, durable reply references, reactions, authorized
  deletion tombstones, unread/mute state, delayed receipt metadata, and explicit
  sender-clock trust;
- fixed-capacity payload storage with internal and expansion/SD-like placement,
  integrity checks, missing/read-only/corrupt/failing-media behavior, and
  incomplete-history state;
- fixed 192-byte fragmentation and bounded out-of-order reassembly;
- a durable development outbox with retry timing, cancellation, expiry, queue
  pressure, and distinct relayed-or-observed versus delivered states;
- reboot reconstruction from event and outbox journals;
- inventories, sender progress, missing ranges, bounded range/priority batches,
  receipts, deduplication, and conflict quarantine;
- host simulations for loss-shaped gaps, duplicates, reordering, offline delay,
  reboot, capacity exhaustion, and storage failure.

The host suite passes with UBSan and warnings-as-errors. The T-Deck build passes
at 89,404 bytes RAM and 3,254,685 bytes flash. No UI, production persistence,
cryptography, MeshCore radio integration, Heltec work, or physical test was
performed, and FriendMesh transmission remains hard-disabled.

### 7.9 First local T-Deck feature workspace

The T-Deck now allocates the development runtime in PSRAM and exposes one
`FriendMesh` tile in WadaMesh's existing app drawer. The page reuses WadaMesh
controls and branding and can initialize a local Friends workspace, save typed
notes through the real chat/outbox service, create a current-position meetup
marker, and open a local ride Help incident with notification state.

The meetup action is connected to WadaMesh's actual map renderer: active
FriendMesh markers appear as typed teal overlays, coexist with self/contact
markers, participate in the existing overlap picker, and show FriendMesh marker
details when tapped. `View map` centers the current map on the latest marker.
The page keeps fixed-height status fields and refreshes only when a local action
changes state. It has no background FriendMesh polling, avoids the previous
whole-label flex relayout, and defers map reconstruction until the map opens.

This is deliberately labeled local development behavior: it is volatile across
reboot, does not bind real MeshCore contacts, and cannot send FriendMesh traffic.
The host suite passes, and the optimized T-Deck image builds at 89,556 bytes RAM
and 3,267,661 bytes flash. The preceding workspace image was physically tested;
this updated marker-rendering image still requires a completed recovery flash
after the development T-Deck disconnected during upload.

### 7.10 Simple WadaMesh private-channel nearby join

The first live group-chat join flow reuses WadaMesh private channels and
MeshCore encrypted direct messages. A channel's existing action sheet offers
`Invite nearby` and performs a bounded three-second Bluetooth scan. Presence
advertisements contain only a versioned six-byte public-key prefix and are
matched only to existing saved chat contacts; results expire after 30 seconds.
A zero-hop LoRa advert no older than two minutes remains the fallback. The
sender forces an empty outbound route and sends a bounded `FMCH1`
channel-name/secret envelope through the existing pairwise encrypted message
path. A receiver processes the envelope only when the packet is direct with no
path hashes, then shows an explicit `Join` confirmation before saving the
channel. Invite payloads do not enter visible chat history.

The same channel action sheet now exposes `Members`. A fixed eight-member roster
is persisted through WadaMesh's existing bounded blob storage and represents
`Invited`, `Joined`, `Invite failed`, `Left`, and `Removed` states. Join
acknowledgements, leave notices, and cooperative removal notices reuse encrypted
MeshCore contact messages. Compact roster snapshots use a reserved encrypted
MeshCore group-data type and therefore never enter the channel-text pipeline;
the obsolete `FMRS1` text form is consumed by a compatibility firewall before
history, companion, MQTT, alerts, or UI delivery. Already-joined members are
removed from the invite picker and rejected again by the send operation.

This shares only the symmetric 16-byte channel secret required by MeshCore; it
never shares an identity private key. It is a narrow compatibility bridge, not
the planned FriendMesh membership protocol: there is not yet transcript
comparison, administrator-signed membership, replay persistence, protected
secret storage, per-member grants, or forward-only epoch rotation. Zero hops
and Bluetooth RSSI do not prove that two people are physically close. The static
public-key prefix is observable presence metadata, not authentication, and needs
a rotating privacy-preserving replacement during shared security work.

Member removal is cooperative functionality only. The updated recipient deletes
the local channel, but anyone retaining the symmetric channel secret can still
decrypt future traffic. Leave/removal therefore sets `rekey required`; production
member-event authentication and forward-only key rotation remain Phase 7 work.

The LoRa direct-join, Bluetooth discovery, roster synchronization, legacy
`FMRS1` suppression, and duplicate-member invite prevention have now passed on
the development T-Decks. The current source continues beyond that physically
tested image, so later feature sections report their own verification boundary.

### 7.11 Group map and Friend Compass

An explicit `FriendMeshMeshCorePositionAdapter` converts WadaMesh/MeshCore
contact coordinates from signed microdegrees into bounded FriendMesh E7
position records. It rejects missing `0,0` sentinels, zero identities, and
out-of-range coordinates. The temporary deterministic identity reference is a
functional bridge only; it is not a production signing-identity binding.

An existing private channel's action sheet now offers `Group map`. The map
collects at most eight joined roster members with saved chat contacts and valid
coordinates, then filters WadaMesh's existing map markers, links, and positioned
contact list without creating another map. The map option can turn the
group-only filter off. On open, a bounded queue requests each joined contact's
position through the existing encrypted MeshCore telemetry request/reply path,
strictly one request at a time with a 30-second per-contact ceiling. Replies are
persisted through the existing WadaMesh contact-position store and markers are
rebuilt only between contacts. The queue stops when the roster is exhausted or
the user leaves Map, so it is neither an advert loop nor background tracking.
Selecting a positioned member exposes `Friend Compass`,
which shows distance, absolute bearing, cardinal direction, position age,
source, and a prominent stale state after five minutes. T-Deck currently uses
north-up because it has no reviewed stationary heading source; the selected
contact alone is refreshed once per second.

The response authorization is local FriendMesh roster membership: a MeshCore
contact recorded as `Joined` in at least one channel can receive current GPS in
telemetry even when public `Share location in advert` is off. Leaving/removal
removes that authorization unless the same peer remains joined through another
local channel. This is the functional group-location policy, not the later
transcript-bound production identity/grant design.

Host adapter checks and the offline T-Deck build pass at 90,876 bytes RAM
(27.7%) and 3,291,257 bytes flash (81.0%). The bounded map-position cache is
allocated in PSRAM, the refresh queue adds 272 bytes of static RAM over the
previous map build, and the compass refresh reads only its selected contact. No
device was flashed for this automatic group-location addition, so physical
behavior remains open.

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

### 8.2 Focused build

Build the T-Deck reference target:

```bash
pio run -e LilyGo_TDeck_companion_radio_touch
```

Do not run Heltec or other board environments in the current phase. Use the explicit T-Deck environment: a plain `pio run` follows `default_envs` and would expand the agreed scope.

### 8.3 Build outputs

`merge-bin.py` copies the application image into `out/` and defines the `mergebin` target. A merged standalone image can be produced with:

```bash
pio run -t mergebin -e LilyGo_TDeck_companion_radio_touch
```

The merged image is useful for a full install. Normal update/app images and merged images are not interchangeable. The repository explicitly warns that a padded merged image can overwrite NVS when flashed incorrectly. Use the normal PlatformIO upload flow or the documented four-component flash chain when preserving device state.

No flash, erase, factory reset, reboot, storage migration, or hardware mutation should be performed without the operator's explicit approval and an exact target/port.

### 8.4 Verification ladder

Every current T-Deck FriendMeshOS feature milestone should report these separately:

1. Source review: source paths, capability gates, data flow, and threat boundary checked.
2. Host/unit checks: deterministic parsers, crypto framing, storage recovery, and policy tests.
3. T-Deck build: reference environment compiles and size is recorded.
4. T-Deck physical gate, only when explicitly approved: flash, input methods, storage mode, RF state, reboot/recovery, and UI evidence.
5. Deferred hardware gate: establish separate Heltec baselines and acceptance criteria only after that hardware enters scope.

“Build passes” is not “works on hardware.” A T-Deck pass makes no claim about Heltec. A previous Meshtastic FriendMesh test is not a MeshCore FriendMesh pass.

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
| `platformio.ini` | board environments, feature flags, dependencies | current T-Deck feature/build gate; deferred board definitions remain untouched |
| `boards/` | PlatformIO board definitions | flash/PSRAM/USB facts |
| `src/main.cpp` | application composition and cooperative loop | lifecycle and service wiring |
| `src/MyMesh.*` | MeshCore app, messages, companion protocol | protocol integration boundary |
| `src/AbstractUITask.h` | core-to-UI events | status/event seam |
| `src/DataStore.*` | MeshCore identity/prefs/contacts/channels | migration reference; not protected FriendMesh storage |
| `src/NodePrefs.h` | node/radio preferences | avoid mixing FriendMesh secrets into it |
| `src/ui-touch/UITask.*` | primary UI and most product behavior | minimal FriendMesh status/action hooks; no redesign |
| `src/ui-touch/device_caps.h` | compile-time UI capabilities | future UI integration; no current FriendMesh UI changes |
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

1. T-Deck is the only current implementation, build, and acceptance target.
2. Build only `LilyGo_TDeck_companion_radio_touch` until the user explicitly expands hardware scope.
3. Do not edit, build, flash, or claim compatibility for Heltec during the current phase.
4. Keep board quirks in variants or capability adapters.
5. Keep FriendMesh secrets out of logs, status exports, backups, companion broadcasts, MQTT, QR codes, and screenshots by default.
6. Do not equate MeshCore identity, BLE PIN, device lock, storage PIN, and FriendMesh signing identity.
7. Do not enable FriendMesh transmit as a side effect of UI/storage scaffolding.
8. Long crypto, network, filesystem, and migration work must not run inside LVGL callbacks or block the main loop.
9. Storage-root changes require explicit migration, verification, rollback behavior, and identity continuity checks.
10. Record build, host-test, and physical evidence independently.
11. Preserve the companion frame stream: ordinary debug text on the same USB serial channel can corrupt binary protocol traffic.
12. Preserve WadaMesh functionality while adding features. FriendMesh additions should not silently break or replace ordinary MeshCore chat, companion use, OTA, recovery, navigation, or branding.
13. Avoid broad refactors of `UITask.cpp`, `MyMesh`, or the external core during the first vertical slice. Create narrow seams where the slice needs them.
14. Any change to the pinned external core must update the tag deliberately and rebuild the T-Deck from a clean dependency resolution; wider board validation is deferred.
15. Do not introduce FriendMesh-specific navigation, a replacement home screen, a separate app shell, or a required visual skin. Optional WadaMesh themes are the maximum broad UI change.

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
| Hardware access | only the T-Deck is in current scope | reassess exact Heltec model and baseline only if scope expands later |
| Existing docs | some release/channel docs describe older completed state as future TODOs | reconcile separately; do not use stale checkboxes as live evidence |

## 13. Implementation sequence

This project follows feature-complete-first development rather than an MVP sequence:

1. shared application core for every feature family;
2. people, groups, invitations, and membership behavior together;
3. chat, reactions, outbox, history, and sync together;
4. positions, compass, map/navigation, markers, and meetups together;
5. SOS, Help, incidents, and notifications together;
6. complete minimal integration into existing WadaMesh surfaces;
7. shared production security implementation across all dependent features;
8. MeshCore radio integration, whole-system security fixes/hardening, and physical qualification.

Production identity cryptography, protected storage, signing/encryption, replay, grants, rekey, and recovery are implemented as one shared security cluster after functional feature behavior exists. Their architectural interfaces and the hard transmit gate exist from the beginning so the security pass strengthens the same system rather than rewriting it.

The detailed live phase tracker and completion evidence are in `FRIENDMESHOS_ROADMAP.md`. General FriendMesh radio transmission remains disabled until the shared security and protocol gates pass.

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

T-Deck physical result:
Storage backend exercised:
Companion/network surfaces exercised:

Secrets/redaction review:
Destructive actions performed, if any:

Known issues:
Next exact action:
```

## 15. Scope conclusion

WadaMesh already supplies a strong MeshCore product shell: high-quality on-device chat and contacts, a map, mature settings, multi-input navigation, companion interoperability, remote/web features, persistence, OTA, and a capable T-Deck target. FriendMeshOS can add value as a bounded feature set without replacing that product.

The main opportunity is reuse. The main danger is allowing an optional feature effort to redefine the host project or assuming the polished UI also solves FriendMesh's security and protocol requirements. The current path is to preserve WadaMesh as the working MeshCore product, add FriendMeshOS behind explicit boundaries, and make the T-Deck implementation safe and excellent before considering another device family.
