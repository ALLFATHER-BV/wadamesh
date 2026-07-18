# FriendMeshOS features — full implementation game plan

Status: canonical implementation contract; T-Deck backend foundation build-verified  
Updated: 2026-07-18  
Host project: WadaMesh  
Current target: LilyGo T-Deck / T-Deck Plus through `LilyGo_TDeck_companion_radio_touch`  
Protocol base: MeshCore/meshcomod `core-v1.16.5`  
Current FriendMesh radio state: transmit disabled

## 1. Purpose

This document defines how FriendMeshOS features are implemented inside WadaMesh.

FriendMeshOS is not a replacement firmware, new UI shell, repository takeover, or rebrand. It is the feature name for an optional friends-and-field-coordination layer built on WadaMesh's existing MeshCore runtime and mature device interface.

The intended feature family includes:

- protected FriendMesh identity and trust state;
- trusted contacts and verification;
- private FriendMesh groups and membership lifecycle;
- signed group events and chat;
- durable queues and bounded offline synchronization;
- Friend Compass, group map filters, navigation, markers, and meetups;
- SOS and non-emergency Help Requests;
- recovery, diagnostics, migrations, and honest security state.

The goal is to add these capabilities without disrupting ordinary WadaMesh chat, contacts, maps, settings, companion transports, OTA, recovery, UI, branding, or release identity.

## 2. Document authority

Use this document with:

- [`FRIENDMESHOS_ROADMAP.md`](FRIENDMESHOS_ROADMAP.md) for phase order and live completion state;
- [`FRIENDMESHOS_FEATURES.md`](FRIENDMESHOS_FEATURES.md) for the surveyed WadaMesh architecture and integration map;
- [`README.md`](README.md) for the host project's build and usage instructions;
- live source and current build output for actual implementation facts.

Authority rules:

1. Live compiled source owns current behavior.
2. This game plan owns approved FriendMesh behavior and safety gates.
3. The roadmap owns work order and completion tracking.
4. `FRIENDMESHOS_FEATURES.md` owns the repository survey and architectural context.
5. When they disagree, stop, inspect the source, record the decision, and update all affected documents before coding further.

## 3. Current starting base

### 3.1 Verified WadaMesh baseline

The current work begins from WadaMesh commit `95a9f05` on branch `friendmeshOS-proposal`.

The clean `LilyGo_TDeck_companion_radio_touch` build passed with:

- RAM: 89,372 / 327,680 bytes (27.3%);
- application flash: 3,251,813 / 4,063,232 bytes (80.0%);
- no flash or physical test in the baseline session;
- no Heltec build or test.

### 3.2 Implemented FriendMesh foundation

The implementation base is:

```text
platformio.ini
  T-Deck only: FRIENDMESH_FEATURES=1
  T-Deck only: +<friendmesh/*.cpp>

src/main.cpp
  MeshCore starts normally
  FriendMeshFeatureService begins afterward

src/friendmesh/
  FriendMeshFeatureService.h
  FriendMeshFeatureService.cpp
```

The service currently provides:

- a `LifecycleState` with `NotConfigured`, `Locked`, `Ready`, and `Degraded` vocabulary;
- a `TransmitState` fixed to `DisabledByFoundation`;
- a secret-free `StatusSnapshot` containing storage, identity, protocol, and consent readiness;
- an in-memory boot reset to `NotConfigured` with every readiness gate false;
- `canTransmit()` hard-coded false.

It does not:

- read or write SPIFFS, SD, NVS, preferences, or MeshCore storage;
- create, import, migrate, wrap, unwrap, or expose identity material;
- parse, send, receive, queue, relay, acknowledge, or alter FriendMesh packets;
- change radio state or ordinary MeshCore messages;
- register screens, labels, icons, themes, navigation, or settings;
- expose status through companion, web, MQTT, logging, or backups.

The foundation build passed with:

- RAM: 89,380 / 327,680 bytes (27.3%);
- application flash: 3,251,845 / 4,063,232 bytes (80.0%);
- delta: +8 bytes RAM and +32 bytes flash.

