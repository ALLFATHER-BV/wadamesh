# FriendMeshOS features for WadaMesh — roadmap

Status: active T-Deck-only, feature-complete-first roadmap
Updated: 2026-07-26
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
- [x] T-Deck FriendMesh long-term data is SD-bound. The only intentional
  no-SD social fallback is four compact accepted Friend cards; pending requests,
  histories, routes, locations, and other growing records do not persist internally.
- [x] Non-destructive SPIFFS-to-SD repair explicitly migrates and verifies identity, recursively preserves additional data, and retains the internal recovery source.
- [x] Ordinary WadaMesh/MeshCore behavior remains unchanged when FriendMesh is disabled.
- [x] General FriendMesh event transmission remains hard-disabled. Reviewed
  compatibility exceptions use existing MeshCore carriers: private-channel
  invites, roster/coordination records, and the bounded Friend Request exchange.
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

1. [`README.md`](../../README.md) — WadaMesh introduction/build entrypoint.
2. [`FEATURES.md`](FEATURES.md) — repository survey and integration boundary.
3. [`IMPLEMENTATION_GAME_PLAN.md`](IMPLEMENTATION_GAME_PLAN.md) — detailed product/security/test contracts.
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
- [x] T-Deck curated Friends surface over the live MeshCore directory: full-key
  SD-backed records, private aliases, Friends-first filtering, local name search
  with duplicate-name fingerprints, and add flows from Discovered, contacts,
  map markers, or a complete public key. Host/build verified; physical-device
  persistence and interaction regression remain open. This does not yet claim
  Phase 7 trust verification.
- [x] Explicit public-channel Friend Request flow: long-press a currently
  received message, return a signed requester identity through that message's
  reversed MeshCore path, match the exact packet hash on the intended device,
  review it in an opt-in Accept/Deny inbox, and return acceptance through
  MeshCore's authenticated/encrypted anonymous-request carrier using the
  request's bounded signed return path. Friendship promotion is two-phase and
  non-optimistic: the accepter keeps the inbox request pending; the requester
  adds the accepter only after receiving acceptance and successfully queuing
  its authenticated acknowledgement; the accepter adds the requester and
  resolves the inbox only after receiving that acknowledgement. Acceptance is
  idempotently acknowledged and retried, so the accepter never needs to send a
  reciprocal request. Denial sends one authenticated direct control without an
  ACK and offers an identity block.
  This adds an
  application group-data type (`0xFF03`) without changing MeshCore's packet
  format; both endpoints need FriendMesh-aware firmware.
- [x] Bound Friend Request state: 16 recent locally-sent message hashes; at
  most 12 pending records on SD or four session-only pending records without
  SD; at most four compact accepted Friend cards when SD is unavailable; and a
  shared 16-record, two-slot checksummed NVS transaction journal containing
  only public correlation metadata and attempt budgets. Exact inbox replays are
  ignored; a newly verified ID from the same identity replaces the stale
  transaction without another alert. No request route/hash history, private
  keys, channel secrets, message content, or location is written to the journal.
- [x] Contextual confirmed removal in the main contact action sheet: Friends
  expose `Remove friend`, send an authenticated quiet reciprocal removal, and
  retain the MeshCore contact; blocking a Friend removes the relationship first
  and blocks future requests from that key. Non-friends
  expose `Remove node` and clear the network contact plus matching Discovered
  cache entry. Bulk contact deletion excludes Friends.
- [x] Make request, acceptance, decline, and reciprocal-removal delivery use a
  shared bounded transaction policy: direct first, at most two initiator-owned
  floods, no flooded ACK, one direct decline without an ACK, at most four
  RAM-only live control payloads, authenticated peer acknowledgements, short
  retries, duplicate suppression, ACK-gated two-phase Friend promotion,
  strict MeshCore AES zero-padding normalization, pending/completed/unknown
  acceptance correlation, deferred requestee inbox resolution, and forced Friends-list cache
  invalidation so both endpoints update without reboot. Add WadaMesh-side
  diagnostics for request correlation, control queuing/retries, radio-driver
  transmit completion/failure, raw anonymous-request routing/decrypt probes,
  acknowledgement matching, and storage finalization without modifying
  MeshCore. A dismissible progress modal reports completed/current stages and
  the flood count. The journal restores correlation, terminal tombstones, and budgets
  after reboot, but does not yet reconstruct the RAM-only encoded payload and
  route for autonomous resend. Both invalid journal slots disable new sends and
  raise a recovery-required UI state. Host tests and the T-Deck build pass;
  physical two-endpoint validation over the existing local multi-hop MeshCore
  network remains open.
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
- [x] Local GPS course/headway: bounded SD-backed user fixes, qualitative maneuver instructions and shaped arrows with a north-arrow override, closer/farther/steady state, signed closing speed, and ETA only while approaching.
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

