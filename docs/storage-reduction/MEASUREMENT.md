# Measurement and verification

## Baseline

Captured on 2026-07-25 from the live dirty worktree. The values include all
current local source edits and must not be treated as a clean-branch release
baseline.

| Item | Value |
|---|---:|
| Environment | `LilyGo_TDeck_companion_radio_touch` |
| Board flash | 16 MiB |
| App slot size | 4,063,232 bytes (`0x3E0000`) |
| `firmware.bin` | 3,288,656 bytes |
| App-slot headroom | 774,576 bytes |
| App-slot use | 80.94% |
| Size-tool `text` | 1,828,435 bytes |
| Size-tool `data` | 1,476,108 bytes |
| Size-tool `bss` | 1,904,026 bytes |

The generic `size` totals include sections mapped for external RAM/flash and
are not a substitute for the PlatformIO RAM/flash percentage summary or
on-device free-heap readings.

## Commands

Use the project-local established environment and the explicit PlatformIO path:

```sh
./scripts/test-friendmesh-core.sh
/Users/tylersmith/.platformio/penv/bin/pio run -e LilyGo_TDeck_companion_radio_touch
/Users/tylersmith/.platformio/penv/bin/pio run -e LilyGo_TDeck_companion_radio_touch -t size
stat -f '%z' .pio/build/LilyGo_TDeck_companion_radio_touch/firmware.bin
git diff --check
```

For every candidate, record a baseline and candidate build from the same commit,
toolchain, partition table, and build mode. Clean both builds when a feature gate
changes dependencies or source filters; incremental artifacts can otherwise
produce misleading comparisons.

## Per-change record

Copy this table into `DECISIONS.md` for every implemented gate:

| Field | Baseline | Candidate | Delta |
|---|---:|---:|---:|
| `firmware.bin` bytes | | | |
| PlatformIO flash bytes/percent | | | |
| PlatformIO static RAM bytes/percent | | | |
| Boot free internal heap | | | |
| Boot largest internal block | | | |
| Boot free PSRAM | | | |

Also record:

- commit and whether the worktree was dirty;
- exact build flags;
- features exercised on hardware;
- persistence paths written during the test;
- whether a card was inserted;
- whether the result is host-only or physically verified.

## Verification ladder

### Host gate for every change

1. Core tests pass.
2. T-Deck environment builds from a clean build directory.
3. Binary and size deltas are recorded.
4. No references to the disabled feature escape its compile-time gate.
5. `git diff --check` passes.

### Hardware smoke gate for presentation-only removals

1. Cold boot reaches the WadaMesh home screen.
2. Touch, keyboard, and trackball work.
3. Existing contacts/channels load.
4. Direct and channel messages send and receive.
5. Reboot retains settings and messages.

### Additional gate for storage-related removals

1. First boot with the expected SD layout succeeds.
2. Reboot with the card retains FriendMesh state.
3. Boot without the card remains recovery-readable and write-disabled.
4. Reinserting the card restores the intended writable backend.
5. A partial SPIFFS-to-SD copy is rejected; adoption still requires
   `identity_ok && failed == 0`.

### Additional gate for network/transport removals

Test every retained companion route independently: USB, BLE, TCP, and WebSocket.
Verify that a removed route does not leave a misleading UI toggle or background
service. If OTA changes, prove both the normal update path and wired recovery on
physical hardware before shipping.

## Partition changes are a separate project gate

Do not combine feature stripping with a partition-table migration in the same
measurement. First shrink the binary and verify behavior. Then design the new
layout with an explicit backup, flash, rollback, and existing-data migration
procedure. A successful host build cannot prove that an installed device will
retain data across a partition change.
