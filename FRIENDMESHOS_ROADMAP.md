# FriendMeshOS features for WadaMesh — roadmap

Status: active T-Deck-only development roadmap  
Updated: 2026-07-18  
Repository: `Atlessc/wadamesh`  
Working branch: `friendmeshOS-proposal`  
Current firmware base: WadaMesh with MeshCore/meshcomod `core-v1.16.5`  
Current build target: `LilyGo_TDeck_companion_radio_touch`

## What this project is

This repository is **WadaMesh**. It is not being renamed, replaced, or taken over.

**FriendMeshOS features** is the name of an optional feature layer being added to WadaMesh for friends, trusted contacts, private groups, field coordination, recovery, and safety-oriented workflows. WadaMesh remains the firmware, user experience, visual identity, navigation system, release identity, and upstream base.

The project starts from WadaMesh because WadaMesh already provides the hard product shell:

- a mature on-device LVGL interface;
- MeshCore companion-radio messaging and routing;
- contacts, channels, chat history, maps, GPS, sensors, and radio controls;
- touch, keyboard, and trackball input on the T-Deck;
- USB, TCP, WebSocket, BLE, MQTT, web, update, and recovery infrastructure;
- SPIFFS, optional SD storage, preference storage, and migration behavior;
- an existing screenshot feature and other field/debug tooling.

FriendMesh work should add missing trust and coordination capabilities behind narrow service boundaries. It should not rebuild functionality WadaMesh already handles well.

## Product boundary

The following rules are non-negotiable:

- [x] Keep the WadaMesh name, branding, boot identity, terminology, layout, tabs, app drawer, settings structure, and navigation.
- [x] Keep FriendMeshOS described as features for WadaMesh, not a replacement firmware or repository takeover.
- [x] Develop and build only for the LilyGo T-Deck / T-Deck Plus during the current scope.
- [x] Do not edit, build, flash, or claim Heltec compatibility until the hardware scope is explicitly expanded.
- [x] Keep ordinary MeshCore/WadaMesh behavior unchanged when FriendMesh features are unavailable or disabled.
- [x] Keep FriendMesh transmission disabled until identity, storage, protocol, replay, recovery, migration, and consent gates pass.
- [x] Do not automatically provision secrets or mutate FriendMesh storage during a read-only boot probe.
- [x] Never fall back to plaintext storage for protected FriendMesh material.
- [x] Add only small hooks to existing WadaMesh screens when a feature needs UI.
- [x] Optional additions to WadaMesh's existing theme system are the maximum broad visual change in scope.
- [x] Do not require a FriendMesh theme; every feature must work with WadaMesh's existing themes.

## Sources of truth

Read these documents together:

1. [`README.md`](README.md) — how to build and use WadaMesh.
2. [`FRIENDMESHOS_FEATURES.md`](FRIENDMESHOS_FEATURES.md) — repository architecture, existing capabilities, hardware notes, and feature boundary.
3. [`FRIENDMESH_FULL_IMPLEMENTATION_GAME_PLAN.md`](FRIENDMESH_FULL_IMPLEMENTATION_GAME_PLAN.md) — detailed FriendMesh behavior, security contracts, acceptance gates, and implementation rules.
4. This roadmap — current order, status, completed baseline, and next work.
5. Live source and build output — final authority for what is actually implemented and verified.

If a document conflicts with the live source, the source wins for current behavior and the documents must be corrected before further work.

## Current verified baseline

### WadaMesh baseline

The clean reference build was recorded before the FriendMesh feature service was enabled:

| Evidence | Result |
|---|---|
| Git baseline | `95a9f05` on `friendmeshOS-proposal` |
| PlatformIO environment | `LilyGo_TDeck_companion_radio_touch` |
| Build result | pass |
| RAM | 89,372 / 327,680 bytes (27.3%) |
| Application flash | 3,251,813 / 4,063,232 bytes (80.0%) |
| Physical test for this baseline session | not run |
| Heltec build/test | intentionally not run |