This is the only approved starting base for new FriendMesh code in this repository.

## 4. Product and UI rules

### 4.1 WadaMesh remains the product shell

Preserve:

- WadaMesh name, wordmark, boot presentation, icons, terminology, and attribution;
- current main tabs, command center, app drawer, status bar, settings, control center, chat, contacts, and map;
- touch, keyboard, trackball, focus, back, and on-screen-keyboard behavior;
- WadaMesh release identifiers, feeds, web flasher, OTA behavior, and repository structure.

FriendMesh may add only the smallest suitable integration:

- a status line or badge;
- an action in an existing contact or map detail;
- an existing-settings row;
- chat metadata or a confirmation modal;
- a group filter or bounded drawer/app entry when the feature truly requires it.

Do not add:

- a replacement FriendMesh launcher or home screen;
- parallel navigation;
- repo-wide FriendMesh naming;
- a required FriendMesh visual skin;
- separate copies of existing WadaMesh chat, map, contacts, settings, or update flows.

Optional new themes are allowed only through WadaMesh's existing theme mechanism. They remain optional and cannot become feature dependencies or replace WadaMesh branding.

### 4.2 Existing features are inherited, not reimplemented

FriendMesh can build on:

- MeshCore chat, routing, contacts, positions, and companion behavior;
- WadaMesh message history and map presentation;
- T-Deck touch, keyboard, and trackball;
- SD/SPIFFS infrastructure;
- background workers and watchdog protection;
- USB/TCP/WebSocket/BLE, web mirror, MQTT, OTA, and diagnostics;
- built-in SD screenshot capture by holding the status bar for three seconds.

Every inherited remote/export surface must be reviewed before it is allowed to expose FriendMesh state.

## 5. Hardware and scope contract

### 5.1 Current target

Only the T-Deck environment is in scope:

```bash
/Users/tylersmith/.platformio/penv/bin/pio run -e LilyGo_TDeck_companion_radio_touch
```

Do not use a plain `pio run` for FriendMesh work because repository defaults include other boards.

### 5.2 T-Deck capabilities

| Capability | FriendMesh rule |
|---|---|
| 320×240 touch display | reuse existing WadaMesh layouts and focus model |
| Keyboard and trackball | enhancements allowed; all required actions also need touch paths |
| SX1262 | ordinary MeshCore owns radio until a reviewed FriendMesh integration exists |
| GPS | optional at runtime; stale/missing state must be visible |
| microSD | optional expansion; essential identity/group access cannot depend solely on it |
| SPIFFS/internal storage | candidate essential store; must be protected and transactional |
| Speaker/tones | optional notification implementation; not a protocol dependency |
| Screenshot capture | inherited WadaMesh tool; sensitive screens must not leak secrets |
| PSRAM | useful for bounded caches/workers; not permission for unbounded state |

### 5.3 Deferred hardware

Heltec environments remain documented for possible future access but are not current implementation, build, or acceptance targets. Do not modify them speculatively or claim compatibility from shared-source appearance.

## 6. Architecture contract

### 6.1 Ownership

Keep FriendMesh responsibilities separated:

```text
src/friendmesh/
  FriendMeshFeatureService.*   lifecycle, composition, readiness, policy
  status/                      secret-free status and bounded diagnostics
  storage/                     protected records, providers, recovery, migration
  security/                    signing identity, verification, key lifecycle
  domain/                      contacts, groups, members, events, incidents
  protocol/                    MeshCore-facing codec, replay, policy, queues
  navigation/                  distance, bearing, freshness, heading abstractions
  ui/                          narrow presenters/controllers for existing WadaMesh UI
```

Directories are added only when their phase begins; do not scaffold empty architecture for appearance.

### 6.2 Integration seams

