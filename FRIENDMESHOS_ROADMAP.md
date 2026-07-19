# FriendMeshOS features for WadaMesh — roadmap

Status: active T-Deck-only, feature-complete-first roadmap
Updated: 2026-07-19
Repository: `Atlessc/wadamesh`
Working branch: `friendmeshOS-proposal`
Host/base: WadaMesh with MeshCore/meshcomod `core-v1.16.5`
Build target: `LilyGo_TDeck_companion_radio_touch`

## What this project is

This repository is **WadaMesh**. FriendMeshOS features are an optional friends, trusted-groups, navigation, recovery, and peer-assistance layer built inside WadaMesh. They do not replace the repository, firmware identity, UI, branding, navigation, releases, or ordinary MeshCore behavior.

WadaMesh supplies the mature product shell: LVGL interface, chat, contacts, history, maps, GPS, T-Deck touch/keyboard/trackball, companion transports, Wi-Fi/BLE/MQTT/web, OTA/recovery, SD/SPIFFS, screenshots, and field diagnostics. FriendMesh adds the missing social domain, shared events, coordination behavior, and later security implementation behind narrow services.

## Non-negotiable boundaries

- [x] WadaMesh remains the host project, UI, branding, terminology, and release identity.
- [x] Current implementation/build scope is T-Deck only.
- [x] Heltec editing, builds, flashes, and compatibility claims are deferred.
- [x] FriendMesh uses minimal additions to existing WadaMesh surfaces.
- [x] Optional themes are the maximum broad visual addition; no special theme is required.
- [x] Ordinary WadaMesh/MeshCore behavior remains unchanged when FriendMesh is disabled.
- [x] General FriendMesh event transmission remains hard-disabled; the only
  live exception is the explicit, bounded private-channel invite carried by
  MeshCore's existing encrypted direct-message protocol.
- [x] No development-only security provider or unprotected state may reach production radio paths.
- [x] No automatic secret provisioning or silent plaintext fallback.

## Implementation strategy

This is not an MVP roadmap. We are building the planned feature system, then performing the dedicated security and reliability pass across the complete system.

Rules:

1. Build reusable feature/domain code once, not one isolated demo at a time.
2. Implement features that depend on the same event, storage, location, notification, or UI code together.
3. Implement security-dependent features against explicit provider interfaces and development/test identities first.
4. Implement shared production security together: identity, verification, protected storage, signing, encryption, replay, key grants, rekey, and recovery.
5. Keep real radio transmission disabled until that shared security pass and its gates succeed.
6. Complete functional behavior in host/local simulation before security hardening and on-air qualification.
7. Do not postpone structural security boundaries; postpone the production crypto/hardening implementation.

## Sources of truth

1. [`README.md`](README.md) — WadaMesh introduction/build entrypoint.
2. [`FRIENDMESHOS_FEATURES.md`](FRIENDMESHOS_FEATURES.md) — repository survey and integration boundary.
3. [`FRIENDMESH_FULL_IMPLEMENTATION_GAME_PLAN.md`](FRIENDMESH_FULL_IMPLEMENTATION_GAME_PLAN.md) — detailed product/security/test contracts.
4. This roadmap — current order and completion state.
5. Live source/build output — authority for current implementation facts.

## Verified baselines

### Clean WadaMesh baseline

| Evidence | Result |
|---|---|
| Commit | `95a9f05` |
| Environment | `LilyGo_TDeck_companion_radio_touch` |
| Build | pass |
| RAM | 89,372 / 327,680 bytes (27.3%) |
| Flash | 3,251,813 / 4,063,232 bytes (80.0%) |
| Physical/Heltec work | not run |

### Dormant FriendMesh service baseline

Commit `fef6ff2` established the new starting base:

- `src/friendmesh/FriendMeshFeatureService.*`;
- T-Deck-only `FRIENDMESH_FEATURES=1` and source filter;
- secret-free in-memory status;
- every readiness gate false;
- `canTransmit()` hard-coded false;
- no FriendMesh UI, storage, identity, protocol, network, or radio work.