- [x] Reuse existing contacts, chat, map, settings, alerts, and status bar;
  FriendMesh adds no dedicated app-drawer tile or parallel workspace.
- [x] Add only necessary rows, badges, actions, filters, metadata, and
  confirmation/progress modals for the currently live FriendMesh surfaces.
- [x] Add `Add channel` -> `FriendMesh channels` -> `Create` / `Join`; Create opens a two-minute six-digit BLE host session and Join discovers it without a MeshCore advert, verifies the code, and provisions the existing MeshCore private channel over an encrypted direct BLE exchange.
- [x] Replace immediate nearby Friend writes with a consent-based BLE request flow: discovery-only scan, full-key/prefix match, signed target-bound request, shared Accept/Deny/Block inbox, acceptance-only reciprocal add/notification, and local-only `Save contact by key` semantics.
- [x] Add a bounded `Members` view with persisted invite/join/failure/leave/removal states, encrypted direct control notices, binary encrypted roster snapshots that cannot surface as chat, duplicate-member invite prevention, and explicit rekey-required status.
- [x] Replace the independent legacy roster/coordination blobs with one
  versioned, channel-bound, checksummed two-slot group record. Preserve full
  member public keys when locally resolvable, migrate valid legacy blobs on
  first access, verify every inactive-slot write before selection, and block
  corrupt, divergent, oversized, or read-only mutations rather than treating
  them as empty state. This is crash recovery and integrity detection, not the
  Phase 7 authenticated/encrypted protected store.
- [x] Add bounded shared meetup/pickup, Help, response, closure, and three-second-hold SOS actions to the private-channel sheet, with encrypted binary control records and existing-map overlays.
- [x] Keep ordinary MeshCore channels free of FriendMesh actions; add an explicit `Create a FriendMesh group` path and `[FM]` inbox/header/action-sheet identification derived from persisted roster metadata.
- [x] Add confirmed administrator disband to the native Members view, with best-effort encrypted member notices, chat suppression, local deletion, and explicit unreachable-recipient reporting.
- [ ] Upgrade the six-digit BLE compatibility handshake to transcript-bound signed membership epochs and explicit administrator approval after shared production security exists.
- [x] No replacement home screen, launcher, visual identity, or required theme.
- [ ] All functional states reachable by touch, keyboard, and trackball.
- [ ] Sensitive entry masked from WadaMesh screenshots and remote surfaces.
- [x] Remove the obsolete volatile local development app, its PSRAM runtime,
  local-only notes/markers/Help demonstration, launcher tile, and walkthrough
  test; retain the platform-neutral services and live integrations.