### FriendMesh backend foundation

The new starting base is now present:

- `src/friendmesh/FriendMeshFeatureService.*` owns the optional feature lifecycle boundary.
- `FRIENDMESH_FEATURES=1` is defined only in the T-Deck PlatformIO environment.
- `src/friendmesh/*.cpp` is compiled only into that T-Deck environment.
- `src/main.cpp` starts the service after ordinary MeshCore initialization.
- startup state is `NotConfigured`.
- storage, identity, protocol, and consent readiness are false.
- `canTransmit()` is hard-coded to return false.
- the service performs no filesystem, NVS, SD, identity, protocol, network, radio, or UI work.

| Evidence | Result |
|---|---|
| T-Deck build | pass |
| RAM | 89,380 / 327,680 bytes (27.3%) |
| Application flash | 3,251,845 / 4,063,232 bytes (80.0%) |
| Delta from baseline | +8 bytes RAM, +32 bytes flash |
| FriendMesh object compiled | yes |
| FriendMesh transmission | disabled |
| WadaMesh UI/branding changes | none |
| Flash or physical test | not performed |

This service is the starting point for all new FriendMesh features. New work should extend or collaborate with this service rather than scattering FriendMesh state through `UITask`, `MyMesh`, preferences, and board code.

## Existing WadaMesh features we inherit

These are baseline WadaMesh capabilities, not new FriendMesh accomplishments:

- MeshCore public/private chat and contacts;
- map and GPS-related presentation;
- message history persistence;
- touch, T-Deck keyboard, and trackball input;
- multi-transport companion access;
- Wi-Fi, BLE, MQTT, web mirror, OTA, and recovery support where configured;
- SD and SPIFFS storage paths;
- application drawer, command center, control center, settings, status bar, and existing themes;
- status-bar long-hold screenshot capture to an SD-card BMP under `/screenshots`;
- image viewing, spectrum and diagnostic utilities already present in WadaMesh.

FriendMesh must reuse these surfaces where appropriate and explicitly review their privacy/security exposure before showing FriendMesh data.

## Status language

Use these terms consistently:

- `[x]` — implemented and verified to the level named by the item.
- `[ ]` — not complete; may be untouched, partial, or awaiting evidence.
- `Implemented` — source exists.
- `Host verified` — deterministic host checks passed.
- `Build verified` — the named T-Deck environment compiled and size was recorded.
- `Physically verified` — the named behavior was exercised on identified T-Deck hardware.
- `Deferred` — deliberately outside the current phase, not accidentally forgotten.
- `Blocked` — cannot progress without a named decision, permission, hardware item, or dependency.

A build pass is not a physical pass. Old Meshtastic FriendMesh evidence is design history, not proof for this MeshCore implementation.

## Roadmap ordered from easiest to hardest

### Phase 0 — Project definition and T-Deck foundation

Goal: establish a truthful base without changing the product experience.

- [x] Document WadaMesh as the host project and FriendMeshOS as an optional feature layer.
- [x] Lock current implementation scope to the T-Deck.
- [x] Preserve WadaMesh UI and branding.
- [x] Record the clean T-Deck build baseline.
- [x] Add the T-Deck-only `src/friendmesh/` service boundary.
- [x] Add a secret-free status snapshot.
- [x] Hard-disable FriendMesh transmission.
- [x] Rebuild and record the size delta.
- [ ] Physically smoke-test the unchanged WadaMesh behavior on the development T-Deck, only with explicit approval.

Exit rule: complete for source/build work; physical evidence remains open.

### Phase 1 — Status, diagnostics, and policy model

Goal: make future behavior observable without secrets or radio changes.