| Evidence | Result |
|---|---|
| Build | pass |
| RAM | 89,380 bytes |
| Flash | 3,251,845 bytes |
| Delta from clean baseline | +8 RAM, +32 flash |

### Shared application-core baseline

The current working tree adds the feature backbone used by every planned feature:

- fixed-capacity IDs, result codes, identity references, friends, groups, and members;
- one event vocabulary spanning identity/trust, membership, chat, sync, location, navigation, markers, meetups, SOS, Help, and control;
- typed feature records for chat, reactions, sync cursors, position, navigation, markers, meetups, and incidents;
- shared provider interfaces for storage, security, transport, clock, location, and notifications;
- bounded event history and outbox state machine;
- durability/priority/member/group policy for every event type;
- explicit `RelayedOrObserved` versus `Delivered` states;
- host tests across all feature families;
- FriendMesh transmit still hard-disabled.

| Evidence | Result |
|---|---|
| Host test | `scripts/test-friendmesh-core.sh` passes |
| T-Deck build | pass |
| RAM | 89,404 bytes (27.3%) |
| Flash | 3,252,277 bytes (80.0%) |
| Delta from dormant service | +24 RAM, +432 flash |
| UI/radio/storage behavior | unchanged |

### People/groups/membership baseline

Phase 2 adds the complete functional membership cluster behind development-only
interfaces:

- a trusted-contact repository that references an existing WadaMesh/MeshCore
  contact directory without owning that directory;
- local development identity create/replace/clear behavior;
- bounded groups, member aliases, join order, roles, blocking, and verification;
- invitation lookup, candidate requests, transcripts, approval, rejection,
  cancellation, expiry, conflict handling, and capacity behavior;
- secure-default direct-only invitation policy with fresh zero-hop,
  forwarding-disabled transport observations; this is not treated as proximity
  proof or encryption;
- reversible leave/kick operations followed by an explicit rekey-required
  boundary;
- per-member development grant states for each membership epoch, including the
  rule that excluded/removed identities never receive the new grant;
- self/admin identity replacement with fresh approval, administration transfer,
  recorded and deduplicated majority succession, and disbanding;
- multi-group and fixed-capacity host simulations.

These are functional state transactions, not cryptographic security. The live
`MyMesh` contact adapter, persistence, UI, production proofs/grants, and radio
transport remain later integration work, and `canTransmit()` is still false.

| Evidence | Result |
|---|---|
| Host test | `scripts/test-friendmesh-core.sh` passes with UBSan and warnings-as-errors |
| T-Deck build | pass |
| RAM | 89,404 bytes (27.3%) |
| Flash | 3,253,533 bytes (80.1%) |
| Delta from shared core | +0 RAM, +1,256 flash |
| Physical/UI/storage/radio work | not performed |

### Chat/history/synchronization baseline

Phase 3 completes the functional chat, delivery, history, and synchronization
cluster using bounded development-only providers:

- message creation and receipt, durable reply references, reactions, authorized
  deletion tombstones, unread state, mute state, delayed receipt metadata, and
  untrusted-clock metadata;
- fixed payload handles with internal and expansion/SD-like capacity behavior,
  checksums, corruption/read-only/missing-media/write-failure simulation, and
  incomplete-history reporting;
- fixed 192-byte fragmentation with bounded, out-of-order reassembly and
  conflicting-fragment detection;
- a persisted development outbox with queued, transmitting,
  relayed-or-observed, delivered, retry-waiting, cancelled, and expired states;
- journal and outbox restoration across simulated reboot;
- bounded sync inventories, sender high-water/gap tracking, missing ranges,
  range and priority batches, receipts, deduplication, and quarantine for event,
  sequence, epoch, authorization, and reorder-window conflicts;
- host simulations for capacity pressure, storage failures, offline delay,
  duplicates, reordering, cancellation, expiry, reboot, and incomplete history.

