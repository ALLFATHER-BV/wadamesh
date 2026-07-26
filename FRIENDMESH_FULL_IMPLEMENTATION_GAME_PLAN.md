# FriendMeshOS features — full implementation game plan

Status: canonical implementation contract; T-Deck functional integration build-verified
Updated: 2026-07-19
Host project: WadaMesh  
Current target: LilyGo T-Deck / T-Deck Plus through `LilyGo_TDeck_companion_radio_touch`  
Protocol base: MeshCore/meshcomod `core-v1.16.5`  
Current FriendMesh radio state: narrow channel compatibility traffic active; production FriendMesh event transport disabled

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

### 3.3 Shared application-core foundation

The next foundation is now implemented and host/build verified:

- `core/FriendMeshCoreTypes.*` defines fixed capacities, IDs, results, roles, lifecycle vocabulary, and shared bounds;
- `core/FriendMeshDomain.*` defines bounded friends, identity references, groups, members, aliases, membership epochs, roles, blocking, and admin transfer behavior;
- `core/FriendMeshEvent.*` defines one event vocabulary and policy table for identity/trust, groups, chat, sync, location, navigation, markers, meetups, SOS, Help, and control;
- `core/FriendMeshFeatureModels.h` defines shared typed records for chat, reactions, sync, positions, navigation, markers, meetups, and incidents;
- `core/FriendMeshProviders.h` defines storage, security, transport, clock, location, and notification interfaces;
- `core/FriendMeshEventHistory.h` and `core/FriendMeshOutbox.h` provide fixed-capacity history and delivery-state machinery;
- `scripts/test-friendmesh-core.sh` host-checks every feature family and the hard transmit gate.

The T-Deck build passes at 89,404 bytes RAM and 3,252,277 bytes flash. This adds 24 bytes RAM and 432 bytes flash over the dormant-service baseline because maximum domain/history/outbox stores are types used by tests and future coordinators, not global runtime allocations.

### 3.4 Functional people/groups/membership cluster

Phase 2 is implemented and host/build verified. `people/FriendMeshTrustedContacts.*`
defines the non-owning directory seam for existing WadaMesh/MeshCore contacts,
and `people/FriendMeshMembership.*` implements development identity lifecycle,
invitations, verification transcripts, candidate review, group membership,
leave/kick countdowns, replacement, administrative transfer, recorded majority
succession, and disbanding.

Membership-epoch changes are one shared transaction model. Excluded identities
are revoked, remaining approved members become grant-pending, and a rekey cannot
complete until every approved member has an active development grant for the new
epoch. These grant records are functional placeholders, not key material or a
security claim.

`scripts/test-friendmesh-core.sh` covers multi-group behavior, authorization,
duplicates, expiry, reversible and irreversible boundaries, replacement,
succession votes, disbanding, and every declared capacity. The T-Deck build
passes at 89,404 bytes RAM and 3,253,533 bytes flash. There is still no live
contact binding, persistence, UI, production security, FriendMesh radio path, or
physical verification, and transmission remains hard-disabled.

### 3.5 Functional chat/history/synchronization cluster

Phase 3 is implemented and host/build verified in `friendmesh/chat/`:

- `FriendMeshChat.*` implements messages, durable replies, reactions, authorized
  deletion tombstones, unread/mute state, delayed and untrusted-clock metadata,
  and durable delivery transitions;
- `FriendMeshDevelopmentStorage.*` supplies fixed-capacity development payload,
  event-journal, and outbox providers with internal/expansion placement and
  explicit failure simulation;
- `FriendMeshFragmentation.h` provides 192-byte bounded fragments and
  out-of-order reassembly;
- `FriendMeshSync.*` provides inventory, per-sender high-water and gap tracking,
  missing ranges, range/priority batches, receipts, deduplication, and quarantine;
- the host suite exercises capacity, loss-shaped gaps, duplicates, reordering,
  offline delay, retries, cancellation, expiry, reboot reconstruction,
  authorization conflicts, storage failures, and incomplete history.

The T-Deck build passes at 89,404 bytes RAM and 3,254,685 bytes flash. These are
development models: no production filesystem/SD adapter, cryptography, UI,
MeshCore radio path, Heltec work, or physical verification exists, and
transmission remains hard-disabled.

### 3.6 Native private-group compatibility integration

The current T-Deck source also binds selected functional behavior to existing
WadaMesh/MeshCore surfaces while the production FriendMesh transport gate stays
disabled:

- nearby private-channel key delivery uses encrypted zero-hop MeshCore direct
  messages after bounded BLE discovery or a fresh direct advert;
- an eight-member persisted roster synchronizes through reserved encrypted
  group data and authorizes the bounded existing-telemetry location refresh;
- `Group map` and `Friend Compass` reuse WadaMesh's map and contact positions;
- `Coordinate` shares one meetup/pickup and one Help/SOS incident per channel,
  including member responses and cancellation/closure;