- [ ] Define stable lifecycle states: unavailable, not configured, locked, ready, degraded, and recovery required.
- [ ] Define reason codes without filenames, key bytes, PINs, message content, or sensitive identifiers.
- [ ] Add a bounded in-memory event/status history.
- [ ] Add host tests for state transitions and transmit policy.
- [ ] Add one minimal status line or detail modal to an existing WadaMesh settings/about/diagnostic surface only after the backend contract is stable.
- [ ] Verify screenshots and companion diagnostics do not expose secrets.
- [ ] Keep transmit and receive integration disabled.

Exit rule: status is deterministic, secret-free, testable, and does not alter ordinary WadaMesh behavior.

### Phase 2 — Protected storage primitives

Goal: provide trustworthy T-Deck storage before creating an identity.

- [ ] Define internal essential storage and optional SD expansion roles.
- [ ] Define an independent FriendMesh storage PIN; do not reuse BLE or screen-lock PINs.
- [ ] Select reviewed KDF, wrapping, AEAD, nonce, and device-binding designs.
- [ ] Implement authenticated record framing and versioning.
- [ ] Implement two-slot or journaled power-loss recovery.
- [ ] Probe read-only at startup; require explicit user action for provisioning.
- [ ] Fail closed on corruption, wrong PIN, wrong binding, missing storage, or unsupported schema.
- [ ] Add deterministic host tests, mutation/truncation tests, and power-loss fault injection.
- [ ] Keep transmit disabled.

Exit rule: protected storage is host/build verified and cannot silently create or downgrade secrets.

### Phase 3 — FriendMesh signing identity

Goal: create one protected device identity with an explicit lifecycle.

- [ ] Define the relationship between MeshCore identity and the separate FriendMesh signing identity.
- [ ] Validate the selected signing implementation with published vectors.
- [ ] Require explicit create/import/restore action.
- [ ] Store the private identity only through Phase 2 protected storage.
- [ ] Implement lock, unlock, wipe-from-RAM, reboot recovery, backup policy, and replacement semantics.
- [ ] Expose only public fingerprint and secret-free status.
- [ ] Physically verify create, lock, unlock, reboot, wrong PIN, corruption, and recovery on the T-Deck.
- [ ] Keep general radio transmit disabled.

Exit rule: identity continuity and failure behavior are proven without protocol transmission.

### Phase 4 — Trusted contacts and local Friend Compass

Goal: add useful local-only friend behavior before inventing a wire protocol.

- [ ] Define trusted-contact records that reference existing MeshCore contacts without overwriting them.
- [ ] Add local favorite/trust labels and verification state.
- [ ] Calculate distance and bearing from existing position data.
- [ ] Support north-up as the guaranteed compass fallback.
- [ ] Use GPS course only while moving and label the heading source honestly.
- [ ] Mark stale/missing coordinates and hop observations clearly.
- [ ] Add minimal actions to existing contact details and map surfaces.
- [ ] Verify touch, keyboard, and trackball behavior.
- [ ] Keep FriendMesh protocol transmit disabled.

Exit rule: local friend organization and navigation work without new on-air traffic.

### Phase 5 — Group domain model

Goal: define groups, roles, membership epochs, and security states locally.

- [ ] Confirm release-one limits after memory/airtime estimates; initial planning target is up to eight groups and eight tested members per group.
- [ ] Define group IDs, aliases, roles, membership states, epochs, and admin-event ordering.
- [ ] Define secure, locked, degraded, rekey-pending, and unsafe-configuration states.
- [ ] Add bounded internal persistence using protected storage.
- [ ] Define SD expansion without making SD mandatory for essential group access.
- [ ] Add local create/delete simulations and recovery tests with no radio integration.
- [ ] Add small group entries to existing WadaMesh surfaces only after the model passes tests.

Exit rule: all local group transitions are deterministic, durable, bounded, and recoverable.

### Phase 6 — MeshCore FriendMesh wire contract

Goal: derive a protocol for MeshCore instead of copying Meshtastic assumptions.