This baseline does not use a production filesystem, SD driver, radio transport,
cryptography, or WadaMesh UI. FriendMesh transmission remains hard-disabled.

| Evidence | Result |
|---|---|
| Host test | `scripts/test-friendmesh-core.sh` passes with UBSan and warnings-as-errors |
| T-Deck build | pass |
| RAM | 89,404 bytes (27.3%) |
| Flash | 3,254,685 bytes (80.1%) |
| Delta from Phase 2 | +0 RAM, +1,152 flash |
| Physical/UI/production storage/radio work | not performed |

## Status language

- `[x]`: implemented and verified to the level named.
- `[ ]`: incomplete, partial, or missing evidence.
- `Host verified`: deterministic host checks passed.
- `Build verified`: named T-Deck environment compiled and size was recorded.
- `Physically verified`: named behavior was exercised on identified hardware.
- `Deferred`: deliberately outside current scope.

A build is not a physical pass. Old Meshtastic evidence is design history, not MeshCore proof.

## Roadmap: feature completeness first, security/hardening afterward

### Phase 0 — WadaMesh boundary and dormant service

- [x] Define FriendMeshOS as features for WadaMesh.
- [x] Lock scope to T-Deck and preserve WadaMesh UI/branding.
- [x] Record clean build baseline.
- [x] Add T-Deck-only service and hard transmit gate.
- [ ] Physical unchanged-WadaMesh smoke test, only with approval.

### Phase 1 — Shared application core

Goal: one bounded code base for all features.

- [x] Define common IDs, bounds, results, roles, states, and feature families.
- [x] Define friends, identity references, groups, and member records.
- [x] Define chat, reaction, sync, position, navigation, marker, meetup, SOS, and Help models.
- [x] Define one common event header and event policy table.
- [x] Define provider interfaces for storage, security, transport, clock, location, and notifications.
- [x] Add bounded history and outbox state machinery.
- [x] Add host tests covering every feature family, policy, group/member behavior, history, outbox, and hard transmit gate.
- [x] T-Deck build and size verification.
- [x] Add deterministic service lifecycle/reason codes and explicit non-global coordinator composition.

### Phase 2 — People, groups, invitations, and membership behavior

Implement together because they share identity references, verification state, membership epochs, roles, and administrative events.

- [x] Trusted-contact/friend repository over an existing-contact directory contract; live `MyMesh` binding remains integration work.
- [x] Local identity-reference lifecycle using development/test providers.
- [x] Group create/rename/disband and eight-group bounds.
- [x] Member aliases, roles, join ordering, approval, replacement, blocking, leave, kick, transfer, and recorded majority succession behavior.
- [x] Invitation sessions, code lookup, candidate requests, admin approval/rejection, verification transcript model, expiry, conflicts, and capacity errors.
- [x] Nearby direct-join policy requiring fresh zero-hop observation, forwarding disabled, comparison verification, and admin approval; production handshake/grant cryptography remains Phase 7.
- [x] Rekey-required states and per-member grants as functional domain transactions; production cryptography comes in the shared security phase.
- [x] Host simulation for complete membership lifecycle, multiple groups, unauthorized actions, expiry, and all fixed-capacity failures.

### Phase 3 — Chat, reactions, outbox, history, and synchronization

Implement together because they are all event-store and delivery-state consumers.

- [x] Chat messages, reactions, deletion tombstones, unread state, mute, and details.
- [x] Bounded payload storage handles and fragmentation/reassembly model.
- [x] Durable-outbox adapter contract, retries, cancellation, expiry, queue pressure, and delayed timestamps.
- [x] Bounded history, inventory/high-water cursors, gaps, batches, receipts, deduplication, and conflict quarantine.
- [x] Priority rules so safety/control/live traffic outrank old history.
- [x] Internal/SD capacity behavior and incomplete-history state using development storage providers.
- [x] Loss, duplicate, reordering, reboot, and offline-peer simulation.