- `FriendMeshFeatureService` owns the feature lifecycle and readiness policy.
- `src/main.cpp` remains composition-only and must not become feature logic.
- `MyMesh` receives a narrow reviewed interface at exact send/receive points; it does not own storage or UI.
- `UITask` consumes immutable/secret-free snapshots and invokes bounded actions; it does not perform KDFs, migrations, recovery scans, or large writes.
- board/variant code handles actual hardware quirks only.
- changes below the external `BaseChatMesh`/MeshCore boundary require a deliberate core-tag update and separate review.

### 6.3 Cooperative runtime

WadaMesh's main loop and LVGL callbacks must remain responsive. Crypto, filesystem scans, compaction, imports, exports, and recovery execute in bounded slices or worker tasks using existing synchronization patterns. No FriendMesh task may monopolize the shared SPI bus, radio, main loop, or companion stream.

## 7. Lifecycle and transmit policy

### 7.1 Required lifecycle states

The final service should distinguish at least:

- `Unavailable` — build/runtime capability absent;
- `NotConfigured` — no protected FriendMesh state exists;
- `Locked` — protected state exists but is not unlocked;
- `Ready` — required local gates are satisfied;
- `Degraded` — limited safe operation remains available;
- `RecoveryRequired` — inconsistency requires an explicit recovery action.

Reason codes must be bounded, stable, and secret-free.

### 7.2 Transmit gates

FriendMesh transmission remains false unless every required gate is true:

```text
feature compiled and enabled
AND protected storage ready
AND FriendMesh identity ready
AND schema/migration complete
AND recovery/journal clean
AND protocol version supported
AND replay state durable
AND radio policy permits the operation
AND destination/group authorization valid
AND explicit user consent valid
AND durable-outbox policy satisfied for the event type
```

The current foundation deliberately bypasses this expression by returning false unconditionally. Replacing that hard gate requires an approved phase, tests for every false branch, and explicit review.

No UI state, theme, setting default, successful build, unlocked screen, or storage probe may enable transmit as a side effect.

### 7.3 Diagnostics

Diagnostics may expose:

- lifecycle and reason code;
- schema version;
- provider type and capacity class;
- locked/unlocked state;
- recovery/degraded flags;
- bounded queue counts and ages;
- protocol version and compatibility state;
- redacted event/result codes;
- build/version information.

Diagnostics must not expose:

- PINs, KDF outputs, private keys, group keys, nonces, plaintext records, recovery material, channel secrets, exact protected filenames, or raw decrypted payloads;
- sensitive coordinates or friend/group names without an explicit user-facing reason;
- secrets through Serial, companion broadcasts, MQTT, web mirror, backups, QR codes, screenshots, crash logs, or exports.

## 8. Protected storage contract

### 8.1 Storage roles

Internal storage must hold the minimum required protected state for identity, group access, replay, transactions, and safe recovery. SD may expand history, drafts, maps, breadcrumbs, diagnostics, and explicit exports but cannot be the only copy of essential live security state unless the product explicitly supports an SD-bound mode.

### 8.2 PIN and key rules

- FriendMesh storage PIN is independent from BLE PIN and display lock.
- PIN policy and retry behavior must be explicit.
- A reviewed embedded-appropriate KDF derives/wraps key material with versioned parameters.
- Per-purpose subkeys separate identity, group state, history, outbox, replay, contacts, and incidents.
- AEAD protects every sensitive record; encryption without authentication is insufficient.
- Nonces/sequence numbers must remain unique across reboot, interrupted writes, and recovery.
- Device binding must be stable across normal WadaMesh OTA and explicit about what breaks after factory reset or identity replacement.
- Plaintext fallback is forbidden.

### 8.3 Startup and provisioning

Startup performs a read-only probe:

1. detect provider and record presence;
2. validate public framing/size/schema bounds before expensive work;
3. classify empty, locked, ready, degraded, corrupt, unsupported, or recovery-required state;
4. expose a secret-free status;
5. perform no key creation or destructive repair.

Creating, importing, migrating, rewrapping, repairing, or erasing protected state requires an explicit user action with a confirmation appropriate to its consequence.

### 8.4 Transaction and recovery rules