- [ ] Identify reviewed `MyMesh` receive/transmit seams and external core implications.
- [ ] Specify packet format, versioning, signature scope, encryption scope, group dispatch, fragmentation, expiration, and size limits.
- [ ] Specify routing and relay behavior using actual MeshCore primitives.
- [ ] Specify ACK meaning and avoid presenting relay/queue evidence as end-recipient proof.
- [ ] Specify replay windows, sender sequences, event IDs, epoch checks, and downgrade rejection.
- [ ] Specify stock MeshCore behavior when FriendMesh data is unsupported.
- [ ] Add canonical byte vectors, parser rejection cases, fuzzing, and airtime budgets.
- [ ] Review whether the pinned external core needs a deliberate versioned change.
- [ ] Keep production transmit disabled; allow only host vectors or an explicitly approved loopback harness.

Exit rule: the protocol is documented, bounded, test-vector complete, and security reviewed before radio use.

### Phase 7 — Nearby invitation and verification

Goal: safely bind people/devices to approved group membership.

- [ ] Define invitation lifetime and human-verifiable code/transcript.
- [ ] Prove possession of MeshCore and FriendMesh identity material as required by the approved protocol.
- [ ] Require in-person verification for trusted membership.
- [ ] Handle alias conflicts, full groups, expired invitations, replay, spoofing, and changed identities.
- [ ] Deliver initial group material only after approval.
- [ ] Test with two T-Decks before expanding topology.

Exit rule: an unverified device cannot acquire group secrets or become an approved member.

### Phase 8 — Signed group chat and durable outbox

Goal: deliver the first controlled end-to-end FriendMesh feature.

- [ ] Persist ordinary outgoing events before transmission.
- [ ] Implement bounded retry, expiration, queue pressure, and cancellation policy.
- [ ] Add signed messages, delivery states, reactions, and deletion tombstones only as approved.
- [ ] Add fragmentation only after measured packet/airtime limits require it.
- [ ] Keep ordinary WadaMesh/MeshCore chat unchanged.
- [ ] Use existing chat presentation with minimal FriendMesh metadata and security indicators.
- [ ] Physically verify direct and relayed behavior on an approved test channel.

Exit rule: messages survive reboot, never bypass storage policy, and do not overclaim delivery or privacy.

### Phase 9 — Offline synchronization and lifecycle recovery

Goal: recover bounded history and membership state after devices return.

- [ ] Define inventory/high-water/gap exchange.
- [ ] Prioritize control and safety events over old chat.
- [ ] Throttle sync by channel utilization and duty-cycle requirements.
- [ ] Quarantine conflicting IDs or broken administrative chains.
- [ ] Enforce join/removal epochs on history access.
- [ ] Display incomplete history honestly when no replica retained it.
- [ ] Test packet loss, duplicates, reordering, long offline periods, and reboot during sync.

Exit rule: sync is resumable, bounded, and cannot block live mesh/UI processing.

### Phase 10 — Rekey, leave, kick, replacement, and succession

Goal: make group membership changes cryptographically meaningful.

- [ ] Implement reversible pending states before an irreversible boundary.
- [ ] Implement forward-only rekey journaling.
- [ ] Distribute new epoch material only to approved remaining identities.
- [ ] Handle offline members, changed identities, replacement, admin transfer, succession, and disband.
- [ ] Ensure removed devices cannot decrypt a new epoch.
- [ ] Test power loss at every transaction boundary and network partitions.

Exit rule: membership removal advances security state safely and recovery never returns to a compromised epoch.

### Phase 11 — Group map, navigation, markers, and meetups

Goal: layer field coordination onto proven identity/group behavior.

- [ ] Add group filters to the existing map.
- [ ] Add stale/hidden position policy and clear source/accuracy/age labels.
- [ ] Add arrow-only navigation when map tiles are unavailable.
- [ ] Add bounded breadcrumbs, markers, meetup proposals, votes, attendance, and rally points.
- [ ] Reuse existing WadaMesh map/contact surfaces and semantic styling.
- [ ] Test missing GPS, stale GPS, missing SD, missing tiles, and clock skew.