- FriendMesh actions require explicit persisted group metadata; ordinary
  MeshCore channels do not inherit them, and the UI marks group rows/titles
  `[FM]` without modifying the underlying channel name;
- coordination events use fixed type `0xFF02`, are consumed before chat, and
  persist in at most 248 bytes per channel;
- SOS requires a continuous three-second hold.

This compatibility integration uses the MeshCore channel key and local roster;
it does not replace the production signing, replay, protected storage, grant,
or rekey requirements below. A retained channel-key holder can spoof the claimed
roster prefix. The host suite and focused T-Deck build pass at 90,908 bytes RAM
(27.7%) and 3,301,141 bytes flash (81.2%). Physical verification of this newest
coordination slice remains open; no device was flashed during its implementation.

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

### 4.3 Feature-complete-first development rule

This project is not targeting an MVP. Implement the planned feature behavior through shared application code first, then apply the production security and reliability implementation across the complete system.

- Friends, groups, invitations, membership, verification state, replacement, and rekey-required behavior are one cluster.
- Chat, reactions, deletion, outbox, history, and synchronization are one cluster.
- Position freshness, compass, map filters, navigation, breadcrumbs, markers, and meetups are one cluster.
- SOS, Help Requests, incidents, location policy, retries, and notifications are one cluster.
- Identity cryptography, protected storage, signing/encryption, verification proof, replay, grants, rekey, transaction recovery, and exposure controls are one later shared security cluster.

Production security is implemented after functional feature completeness, but the provider boundaries, stable IDs, event policy, bounded state, and hard transmit gate exist from the start. Development-only providers may support host/local simulations; they must never enable production FriendMesh radio transmission or be represented as secure.

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
  core/                        shared domain, events, models, providers, history, outbox
  status/                      future secret-free status and bounded diagnostics
  storage/                     protected records, providers, recovery, migration
  security/                    signing identity, verification, key lifecycle
  domain/                      contacts, groups, members, events, incidents
  protocol/                    MeshCore-facing codec, replay, policy, queues
  navigation/                  distance, bearing, freshness, heading abstractions
  ui/                          narrow presenters/controllers for existing WadaMesh UI