- Use authenticated two-slot records or an append-only journal with generations.
- Verify writes by readback before committing state.
- Never select two divergent records with the same generation silently.
- Preserve a last-known-good path for essential records.
- Recovery resumes forward after an irreversible security boundary.
- Corrupt material is quarantined or rejected without logging plaintext.
- Power loss at every write boundary is a required test case.
- Storage-full, missing, removed, read-only, corrupt, and wrong-schema states must be explicit.

## 9. Identity and trust contract

### 9.1 Separate concepts

Do not conflate:

- MeshCore node identity;
- FriendMesh signing identity;
- group keys;
- storage PIN;
- BLE PIN;
- screen lock;
- a human friend/contact.

The exact binding between MeshCore identity and FriendMesh signing identity must be documented before identity creation ships.

### 9.2 FriendMesh identity

The planned identity provides attributable signatures for FriendMesh events. Requirements:

- reviewed signature primitive and published test vectors;
- strong random generation;
- explicit creation/import/restore;
- protected private material at rest;
- private material wiped from temporary buffers and locked RAM state;
- public fingerprint available for verification;
- stable continuity across normal reboot/OTA;
- explicit replacement/revocation semantics;
- no derivation from group secrets or reuse of incompatible MeshCore key bytes.

### 9.3 Trusted contacts

A FriendMesh trusted contact references, rather than overwrites, an existing MeshCore contact. Planned fields include:

- stable MeshCore contact identifier/fingerprint;
- FriendMesh signing fingerprint;
- human-selected display label or group alias;
- verification state and method;
- first/last verified timestamps where trustworthy;
- replacement/revocation history;
- optional emergency-contact consent;
- last authenticated activity and position freshness metadata.

Local blocking, favorites, or labels do not change remote group membership or cryptographic access.

## 10. Group domain contract

Initial planning assumes up to eight configured groups and eight physically tested members per group. These are targets, not promises, until memory and airtime measurements confirm them.

### 10.1 Group fields

- random opaque group ID;
- display name;
- current security/membership epoch;
- current approved admin identity;
- ordered member records and roles;
- security/readiness state;
- activity timestamp and clock-quality metadata;
- administrative sequence and previous-event hash;
- pending transactions, replay state, and tombstones;
- location-sharing and safety policies;
- optional active meetup/rally state.

### 10.2 Member fields

- bound MeshCore and FriendMesh public identity references;
- group-specific alias;
- role;
- join order/event;
- membership status;
- epoch admitted/removed;
- last authenticated activity;
- replacement/revocation state.

### 10.3 Administrative events

Membership and security changes require:

- a monotonic administrative sequence;
- previous-event hash or equivalent chain;
- group and membership epoch;
- authorized signature or approved quorum certificate;
- durable transaction state before transmission;
- replay and stale-epoch rejection.

## 11. MeshCore protocol contract

The prior FriendMesh design was Meshtastic-based. Its `PRIVATE_APP`, protobuf, NodeDB, channel, Router, ACK, and module assumptions are not implementation authority here.

Before any production radio integration, define against the pinned MeshCore source:

- exact outer packet/application representation;
- version/magic and forward-compatibility behavior;
- routing/relay semantics;
- sender and destination representation;
- signature canonicalization;
- encryption and key separation;
- group dispatch without unnecessary metadata disclosure;
- event ID, sender sequence, epoch, replay window, and expiration;
- fragmentation/reassembly limits;
- packet, queue, duty-cycle, and airtime limits;
- ACK/NAK meaning and UI language;
- unsupported-peer behavior;
- direct, group, broadcast, and public fallback policy;
- receive/transmit hooks in `MyMesh` and any required external-core changes.

Canonical byte vectors and rejection vectors are required before on-air tests. Parser fuzzing, malformed length checks, unknown version/type handling, duplicate/replay rejection, and downgrade behavior are release gates.

## 12. Event and chat contract

Potential event families include:

- chat message, reaction, and deletion tombstone;
- membership and identity changes;
- key/rekey/control events;
- sync inventory, range, batch, receipt, and gap events;
- position/freshness metadata;
- marker and meetup events;
- Help Request and SOS incidents.

Exact wire types remain unapproved until Phase 6.

### 12.1 Durable outbox

- Ordinary FriendMesh messages do not transmit unless their durable outbox record succeeds.
- Security/membership events do not transmit unless their transaction record is durable.
- SOS/Help may use an explicitly designed storage-bypass path when persistence fails, while warning that local history was not saved.
- Queues are bounded by records, bytes, age, retry, and airtime.
- Reboot recovery cannot duplicate an event ID or nonce.
- User-visible states distinguish queued, transmitting, relayed/observed, delivered when provable, timed out, expired, and failed.

### 12.2 Chat presentation

Reuse the WadaMesh chat design. FriendMesh additions may show:

- verified alias/fingerprint state;
- security/group epoch status;
- queue/delivery detail;
- delayed or incomplete state;
- signed system-event rows;
- bounded reactions and tombstones if approved.

Do not redesign global chat navigation or relabel ordinary MeshCore messages as cryptographically stronger FriendMesh messages.

## 13. Offline synchronization

Synchronization is best effort among devices that retained records. It cannot recover history no device possesses.

Requirements:

1. exchange compact inventory/high-water summaries;
2. calculate bounded missing ranges;
3. prioritize active safety, security, membership, and newest chat events;
4. throttle for channel use, duty cycle, power, and live traffic;
5. persist and validate before acknowledging;
6. resume after interruption;
7. enforce join/removal epochs;
8. quarantine conflicting event IDs or broken chains;
9. display incomplete history honestly.

Original sender time, local receive time, sync time, and clock quality should remain distinguishable.

## 14. Rekey and membership lifecycle

Planned lifecycle operations include join, leave, kick, device replacement, admin transfer, succession, expiration, and disband.

Security rules:

- reversible countdowns end at a clearly marked irreversible boundary;
- once new epoch material is distributed, recovery moves forward, not back to the old key;
- removed identities never receive new epoch material;
- offline approved members remain rekey-pending until verified delivery/receipt;
- grants bind group, epoch, target identity/fingerprint, and authorization;
- changed identities require fresh approval;
- old keys are never selected for new transmission;
- any retired-key observation uses cautious evidence language, not physical-attribution claims.

## 15. Friend Compass and navigation

Friend Compass uses positions already available to WadaMesh/MeshCore. It does not claim Totem protocol compatibility.

### 15.1 Local MVP

- select a trusted existing contact;
- read local and remote coordinates from reviewed WadaMesh state;
- compute great-circle distance and absolute bearing;
- show target name, distance, bearing, position age, source, accuracy, and last-heard information;
- show stale/missing positions clearly;
- compare calculations against deterministic geographic vectors and a trusted map.

### 15.2 Heading fallback

1. supported calibrated magnetometer, if one is later available: `MAG`;
2. valid GPS course while moving: `GPS`;
3. otherwise absolute north-up bearing: `NORTH-UP`.

Never claim stationary device-relative direction from GPS course. Arrival radius should account for reported accuracy and never promise one-meter precision.

### 15.3 UI integration

Add navigation through existing contact details and map actions. Reuse WadaMesh map/no-tile behavior. A separate FriendMesh home screen is not allowed.

## 16. Markers and meetups

Planned group coordination may include:

- ordinary markers with type, creator, timestamp, expiry, and navigation action;
- one bounded active meetup proposal with votes and expiry;
- attendance/responding state;
- an admin-controlled rally point;
- private return-to-start and breadcrumbs where storage permits.

Sensitive incident markers remain FriendMesh-only. Ordinary marker interoperability with existing MeshCore behavior must be evaluated rather than assumed.

All marker/meetup operations require clock-skew, stale position, missing GPS, duplicate, offline vote, and cancellation behavior.

## 17. SOS and Help Requests

These are best-effort peer-assistance features, not emergency-service dispatch.

### 17.1 SOS activation

