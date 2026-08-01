# WadaMesh storage-reduction track

This directory is the source of truth for reducing the LilyGo T-Deck firmware
footprint. It is intentionally documentation-only for the first pass. No
feature is approved for removal merely because it appears in these files.

## Objective

Produce a smaller T-Deck build without accidentally removing the MeshCore
companion-radio core, corrupting persisted data, weakening recovery, or making
shared code unusable by a future Heltec build.

The initial target is the PlatformIO environment:

```text
LilyGo_TDeck_companion_radio_touch
```

T-Deck decisions should use capability flags or profile-level feature flags.
They should not add scattered board-name checks to the shared UI. Heltec does
not need to receive the same stripped profile, but shared interfaces must keep
working for it.

## What “storage” can mean

These are separate budgets and must not be mixed in status reports:

| Budget | Current evidence | How a change helps |
|---|---:|---|
| App flash | `firmware.bin` = 3,288,656 bytes on 2026-07-25 | Compile/link less code and fewer embedded assets |
| OTA slot | 4,063,232 bytes per slot (`0x3E0000`) | A smaller image creates upgrade headroom; changing the partition table changes capacity |
| Internal data flash | 4.75 MiB `tiles`, 3.375 MiB `spiffs`, 64 KiB coredump | Remove/repartition only after deciding which data features survive |
| Static/internal RAM | Size report needs section-aware interpretation because PSRAM sections are present | Remove permanent buffers/objects or move eligible data to PSRAM |
| SD usage | FriendMesh long-term data is rooted at `/meshcomod` | Retention limits and cache policies reduce card growth; they do not shrink the firmware |

The app image currently occupies about **80.94%** of either OTA slot, leaving
774,576 bytes of slot headroom. This percentage is calculated from the actual
binary and partition sizes above, not the stale “~2.55 MB” comment currently in
the partition CSV.

## Guardrails

Keep these unless a later decision explicitly changes the product scope:

- LoRa/SX1262 radio operation and the MeshCore companion-radio core;
- display, GT911 touch, keyboard, trackball, and basic settings;
- direct and channel messaging, contacts, and channel persistence;
- the current WadaMesh name, visual identity, and ordinary MeshCore workflows;
- SD-bound FriendMesh long-term persistence at `/meshcomod`;
- read-only recovery when the SD card is absent;
- fail-closed SPIFFS-to-SD migration: adoption requires `identity_ok && failed == 0`;
- a recoverable firmware-install path, even if the exact OTA feature is later changed.

## Files in this track

- [FEATURE_MATRIX.md](FEATURE_MATRIX.md) ranks removable feature families and
  records their dependencies and user-visible cost.
- [MEASUREMENT.md](MEASUREMENT.md) defines the repeatable build/measurement and
  verification procedure.
- [DECISIONS.md](DECISIONS.md) is the approval ledger. Code removal starts only
  after the intended profile is recorded there.

## Proposed sequence

1. Choose the product profile in `DECISIONS.md`.
2. Introduce centralized `WADAMESH_FEATURE_*` gates with safe defaults.
3. Remove one feature family per build so its real delta can be measured.
4. Remove now-unused libraries or source filters only after the linker proves
   that no retained feature needs them.
5. Consider partition changes last. Partition edits affect recovery, OTA, and
   existing on-device data even when the application code is correct.

Documentation status: baseline captured; removal scope awaits approval.