Current Phase 6 slice: the obsolete volatile FriendMesh app and its local-only
workspace have been removed. FriendMesh behavior is exposed through WadaMesh's
existing Contacts/Friends, Friend Request inbox, private-channel action sheet,
map, alerts, and transaction-progress modal. `Add channel` exposes one
`FriendMesh channels` entry with a secondary `Create` / `Join` choice. Create initializes the normal
MeshCore private channel and opens a RAM-only two-minute host screen displaying
a six-digit code. Join performs an active BLE scan; the host's scan response is
the direct ACK, so neither device needs a MeshCore advert, saved contact, or LoRa
route. After code entry, a bounded AES-GCM GATT exchange transfers the existing
channel name/secret plus both public identities and installs the normal roster
locally on both devices. The code is never advertised; five failed attempts end
the session, and existing joined public-key prefixes are rejected. This is a
short-range compatibility handshake, not the future transcript-bound signed
FriendMesh membership protocol. The action sheet also
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
The compass refreshes only the selected contact, offers absolute north-up or a
two-local-fix GPS-course headway-up dial on T-Deck, and labels missing or
older-than-five-minute coordinates instead of
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
marked `[FM]` without renaming the channel. Roster, rekey status, coordination,
and one pending coordination envelope now share a version-2 group record bound
to the channel secret. Each commit writes the inactive SD slot, reopens and
byte/decode verifies it, then advances the generation. Valid legacy roster and
coordination blobs migrate together and remain as rollback evidence; locally
resolvable identities upgrade to full keys while unresolved six-byte prefixes
remain explicitly legacy. One corrupt slot can recover from its valid peer;
equal-generation divergence, invalid legacy data, and read-only storage fail
closed. Coordination and member removal persist before radio submission and
roll back on explicit queue failure. The checksum detects torn writes and
accidental corruption but does not authenticate an attacker with SD access.
Unreadable metadata retains its FriendMesh channel classification, and Members,
Group Map, and Coordination show `Group storage needs recovery` instead of an
empty roster or ordinary-channel interpretation. The first physical build of
this record layer exposed a `loopTask` stack-canary crash while Create Group
looked for the expected, not-yet-created first slot. The matching ELF decoded
the path as Create Group -> roster save -> record save -> record load -> SD
open; no corrupt group file was involved. The nested record locals consumed
roughly 4 KiB of stack. All group-record candidates, save verification records,
caller records, and roster-preservation workspaces now use bounded, wiped heap
allocations and fail closed on allocation failure. Firmware disassembly reduced
the record-load frame from about 1,456 to 176 bytes and the record-save frame
from about 1,456 to 192 bytes; a host guard rejects future stack-local group
records. This is host/build verified and still requires the same physical
Friend Map and Create Group actions to prove the crash is gone.
Page one's prediction readout now preformats its speed outside LVGL's
float-disabled formatter, bounds the lead
display to the intended 45 seconds, and uses labeled rings sharing the plotted
position scale. The SD-required persistence and duplicate-asset cleanup pass
puts the T-Deck build at 87,020 bytes RAM (26.6%) and 3,293,117 bytes flash
(81.0%), saving 4,096 bytes of static internal RAM and 26,272 bytes of flash
without changing WadaMesh's UI. Long-term `DataStore`, history, FriendMesh
motion and battery data, backups, crash exports, discovery state, and online map caches are routed to SD;
missing-card recovery is read-only. The compass orientation and maneuver-arrow
interaction is physically validated on T-Deck. Physical two-device testing of
the targeted Compass-start notification and two-moving-device prediction is
hardware-blocked until additional GPS-capable devices and a node are available.
The administrator disband change is build-verified but has not yet been
installed; it remains queued for testing on both available T-Decks. The reported
settings-migration diagnosis/recovery checks and map-tile retention check are
complete.

Current physical follow-up status:

- [x] Diagnose the observed SD-backed settings transition.
- [x] Complete the safe settings recovery/selection check.
- [x] Confirm existing SD map tiles remain intact.
- [x] Decode the matching Create Group coredump and identify `loopTask` stack
  exhaustion in the nested version-2 group-storage call chain.
- [x] Move all large group-record/storage workspaces off the UI task stack,
  add fail-closed allocation handling, and build the T-Deck target.
- [ ] Install the stack-safe build on both owned T-Decks and repeatedly open
  Friend Map, create a group from a clean state, close/reopen it, and reboot.
  Confirm no stack canary, the first non-empty `fm_group_<binding>.0` appears
  only after a verified commit, and the group remains readable.
- [ ] Validate the revised Friend Request transaction between two owned T-Deck
  endpoints while the existing local MeshCore network supplies the relays. Do
  not modify, flash, administer, or depend on cooperation from community nodes.
  Capture the observed route/hop evidence for the request, direct acceptance,
  direct-only ACK, authenticated one-way decline, and reciprocal removal.
- [ ] Exercise natural route loss or locally invalidate an endpoint's cached
  route during a quiet, deliberate test window. Do not jam or disrupt the live
  network. Prove that no transaction emits more than two initiator-owned floods
  and that no ACK can flood.
- [ ] Reboot at each Friend Request transaction stage and confirm correlation,
  peer binding, terminal tombstones, and flood budgets restore without duplicate
  Friends or notifications. Automatic payload/route reconstruction remains
  explicitly deferred.