- intentional long hold;
- visible cancel/privacy countdown;
- one deduplicated incident ID;
- exact, last-known, or missing-location state shown honestly;
- bounded retries and low-rate active updates subject to radio policy;
- recipient states such as delivered when provable, responding, unable, arrived, and closed;
- explicit closure reasons;
- optional public standard-MeshCore text fallback only after a separate privacy confirmation.

### 17.2 Help Requests

Help Requests are urgent but below SOS. Planned categories may include ride, lost/separated, equipment, medical non-emergency, call/contact, discreet extraction, and free text. Silent presentation, location sharing, responses, escalation, de-escalation, and closure require explicit policy.

### 17.3 Required language

The UI and documentation must say:

- delivery is not guaranteed;
- this does not contact emergency services;
- positions may be stale or inaccurate;
- interference, range, power, congestion, and offline devices may prevent receipt;
- public fallback can expose identity/location;
- local storage failure may prevent incident history even if a transmission is attempted.

## 18. Remote, export, backup, and screenshot exposure

WadaMesh has multiple paths that could accidentally expose FriendMesh data:

- USB serial and binary companion frames;
- TCP and WebSocket companion connections;
- BLE;
- web mirror and browser terminal;
- MQTT;
- logs/crash output;
- backups and identity export;
- QR codes;
- SD files and explicit exports;
- screenshots.

For every new datum, define an exposure matrix before integration:

| Surface | Default | Requirement |
|---|---|---|
| On-device status | secret-free | show only what the user needs |
| Serial/log | denied for secrets | stable redacted reason codes only |
| Companion APIs | denied until specified | authentication and field-level contract |
| Web/MQTT | denied until specified | explicit policy; no accidental broadcast |
| Backup/export | denied until specified | encryption, warning, version, restore test |
| Screenshot | allowed for normal UI | never render private keys/PINs/recovery material |

The inherited screenshot feature captures the composited screen to `/screenshots` on SD. Sensitive workflows must mask secret entry and avoid displaying recoverable secret material at all.

## 19. Testing and evidence contract

### 19.1 Every implementation slice

1. inspect live source and dependency boundaries;
2. document assumptions and exact phase;
3. implement the smallest coherent change;
4. add host tests where behavior is platform-neutral;
5. run static/diff checks;
6. build only `LilyGo_TDeck_companion_radio_touch`;
7. record RAM/flash and compare with the last baseline;
8. perform physical/device-changing work only with explicit approval;
9. record implemented, verified, physically verified, and unfinished states separately;
10. update the roadmap/game plan when facts change.

### 19.2 Required automated campaigns

- lifecycle and transmit-policy branch tests;
- published crypto vectors;
- deterministic codec vectors;
- malformed/truncated/mutated record and packet tests;
- replay, duplicate, stale epoch, wrong identity, and wrong authorization tests;
- storage fault injection at every transaction boundary;
- nonce/sequence uniqueness across reboot simulations;
- queue bounds and expiry;
- parser fuzzing;
- secret scans of logs, fixtures, exports, and built artifacts where practical;
- deterministic distance/bearing and clock-skew tests.

### 19.3 Required physical campaigns

Only after approval and only for the named phase:

- original T-Deck and T-Deck Plus results recorded separately when both are available;
- ordinary WadaMesh boot, touch, keyboard, trackball, chat, contacts, map, companion, screenshot, OTA/recovery regression as applicable;
- SD absent/present/full/corrupt/read-only/removed scenarios;
- reboot and power interruption at approved boundaries;
- direct and relayed radio behavior;
- loss, duplication, reordering, congestion, and offline peers;
- stale/missing GPS;
- safety drills for SOS/Help;
- existing-theme coverage for each added UI state.

### 19.4 Evidence language

- `Source reviewed` is not `implemented`.
- `Implemented` is not `tested`.
- `Host verified` is not `T-Deck build verified`.
- `T-Deck build verified` is not `physically verified`.
- `Physically verified on one T-Deck` is not proof for T-Deck Plus or Heltec.
- Prior Meshtastic FriendMesh evidence does not prove MeshCore FriendMesh behavior.