### Phase 4 — Positions, Friend Compass, maps, navigation, markers, and meetups

Implement together because they use the same position freshness, distance/bearing, clock, and map data.

- [x] Existing MeshCore contact-position adapter, joined-channel-member map filter, and native Friend Compass entry flow.
- [x] Bounded one-at-a-time group-location refresh over existing encrypted MeshCore telemetry, authorized by local joined-roster membership without public adverts.
- [x] Great-circle distance, absolute bearing, accuracy, freshness, and source model.
- [x] Conservative two-fix motion estimate with a distinct observed trail, 45-second lead point, fallback behavior, and bounded SD-only per-channel history.
- [x] Two-page T-Deck compass: detailed spatial dial plus plain-language guidance/data, horizontal swipe, trackball paging, tappable dots, and current-position override.
- [x] Targeted authenticated Compass-start notice showing the starter and current straight-line distance, consumed before chat and attempted once per valid Compass session.
- [x] `NORTH-UP`, moving `GPS`, and optional future `MAG` heading providers.
- [x] Map and arrow-only navigation, arrival, closer/farther, target notifications, and bounded breadcrumbs.
- [x] Marker lifecycle, expiry/reconfirmation, details, and navigation service behavior.
- [x] Meetup proposals, votes, attendance, rally point, cancel/move, arrival, and expiry.
- [x] Missing/stale GPS and bounded functional simulations; live MeshCore position and group-filter adapters remain open above.

### Phase 5 — SOS, Help Requests, incidents, and notifications

Implement together because they share incident IDs, location policy, recipient states, notification priority, retries, and closure.

- [x] Intentional hold, cancel/privacy countdown, deduplication, and incident state machine.
- [x] SOS delivery/responding/unable/arrived/closure behavior.
- [x] Help categories, responses, escalation/de-escalation, silent mode, and closure.
- [x] Exact/last-known/missing location behavior.
- [x] Notification center/in-device alerts and storage-failure behavior.
- [x] Explicit public standard-MeshCore text fallback decision model.
- [x] Loss, congestion, reboot, missing GPS, no-peer, and storage-failure simulation.

### Phase 6 — Complete minimal WadaMesh UI integration

Integrate the completed functional feature set together so shared navigation/focus patterns are not repeatedly rewritten.

- [ ] Reuse existing contacts, chat, map, settings, alerts, status bar, and app drawer.
- [ ] Add only necessary rows, badges, actions, filters, metadata, and confirmation modals.
- [x] Add a simple `Channel` -> `Invite nearby` -> recipient `Join` flow inside existing WadaMesh chat/contact surfaces, reusing MeshCore private channels and encrypted direct messages.
- [x] Add a bounded `Members` view with persisted invite/join/failure/leave/removal states, encrypted direct control notices, binary encrypted roster snapshots that cannot surface as chat, duplicate-member invite prevention, and explicit rekey-required status.
- [x] Add bounded shared meetup/pickup, Help, response, closure, and three-second-hold SOS actions to the private-channel sheet, with encrypted binary control records and existing-map overlays.
- [x] Keep ordinary MeshCore channels free of FriendMesh actions; add an explicit `Create a FriendMesh group` path and `[FM]` inbox/header/action-sheet identification derived from persisted roster metadata.
- [ ] Upgrade the simple flow to `Private group` -> compare -> administrator approval -> `securing group` after shared production security exists.
- [ ] No replacement home screen, launcher, visual identity, or required theme.
- [ ] All functional states reachable by touch, keyboard, and trackball.
- [ ] Sensitive entry masked from WadaMesh screenshots and remote surfaces.
- [ ] Feature-complete local/development-provider walkthrough on T-Deck without production FriendMesh radio transmission.