Exit rule: navigation is useful without claiming device-relative heading or GPS accuracy the hardware cannot provide.

### Phase 12 — SOS and Help Requests

Goal: add best-effort peer-assistance workflows after the reliable foundation exists.

- [ ] Define intentional hold/cancel activation and incident deduplication.
- [ ] Define location privacy, stale/no-location behavior, retry limits, recipient states, and closure.
- [ ] Allow an explicit standard MeshCore public-text fallback only after a clear privacy confirmation.
- [ ] Ensure emergency sends do not silently depend on SD history success.
- [ ] Use existing WadaMesh surfaces with fixed, unmistakable emergency semantics.
- [ ] State clearly that this does not contact emergency services and cannot guarantee delivery.
- [ ] Run physical multi-device drills under loss, congestion, reboot, and missing-GPS conditions.

Exit rule: safety language, state recovery, radio behavior, and failure modes are physically evidenced.

### Phase 13 — Exposure audit, hardening, and release

Goal: qualify the whole feature layer without weakening WadaMesh.

- [ ] Audit USB/TCP/WebSocket/BLE/web/MQTT, logs, backups, QR, screenshots, and exports for FriendMesh exposure.
- [ ] Run secret scans, parser fuzzing, storage fault injection, endurance, memory, stack, airtime, and battery campaigns.
- [ ] Verify ordinary WadaMesh chat, contacts, maps, companion use, update, recovery, and screenshots.
- [ ] Verify every added UI state with touch, keyboard, and trackball using existing WadaMesh themes.
- [ ] Verify optional themes independently if any are added.
- [ ] Produce reproducible T-Deck artifacts, checksums, migration/recovery instructions, limitations, and changelog.
- [ ] Require explicit release approval.

Exit rule: there are no unresolved critical security/reliability findings and documentation matches the shipped binary.

## Deferred work

The following are deliberately not part of current implementation:

- Heltec source changes, builds, or physical qualification;
- a FriendMesh replacement launcher, home screen, navigation system, or visual identity;
- a new FriendMesh firmware/repository brand replacing WadaMesh;
- Meshtastic module APIs, protobufs, Router/NodeDB assumptions, or `PRIVATE_APP` protocol copied into MeshCore;
- active Wi-Fi attacks, impersonation, deauthentication, credential capture, or disruptive RF tooling;
- guaranteed emergency dispatch, delivery, anonymity, remote deletion, or GPS accuracy;
- automatic secret creation during boot;
- general FriendMesh radio transmission before the required gates.

## Immediate next work

The next implementation phase is **Phase 1: status, diagnostics, and policy model**.

The first slice should:

1. add deterministic lifecycle/reason states to `FriendMeshFeatureService`;
2. keep all status output secret-free;
3. add host-runnable state and transmit-policy tests;
4. make no storage, radio, protocol, or UI changes;
5. rebuild only `LilyGo_TDeck_companion_radio_touch` and record the delta.

Protected storage begins only after that status/policy contract is stable.

## Session handoff template

```text
Date:
Branch / commit:
Roadmap phase:
Scope:
Files changed:

Implemented:
Intentionally disabled or deferred:

Host/static checks:
T-Deck build result and size:
T-Deck physical result:
Storage backend exercised:
Radio/protocol behavior exercised:
Companion/network surfaces exercised:

Secrets/redaction review:
Destructive or device-changing actions:

Known issues:
Next exact action:
```

## Definition of project success

FriendMeshOS features succeed when WadaMesh remains recognizably and operationally WadaMesh while gaining a trustworthy, optional set of friend, group, navigation, recovery, and peer-assistance capabilities on the T-Deck.

Success is not a new skin or a demo screen. It is bounded behavior with protected state, explicit consent, honest radio/security semantics, recovery evidence, ordinary WadaMesh regression coverage, and documentation that distinguishes implemented, build verified, physically verified, and still unfinished work.