```

Directories are added only when their shared feature or security cluster begins; do not scaffold empty architecture for appearance.

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

The FriendMesh T-Deck explicitly supports an SD-bound mode and now makes it the required operating policy. The authoritative identity, group access, settings, contacts, channels, FriendMesh state, history, maps, motion records, backups, and other growing datasets live on microSD. Internal storage may retain a readable migration/recovery source and small boot-control values, but it must not silently become a writable long-term fallback when the card is absent. Production protected storage must therefore bind, authenticate, recover, and clearly report the removable medium; missing, corrupt, replaced, or partially migrated cards must fail closed before FriendMesh transmission or protected-state mutation.

The current functional migration copies critical identity/profile paths explicitly, recursively scans additional SPIFFS paths using their full names, never overwrites existing non-empty SD files during automatic boot repair, verifies the SD identity before adoption, and leaves the internal recovery source intact. This repairs the observed `0 copied, identity MISSING` failure caused by relying on a root-only SPIFFS directory walk.

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

### 10.4 Nearby direct joining

Nearby direct joining is the default onboarding experience. The expected flow is
`Private group` -> `Invite nearby` -> direct device observed -> compare short
number -> administrator approval -> `securing group` -> ready.

The current implementation is an earlier compatibility bridge: an existing
WadaMesh private channel can discover an updated saved contact with a bounded
Bluetooth scan, with a recent zero-hop LoRa advert as fallback, then send the
channel's symmetric secret through MeshCore's existing encrypted direct-message
path. Bluetooth carries only a public-key prefix, never the secret. The outbound
LoRa route is empty, relayed invite packets are rejected, and the recipient
explicitly confirms before saving the channel. This provides usable group chat
without introducing a parallel key transport, but it does not satisfy the
stronger production flow below.

The bridge now also attaches a fixed eight-member roster to the existing
channel. The WadaMesh channel action sheet shows invite/join/failure/leave/remove
states. Direct encrypted acknowledgements and cooperative leave/removal notices
update the owner, while compact encrypted channel snapshots keep joined devices'
functional rosters aligned without showing control text in chat. Metadata is
persisted through the existing bounded blob store. Removal sets a prominent
`rekey required` state because the old shared key is intentionally not presented
as revoked.

The direct exchange requires forwarding disabled and zero observed receive hops.
Sender-side Bluetooth presence or a fresh zero-hop advert is a bounded discovery
gate only. Neither RSSI nor zero-hop RF proves physical distance, identity, or
confidentiality. The comparison transcript and administrator approval remain
independent future gates.

No identity private key is ever transmitted. Production security establishes a
transcript-bound ephemeral pairwise session and sends only the new group epoch
key inside an authenticated, member-specific encrypted grant. A successful join
advances the membership epoch so the new member does not automatically receive
past-history access. Full UX, protocol, rejection, and recovery requirements are
in `FRIENDMESH_NEARBY_JOIN_PROTOCOL.md`.

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

Production FriendMesh wire types remain unapproved. The Phase 6 compatibility
bridge reserves encrypted MeshCore group-data types `0xFF01` for roster state
and `0xFF02` for bounded coordination records; neither is a production signing
or membership protocol commitment.

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

Lifecycle operations include the native development bridge's join, leave,
cooperative removal, and administrator disband flow. Device replacement, admin
transfer, succession, expiration, and cryptographic disband recovery remain in
the shared production protocol.

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

### 15.1 Local functional baseline

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

Current functional integration: an existing private channel exposes `Group
map`, which uses its bounded joined roster to filter WadaMesh's existing map and
positioned-contact list. A selected positioned member exposes `Friend Compass`
with north-up bearing, distance, position age, and explicit stale/missing states.
The detailed dial can toggle to headway-up after two reliable local GPS fixes;
that mode rotates its pointer, plot, cardinal labels, and guidance arrow around
the user's calculated course while retaining north-up as the stationary-safe
fallback.
It retains only the previous and current plausible fix for each bounded group
member plus the local user on microSD, then shows the observed trail separately from a conservative
45-second lead point. Noise, stale samples, and implausible movement fall back
to the observed position. The versioned checksummed SD snapshot has no
internal-flash fallback and is not yet encrypted or authenticated.
The T-Deck exposes that calculation through two WadaMesh-native pages: a larger
detailed spatial dial and a plain-language direction/data page. The first page
uses labeled distance-scaled rings, compact movement/confidence/freshness
status, and a defensively bounded 45-second lead readout. Horizontal
swipe, trackball left/right, or two tappable dots switch pages. The second page
can temporarily guide to the latest observed position instead of the predicted
meeting point. With two recent local fixes separated by at least eight meters,
it reports the user's GPS course over ground, turn-left/turn-right correction,
closing or opening speed, progress state, and an ETA only while closing. The
guidance page translates correction angles into slight, standard, sharp, and
turn-around language with a matching maneuver-shaped arrow, plus an explicit
absolute north-arrow mode. Stale
targets pause headway, and the UI never presents GPS course as a stationary
magnetic heading.
Once both endpoints have valid positions, opening the Compass attempts one
targeted authenticated MeshCore contact notice. The selected member is shown
who started navigating and the current straight-line distance. Receivers verify
the sender and themselves against the local joined roster, then consume the
compact envelope before chat history; it is not a user message or group
broadcast.
The bridge reads existing MeshCore contact coordinates through a narrow E6-to-E7
adapter. On map open it requests joined members sequentially through MeshCore's
existing encrypted telemetry protocol, with one shared response slot, a bounded
roster, and a 30-second per-contact ceiling. The receiver grants location when
the authenticated contact is locally recorded as joined in at least one
FriendMesh channel, independent of public advert-location sharing. It does not
transmit a parallel location event or run after Map closes. Production identity
binding, transcript-bound grants, and finer location-sharing policy remain in
the shared security/protocol phases.

The shared-core motion/course checks and the combined T-Deck build pass at 87,020 bytes
RAM (26.6%) and 3,293,117 bytes flash (81.0%) after the SD-required persistence
and duplicate-asset cleanup pass. The first-page timing formatter
regression is covered by a fixed-horizon assertion. Physical two-device motion and local headway,
paging, layout, scroll, SD reboot-retention, and targeted start-notification
checks remain open.

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
- [x] Phase 1 — Shared application-core types, models, event policy, providers, history, outbox, and host checks.
- [x] Phase 1 completion — deterministic service reason model and non-global coordinator composition.
- [x] Phase 2 — People, groups, invitations, and complete functional membership behavior.
- [x] Phase 3 — Chat, reactions, outbox, history, and synchronization behavior.
- [x] Phase 4 — Functional positions, Friend Compass, map/navigation, markers, and meetups services.
- [x] Phase 5 — Functional SOS, Help Requests, incidents, and notifications services.
- [ ] Phase 6 — Complete minimal integration into existing WadaMesh UI surfaces.
- [ ] Phase 7 — Shared production identity, protected storage, crypto, replay, grants, rekey, and recovery.
- [ ] Phase 8 — MeshCore protocol and controlled radio integration.
- [ ] Phase 9 — Whole-system security fixes, hardening, exposure audit, and recovery campaign.
- [ ] Phase 10 — Physical feature qualification and release.
- [ ] Physical baseline gate — smoke-test unchanged WadaMesh behavior on the development T-Deck.

Current implementation focus: **physically validate and finish the minimal
multi-device WadaMesh surfaces before shared production security**. Phase 4 and
Phase 5 functional services are in the T-Deck build. Private channels now expose
nearby join, members, Group map, Friend Compass, and bounded shared meetup,
pickup, Help, and SOS coordination without changing WadaMesh's navigation or
branding. The newest coordination slice is host/build verified but not yet
flashed. Remaining feature surfaces, production persistence/security, and the
custom production FriendMesh event transport remain disabled or incomplete.

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