Current Phase 6 slice: the existing app drawer opens a native WadaMesh-styled
FriendMesh page with event-driven state refresh, local notes, meetup creation, a
direct `View map` action, and local ride Help. FriendMesh meetup markers render
as typed overlays in the existing map, participate in overlap selection, and
show details when tapped. The same native channel action sheet now exposes
`Invite nearby`; a bounded Bluetooth scan maps advertised public-key prefixes to
saved chat contacts, with a fresh zero-hop LoRa observation as fallback. The
channel key never travels over Bluetooth. The receiver must confirm before the
existing WadaMesh channel is stored. This is the simple MeshCore channel bridge,
not the future transcript-bound FriendMesh group protocol. The action sheet also
opens a WadaMesh-native `Members` view whose bounded persistent roster tracks
invited, joined, failed, left, and removed members. Cooperative removal marks
the group as needing a rekey rather than claiming the old shared key was revoked.
The channel action sheet also exposes `Group map`. It adapts joined roster members'
existing MeshCore contact coordinates into the shared FriendMesh E7 position
model, filters WadaMesh's existing map and positioned-contact list to that
bounded roster, and exposes `Friend Compass` from a member's map action sheet.
Opening Group Map now polls the bounded joined roster one contact at a time over
the existing encrypted MeshCore telemetry exchange. The receiver authorizes a
joined FriendMesh contact independently of the public advert-location switch;
the queue stops when complete or when Map is closed.
The compass refreshes only the selected contact, uses north-up absolute bearing
on T-Deck, and labels missing or older-than-five-minute coordinates instead of
presenting them as live. Two recent plausible fixes add an observed trail and a
45-second predicted meeting point; the bounded two-fix-per-member history lives
only on microSD and is not yet encrypted or authenticated. `Coordinate` now
exposes the first shared navigation
and safety flow: one bounded meetup/pickup and one Help/SOS incident per channel,
member responses, cancellation/closure, typed markers on the same map, and a
continuous three-second SOS hold. Reserved encrypted group-data type `0xFF02`
is consumed before chat surfaces. The fixed state caps at 248 bytes and adds no
background transmission. Ordinary MeshCore channels now retain only ordinary
channel actions; FriendMesh controls require a persisted joined roster created
through the explicit group path or accepted nearby invitation. Those groups are
marked `[FM]` without renaming the channel. Page one's prediction readout now
preformats its speed outside LVGL's float-disabled formatter, bounds the lead
display to the intended 45 seconds, and uses labeled rings sharing the plotted
position scale. The T-Deck build passes at 91,052 bytes RAM (27.8%) and
3,316,085 bytes flash (81.6%); physical two-device testing
of compass paging, motion prediction, SD retention, and the targeted start
notification remains open.

### Phase 7 — Shared production security implementation

Implement all dependent security together across the completed features.

- [ ] Real FriendMesh signing identity and MeshCore binding.
- [ ] Independent storage PIN, KDF/wrapping, device binding, AEAD records, subkeys, nonce discipline, and secret wiping.
- [ ] Authenticated two-slot/journal storage and schema migration.
- [ ] Canonical signing/encryption for every event family.
- [ ] Verification proof, invitations, member-specific key grants, epochs, replay windows, and downgrade rejection.
- [ ] Transcript-bound ephemeral pairwise session and authenticated member-specific epoch grant; identity private keys never leave their device.
- [ ] Forward-only rekey, removal, replacement, succession, and disband recovery.
- [ ] Protected internal essential state and optional SD expansion.
- [ ] Published vectors, mutation/truncation tests, fault injection, secret scans, and power-loss simulation.
- [ ] No security-dependent feature keeps a separate crypto/storage path.

### Phase 8 — MeshCore protocol and controlled radio integration

