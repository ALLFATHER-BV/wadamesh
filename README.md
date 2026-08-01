<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/wadamesh-readme-dark.svg">
    <img alt="WADAMESH" src="assets/wadamesh-readme-light.svg" width="440">
  </picture>
</p>

<p align="center"><b>A real touchscreen UI for your mesh radio.</b> &middot; open source &middot; GPL-3.0</p>

Touch-UI [MeshCore](https://github.com/meshcore-dev/MeshCore) companion-radio
firmware for the **LilyGo T-Deck / T-Deck Plus** and **Heltec V4 + TFT**
(ESP32-S3).

An LVGL touch UI — map, chat, contacts, channels, settings — split out of
[meshcomod](https://github.com/ALLFATHER-BV/meshcomod). The app depends on a
MeshCore fork via PlatformIO `lib_deps`.

## Boards

See **[DEVICES.md](DEVICES.md)** for the full support matrix, install paths and
per-board status.

- LilyGo T-Deck / T-Deck Plus — env `LilyGo_TDeck_companion_radio_touch` (stable)
- Heltec V4 + TFT + CHSC6x touch — env `heltec_v4_tft_companion_radio_usb_tcp_touch` (stable)
- Tanmatsu (ESP32-P4) — built from `tanmatsu/` (ESP-IDF), ships via the Tanmatsu app store
- Elecrow ThinkNode M9 — env `ThinkNode_M9_companion_radio_touch` (beta)
- RAK WisMesh Tap V2 (RAK3312) — env `rak_tap_v2_companion_radio_touch` (beta)

## Architecture

This repo holds only the **app**: the `companion_radio` glue, the `ui-touch`
LVGL UI, the two boards' glue/variants, and `platformio.ini`. The **MeshCore
core is not vendored here** — it's pulled as a library via `lib_deps` from the
[`ALLFATHER-BV/meshcomod`](https://github.com/ALLFATHER-BV/meshcomod) monorepo
(the same repo as the non-touch firmware), pinned by a lean source-only `core-*`
git tag. The touch-app files this repo owns (TouchPrefsStore, WifiRuntimeStore,
the transports, …) are dropped from the lib via `-DMC_VENDORED_TOUCH_APP` so they
aren't compiled twice. The build is byte-identical to the original in-tree
meshcomod firmware.

## FriendMeshOS features

FriendMeshOS is the name of an optional friends, trust, private-group, navigation,
recovery, and peer-assistance feature layer being developed inside WadaMesh. It
does not rename the repository, replace WadaMesh's product identity, or redesign
its UI. Current implementation and verification are limited to the LilyGo T-Deck;
other hardware is deferred until the scope is explicitly expanded.

- **[FRIENDMESHOS_FEATURES.md](FRIENDMESHOS_FEATURES.md)** surveys the current
  WadaMesh architecture, inherited capabilities, boundaries, and integration
  points.
- **[FRIENDMESHOS_ROADMAP.md](FRIENDMESHOS_ROADMAP.md)** explains what the project
  is, records the verified baseline, and orders implementation from easiest to
  hardest.
- **[FRIENDMESH_FULL_IMPLEMENTATION_GAME_PLAN.md](FRIENDMESH_FULL_IMPLEMENTATION_GAME_PLAN.md)**
  defines the detailed feature, security, recovery, test, and release contracts.
- **[FRIENDMESH_NEARBY_JOIN_PROTOCOL.md](FRIENDMESH_NEARBY_JOIN_PROTOCOL.md)**
  defines the nearby zero-hop invitation UX and encrypted member-specific grant
  boundary without ever sharing identity private keys.

The current T-Deck build contains a platform-neutral FriendMesh application core
and development-provider implementations of membership, chat/sync,
position/navigation/markers/meetups, and safety/notification behavior. The old
volatile FriendMesh development app and its local-only marker/runtime state have
been removed; FriendMesh behavior is exposed only through the relevant existing
WadaMesh contact, channel, request, and map surfaces.

WadaMesh private channels have an `Invite nearby` action. A bounded
three-second Bluetooth scan discovers updated T-Decks belonging to saved chat
contacts; a fresh direct zero-hop LoRa advert remains the fallback. Bluetooth
advertises only a versioned public-key prefix and never carries the channel key.
The sender passes the channel's existing 16-byte secret through MeshCore's
encrypted direct-message path with an empty route, and the recipient must
explicitly join. The receiver rejects forwarded invite envelopes. Identity
private keys are never sent. Bluetooth RSSI and direct RF are convenience
signals, not proof of physical proximity. Production FriendMesh identity,
transcript, replay, protected-storage, and epoch/rekey security remain later
shared work.

Private-channel actions also expose a bounded `Members` view. It persists local
roster metadata, shows `Invited`, `Joined`, `Invite failed`, `Left`, and
`Removed` states, and synchronizes joined rosters through compact encrypted
channel control records that never enter chat history. Guests can leave and
owners can cooperatively remove updated T-Decks. Any departure or removal is
visibly marked `rekey required`: until the later security phase rotates and
redistributes the shared channel key, removal is not cryptographic revocation.
The administrator can also disband from the existing Members view. The device
attempts the same encrypted, chat-suppressed removal notice to every joined
member before deleting its local channel and reports unreachable recipients.

Joined private channels also expose `Group map`. It filters the existing
WadaMesh map to positioned joined members, and tapping one of those members
offers `Friend Compass` with selectable north-up/headway-up distance, bearing, position age, and stale
location warnings. Opening the map performs one bounded, single-flight pass over
the joined roster using MeshCore's existing encrypted contact-telemetry request;
each reply updates that member's saved map position without requiring a public
location advert. A device authorizes its current position for contacts locally
recorded as joined in a FriendMesh channel. The pass stops after the roster and
does not become a background transmitter. It does not introduce a second map,
a new wire protocol, or new UI branding.

Friend Compass retains only the previous and current fix for each bounded group
member plus the local user in `/friendmesh/motion_<channel-id>.bin` on the microSD card. Writes are
checksummed, coalesced, and flushed when the map closes; there is deliberately no
internal-flash fallback. This development cache is not yet encrypted or
authenticated, so SD location-history hardening remains part of the later
security phase.

FriendMesh T-Deck builds now require a mounted microSD card for long-term data.
The complete MeshCore/FriendMesh store lives under `/meshcomod`, and chat
history, discovery state, motion history, backups, and online map cache files
also stay on the card; battery history pauses and crash exports fail cleanly
without one. An existing internal store is retained only as a readable
recovery source: if SD mounting or complete migration fails, persistent
DataStore writes are disabled and the UI reports that history is not being
saved. Small NVS boot-selection values remain an intentional control-plane
exception. Insert the card before boot and keep it installed during operation.
Migration copies the identity and other critical files explicitly before a
recursive full-path scan, preserves existing non-empty SD files during automatic
boot repair, verifies the copied identity, and never deletes the internal
recovery source.

The compass itself is a two-page WadaMesh fullscreen tool. Page one is the
detailed spatial dial with a `NORTH-UP` / `HEADWAY-UP` toggle, distance-scaled rings, distinct current and
predicted markers, a fixed 45-second lead readout, and compact movement/freshness
status. North-up is absolute; headway-up consistently rotates the pointer,
cardinal labels, trails, and guidance arrow around the user's reliable two-fix
GPS course. Page two turns the same live calculation into
plain-language travel guidance, movement speed, confidence, freshness, and a
working `Use current position` prediction override. Its default maneuver view
uses qualitative instructions (`slight`, standard, `sharp`, or `turn around`)
and a matching straight, curved, right-angle, or U-turn arrow; `SHOW NORTH`
switches to an absolute north-relative bearing arrow. After the local user moves
at least eight meters across two recent fixes, page two also shows GPS course,
left/right turn correction, closing or opening speed, and a bounded ETA while
approaching the selected current or predicted target. Horizontal touch swipes,
trackball left/right, and two tappable bottom dots switch pages. Vertical input
remains available for focus/scroll behavior.

When both positions are valid, opening Friend Compass sends the selected member
one targeted system notice with the starter's authenticated contact identity and
current straight-line distance. It uses MeshCore's existing encrypted contact
message path, is validated against the local joined roster, and is consumed
before DM/chat history. It is attempted once per Compass session rather than on
every one-second refresh.

Only explicitly created or joined FriendMesh private groups expose `Invite
nearby`, `Members`, `Group map`, and `Coordinate`; ordinary MeshCore channels
retain their normal channel actions. The add-channel menu provides separate
`Create a private channel` and `Create a FriendMesh group` choices. FriendMesh
groups are marked `[FM]` in the inbox, open-chat title, and action-sheet title
without changing the underlying MeshCore channel name.

In a FriendMesh private group, `Coordinate` lets joined members share one
current-location meetup or pickup point, respond `going` or
`arrived`, open bounded ride/lost/equipment/contact Help incidents, respond to
them, and close or cancel them. SOS requires a continuous three-second hold.
These records use compact encrypted MeshCore group data, are persisted per
channel, appear on the existing WadaMesh map, and are consumed before the chat
pipeline so protocol bytes cannot become visible messages. This compatibility
path is functional but not production-secure FriendMesh identity: a holder of
the shared channel key can spoof a roster prefix, and removed members retain
access until Phase 7 signing and forward-only rekey are complete.

The separate FriendMesh development workspace is no longer included. WadaMesh's
UI and branding remain authoritative; optional themes are the maximum broad
visual addition in scope.

## Build

[PlatformIO](https://platformio.org/) pulls the core fork and all libraries
automatically:

```bash
pio run -e heltec_v4_tft_companion_radio_usb_tcp_touch   # Heltec V4 TFT
pio run -e LilyGo_TDeck_companion_radio_touch            # LilyGo T-Deck
# `pio run` builds every environment currently listed in `default_envs`
```

FriendMesh development uses the explicit T-Deck command above; do not use the
multi-environment default as its verification gate.

For routine T-Deck development, use the guarded application-only uploader so a
USB interruption cannot overwrite the bootloader or partition table:

```bash
./scripts/upload-tdeck-app-only.sh \
  --port /dev/cu.usbmodem1101 \
  --confirm-app-only
```

The four-component chain (bootloader / partitions / boot_app0 / firmware at
`0x0 / 0x8000 / 0xe000 / 0x10000`) is reserved for deliberate provisioning or
recovery. Do not use a merged image for routine updates; its padding can wipe
NVS and other partition contents.

## Contributing

Contributions are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md). One topic per
PR; inbound contributions are accepted under the project's GPL-3.0 license.

## License

**GPL-3.0-or-later** — see [LICENSE](LICENSE). wadamesh is copyleft: anyone who
distributes a build or a fork must also make their source available under the GPL.
This keeps the UI open and concentrates community effort instead of fragmenting it
into closed forks.

wadamesh incorporates and depends on
[MeshCore](https://github.com/meshcore-dev/MeshCore) (MIT, © Scott Powell /
rippleradios.com) and other third-party components — see [NOTICE](NOTICE) for the
full list and their licenses. MeshCore-derived files keep their MIT notices; the
combined work is distributed under the GPL (MIT is GPL-compatible). The MeshCore
fork that wadamesh builds against stays **MIT** on purpose, so its Wi-Fi/BLE hooks
remain upstreamable to MeshCore.