## 20. Master phase tracker

- [x] Phase 0 — Define WadaMesh/FriendMesh boundary and record clean T-Deck baseline.
- [x] Phase 0 — Add T-Deck-only backend service with transmit hard-disabled.
- [ ] Phase 0 physical gate — smoke-test unchanged WadaMesh on the development T-Deck.
- [ ] Phase 1 — Status, diagnostics, and policy model.
- [ ] Phase 2 — Protected storage primitives and recovery.
- [ ] Phase 3 — Protected FriendMesh signing identity.
- [ ] Phase 4 — Trusted contacts and local Friend Compass.
- [ ] Phase 5 — Group domain model.
- [ ] Phase 6 — MeshCore FriendMesh wire contract.
- [ ] Phase 7 — Nearby invitation and verification.
- [ ] Phase 8 — Signed group chat and durable outbox.
- [ ] Phase 9 — Offline synchronization.
- [ ] Phase 10 — Rekey and complete membership lifecycle.
- [ ] Phase 11 — Group map, navigation, markers, and meetups.
- [ ] Phase 12 — SOS and Help Requests.
- [ ] Phase 13 — Exposure audit, hardening, recovery, and release qualification.

Current implementation focus: **Phase 1 — status, diagnostics, and policy model**. No storage, identity, UI, protocol, or transmit expansion is authorized merely by this plan.

## 21. Deferred and prohibited assumptions

Do not assume:

- Meshtastic protobufs, `PRIVATE_APP`, NodeDB, channel PSKs, Router, or ACK behavior apply to MeshCore;
- MeshCore contact identity equals FriendMesh signing identity;
- queue/relay observation proves end-recipient delivery;
- SD is always present;
- GPS supplies stationary device heading;
- screenshots, companion access, web, MQTT, or backups are private;
- a WadaMesh UI feature implies a FriendMesh security feature exists;
- a previous physical FriendMesh test applies to this new base;
- Heltec compatibility exists because code looks portable.

Do not implement active Wi-Fi attacks, impersonation, credential capture, deauthentication, disruptive RF tools, guaranteed emergency claims, or silent plaintext/security downgrade as part of FriendMeshOS features.

## 22. Release warning

Until final qualification, describe FriendMeshOS features as incomplete development work. Do not rely on unfinished FriendMesh group security, history synchronization, emergency alerts, key rotation, or location sharing for personal safety, secure production messaging, or emergency response.

Ordinary WadaMesh/MeshCore functionality retains its own maturity and limitations; the warning should not falsely imply that dormant FriendMesh scaffolding replaces or disables it.

## 23. Definition of done

The FriendMesh feature layer is complete only when:

- every required phase has recorded automated, build, recovery, documentation, and physical evidence;
- cryptographic and protocol behavior has deterministic vectors and rejection tests;
- protected storage survives fault injection without plaintext fallback;
- transmit gates cannot be bypassed by UI or corrupt state;
- ordinary WadaMesh/MeshCore functionality remains intact;
- queues, history, storage, memory, and radio use are bounded;
- UI additions are minimal, consistent with existing WadaMesh navigation/branding/themes, and work by touch, keyboard, and trackball;
- privacy, delivery, GPS, sync, and emergency limitations are stated honestly;
- migration, backup, recovery, and rollback behavior are verified;
- release artifacts are reproducible and documented;
- the user explicitly approves release.

## 24. Session record template

```text
Date:
Branch / commit:
Phase and exact slice:
Files changed:

Behavior implemented:
Behavior intentionally disabled/deferred:

Host tests:
Static checks:
T-Deck build and size:
T-Deck physical evidence:
Ordinary WadaMesh regression evidence:

Storage/provider exercised:
Radio/protocol exercised:
Remote/export surfaces reviewed:
Secrets/redaction result:

Device-changing/destructive actions:
Known issues:
Next exact action:
```