- [x] Derive the MeshCore encrypted-DM send seam and direct-route/path-hash receive fields needed by the simple private-channel invitation bridge.
- [ ] Complete the actual MeshCore packet/routing/ACK contract for production FriendMesh events; copy no Meshtastic assumptions.
- [x] Enforce an empty outbound route, bounded Bluetooth-or-zero-hop sender discovery, and direct/zero-path receive checks for simple channel invitations.
- [x] Validate the simple LoRa direct-path channel invitation on two physical T-Decks.
- [x] Add bounded join acknowledgement, leave/removal notices, and reserved encrypted MeshCore group-data roster snapshots; consume the obsolete text envelope before every user-message surface.
- [x] Add a bounded encrypted MeshCore group-data compatibility codec for meetup/pickup and Help/SOS state, consumed before every user-message surface.
- [x] Validate Bluetooth discovery on two physical T-Decks.
- [ ] Complete forwarding/downgrade enforcement for the production protocol.
- [ ] Canonical codec vectors, parser fuzzing, fragmentation, replay, expiry, and airtime bounds.
- [ ] Narrow reviewed `MyMesh` receive/transmit seams.
- [ ] Durable policy gates connected to transport.
- [ ] Replace unconditional transmit false only after every shared security gate has tests.
- [ ] Operator-approved direct and relayed T-Deck testing on a bounded test configuration.

### Phase 9 — Security fixes, hardening, and recovery campaign

- [ ] Fix findings across the complete feature set, prioritizing shared causes over per-screen patches.
- [ ] Audit USB/TCP/WebSocket/BLE/web/MQTT, logs, backups, QR, screenshots, SD, and exports.
- [ ] Fuzzing, storage fault injection, power cuts, endurance, queue bounds, memory/stack/PSRAM, airtime, and battery.
- [ ] Corruption, wrong PIN/binding, missing/read-only/full/removed SD, migration, rollback, and last-known-good recovery.
- [ ] Threat-model review and no unresolved critical/high issues before release.

### Phase 10 — Physical feature qualification and release

- [ ] Multi-device people/group/invitation/chat/sync/rekey tests.
- [ ] Map/navigation/meetup/marker and SOS/Help drills.
- [ ] Ordinary WadaMesh chat, contacts, maps, companion, OTA, recovery, and screenshot regression.
- [ ] Every added state through touch, keyboard, trackball, and existing WadaMesh themes.
- [ ] Reproducible artifact, checksums, migration/recovery guide, limitations, and changelog.
- [ ] Explicit release approval.

## Deferred work

- Heltec implementation or compatibility claims;
- replacement UI, launcher, branding, or repo identity;
- mandatory FriendMesh themes;
- Meshtastic APIs/protobufs/NodeDB/Router/`PRIVATE_APP` assumptions;
- active Wi-Fi attacks or disruptive RF tooling;
- guaranteed anonymity, delivery, remote deletion, GPS accuracy, or emergency dispatch;
- automatic secret provisioning or plaintext fallback.

## Immediate next work

Phases 1 through 5 are host/build complete. Phase 6 now includes the physically
validated nearby join/roster path, the build-verified automatic group map and
Friend Compass, and build-verified shared meetup/pickup plus Help/SOS
coordination over the existing encrypted MeshCore channel. The general
development workspace still does not persist across reboot or communicate
through its custom event path.

Next, physically validate the new coordination flow on two T-Decks: meetup map
appearance without adverts, responses on both devices, cancel/close propagation,
Help alerts, three-second SOS hold, protocol records absent from chat, reboot
persistence, and ordinary chat/map regression. After that, continue the
remaining native people/group/chat flows before the shared Phase 7 protected
identity, signing, replay, grant, and rekey implementation.

## Session handoff template

```text
Date:
Branch / commit:
Phase / feature cluster:
Files changed:
Implemented:
Intentionally disabled/deferred:

Host tests:
T-Deck build result and size:
T-Deck physical result:
Storage/radio/remote surfaces exercised:
Secrets/redaction review:
Device-changing actions:

Known issues:
Next exact action:
```

## Definition of success

FriendMeshOS features succeed when the planned feature system works coherently through shared code, then passes the shared security, recovery, radio, exposure, and physical campaigns—while WadaMesh remains recognizably and operationally WadaMesh.