- [ ] Regress ordinary modified-to-modified and stock-to-modified MeshCore
  messaging, relay behavior, contacts, and map behavior after the transaction test.
- [ ] Install and test administrator disband on both current T-Decks.
- [ ] Test the targeted Compass-start notification; hardware-blocked pending additional GPS-capable devices and a node.
- [ ] Test two-moving-device prediction/headway; hardware-blocked pending additional GPS-capable devices and a node.
- [ ] Confirm FriendMesh group, motion, and coordination state across reboot.
- [ ] On both owned T-Decks, migrate an existing legacy group and verify roster,
  rekey flag, meetup/incident state, and full-key upgrades across reboot. Then
  inject one corrupt slot, one truncated inactive-slot write, and one
  equal-generation divergence; prove valid-slot recovery where unambiguous and
  fail-closed recovery-required behavior where it is not.
- [ ] Interrupt power at each group commit boundary (before write, after
  inactive-slot write, after radio queue acceptance, and before pending-marker
  clear). Confirm there is no fabricated empty roster, no transmit before a
  verified commit, and no silent overwrite of a pending coordination event.
- [ ] Exercise missing, removed, read-only, and full-SD behavior.

### Phase 7 — Shared production security implementation

Implement all dependent security together across the completed features.

- [ ] Real FriendMesh signing identity and MeshCore binding.
- [ ] Independent storage PIN, KDF/wrapping, device binding, AEAD records, subkeys, nonce discipline, and secret wiping.
- [ ] Upgrade checksummed functional two-slot group records and the existing
  transaction journal to authenticated/encrypted protected records with schema
  migration, rollback resistance, and recovery UX.
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
validated nearby join/roster path and compass interaction, the build-verified
automatic group map and administrator disband flow, build-verified shared
meetup/pickup plus Help/SOS coordination over the existing encrypted MeshCore
channel, and the shared Friend Request transaction engine. The obsolete
FriendMesh development app has been removed. The stack-safe T-Deck build passes
at 89,388 bytes RAM (27.3%) and 3,355,329 bytes application flash (82.6%); no
image containing the stack fix or revised transaction system has yet been
physically validated.

The next exact gate is to install this build on the two owned endpoints and
first repeat the two actions that crashed: open Friend Map and create/reopen a
group. Capture the serial log through the first verified group commit and a
reboot. Only after that smoke test passes, run the bounded group-storage
preflight above: legacy migration, ordinary reboot, one-slot
corruption/truncation recovery, divergence fail-closed behavior, and
missing/read-only/full-SD mutation blocking. Do not intentionally corrupt the
only valid slot; retain the legacy blobs and a card image for recovery.

After that preflight, run the operator-approved two-endpoint Friend Request
campaign over the existing local MeshCore network. One owned T-Deck is the
requester and one is the recipient; ordinary community nodes/repeaters provide
whatever relay
path the network naturally selects and receive no firmware or configuration
changes. First capture the actual multi-hop route evidence, then complete
acceptance and the direct-only ACK. Separate bounded transactions cover the
one-way authenticated decline and reciprocal removal.

Exercise flood fallback only during a quiet, deliberate test window and only
after natural route loss or local endpoint route invalidation; do not jam,
impersonate, or disrupt community infrastructure. The campaign must also cover
duplicate delivery, reboot at every durable stage, and proof that the initiating
operation emits no more than two floods. Run ordinary modified-to-modified and
stock-to-modified MeshCore regression on the same live network. Record endpoint
radio logs, observed route/hop data, and UI modal stages separately from final
Friends-list results. A community relay forwarding a reserved carrier is not by
itself proof of full stock-node FriendMesh interoperability.

After that gate, install and validate administrator disband on both current
T-Decks, then physically validate meetup/pickup and Help/SOS coordination:
map appearance without adverts, responses on both devices, cancel/close
propagation, three-second SOS hold, protocol records absent from chat, reboot
persistence, and ordinary chat/map regression. Compass-start and two-moving-GPS
tests remain hardware-blocked as recorded above. Shared Phase 7 protected
identity, signing, replay, grant, and rekey work follows the functional physical
gates; none of those security properties are claimed by the compatibility paths.

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
