# T-Display P4 C6 firmware and Bluetooth plan

## Decision

Use Espressif esp-hosted as the normal WadaMesh backend for current T-Display P4
V1 units, which ship C6 hosted firmware 2.12.3. Retain the existing ESP-AT
backend only for older units carrying LilyGo's now-deprecated AT images.

Older AT units need a guided two-stage conversion because the P4 can reset the
C6 but cannot press its documented physical BOOT control. Current hosted units
need no C6 reflash. After any legacy conversion, C6 firmware updates should be
delivered and verified by the P4 over SDIO so normal WadaMesh updates remain a
one-device operation.

## Goals

- Expose the C6 Bluetooth LE radio to WadaMesh.
- Run the existing MeshCore Bluetooth companion transport without an AT-to-BLE
  protocol translation layer.
- Preserve P4 Wi-Fi scanning, station mode, TCP clients, the companion TCP
  server, HTTP, SNTP, and simultaneous Wi-Fi/BLE operation.
- Make the one-time C6 conversion understandable from the WadaMesh installer.
- Make every later P4 installation or update automatically enforce a compatible
  C6 firmware version.
- Provide A/B update, verification, rollback, and a wired recovery route for the
  C6.
- Keep the P4 usable for LoRa and local UI if the C6 is absent or broken.

## Non-goals

- Classic Bluetooth. The ESP32-C6 supports Bluetooth LE only.
- Silently rewriting the factory C6 through undocumented raw flash commands.
- Depending on deprecated factory ESP-AT images for long-term update support.
- Removing the existing AT implementation before the hosted replacement has
  equivalent Wi-Fi and socket coverage.

## Current architecture

### Processors

- ESP32-P4: runs WadaMesh, LVGL, LoRa, GPS, storage, and the companion logic.
- ESP32-C6: owns the physical 2.4 GHz Wi-Fi and Bluetooth LE radio.

The P4 itself has no Wi-Fi or Bluetooth radio.

### Current C6 connection

The C6 is connected to the P4 over SDIO slot 1:

| Signal | P4 GPIO |
|---|---:|
| CLK | 18 |
| CMD | 19 |
| D0 | 14 |
| D1 | 15 |
| D2 | 16 |
| D3 | 17 |

C6 enable/reset is `XL9535 IO14`. The P4 can pulse this line but does not have a
connection to the C6 IO9 boot strap.

### Current software generations

- Current V1 factory/release firmware uses esp-hosted MCU 2.12.3. WadaMesh uses
  the matching local host fork (`2.12.3~1`) and standard remote Wi-Fi/NimBLE
  paths.
- Older V1 units may carry LilyGo's deprecated ESP-AT SDIO firmware.
- `tdisplay_p4/components/c6_at/` and `C6WifiShim.h` remain as an explicit
  `WADA_P4_LEGACY_AT=1` compatibility backend for those units.
- Legacy AT Wi-Fi scans, joins, sockets, server mode, and SNTP are implemented
  in that worker. Its BLE commands cannot reliably expose the MeshCore service.
- `AT+CIUPDATE` and `AT+USEROTA` are unavailable on the tested legacy image.
- `C6_FLASH_HELPER=1 ./tdisplay_p4/build.sh build` creates a P4 image that does
  not reset or start the C6. It is useful for development while the C6 is placed
  in ROM download mode and flashed through its own USB connection.

### P4 flash layout

The current 16 MB P4 layout contains:

- 4 MB `ota_0`
- 4 MB `ota_1`
- approximately 8 MB FAT `storage`
- NVS, OTA metadata, PHY data, and coredump partitions

The P4 application currently has less than 1 MB free in each app slot. A full C6
image should therefore not be linked into the P4 application. If offline C6
updates are required, reserve a dedicated data partition by reducing the FAT
partition after measuring the final C6 image size.

## Target architecture

### C6 firmware: `wadamesh-c6`

Create a separately versioned ESP-IDF project for the ESP32-C6. Start from the
same esp-hosted slave generation already proven by the Tanmatsu target.

Required C6 features:

- esp-hosted SDIO slave transport
- Wi-Fi station and scan support
- Bluetooth LE HCI transport for NimBLE on the P4
- a small management endpoint for firmware identity and updates
- A/B application partitions with rollback
- signed-image verification
- watchdog and boot-health reporting
- a stable protocol version independent of the human firmware version

The C6 firmware should not implement the MeshCore GATT service itself. It should
transport Bluetooth HCI to the P4, allowing the existing P4-side NimBLE and
MeshCore `SerialBLEInterface` code to own advertising, pairing, GATT, and packet
routing. This keeps one MeshCore BLE implementation across boards.

### Management protocol

Add a narrow WadaMesh management channel alongside the hosted data path. It must
support at least:

- `GET_INFO`: protocol version, firmware version, build hash, active OTA slot,
  rollback state, and capabilities
- `BEGIN_UPDATE`: image size, version, SHA-256, and signature metadata
- `WRITE_CHUNK`: offset, bytes, and per-chunk integrity check
- `END_UPDATE`: full hash/signature verification and boot-slot selection
- `REBOOT`: restart into the pending slot
- `CONFIRM_BOOT`: mark the new C6 image valid after host-side health checks
- `ABORT_UPDATE`: discard a partial update

Commands need sequence numbers and idempotent replies so a P4 reset or SDIO
retry cannot corrupt the inactive slot.

### P4 host changes

Introduce one explicit C6 backend boundary instead of allowing both transports
to initialize:

- `C6_BACKEND_AT`: existing factory ESP-AT support, retained for migration and
  recovery builds
- `C6_BACKEND_HOSTED`: production backend after conversion

For the hosted backend:

- stop compiling or hard-stubbing the duplicate hosted implementations
- use exactly one esp-hosted component, following the Tanmatsu collision fix
- route Wi-Fi through `esp_wifi_remote` and the standard Arduino Wi-Fi facade
- route Bluetooth HCI into NimBLE
- enable the existing MeshCore BLE companion interface
- expose real Bluetooth capability to the UI
- keep C6 reset under the XL9535 callback rather than a fake host GPIO
- leave LoRa/UI startup independent from hosted initialization success

The host must query `GET_INFO` before enabling Wi-Fi or Bluetooth. An unsupported
protocol should produce a clear Settings/About diagnostic and keep the rest of
WadaMesh operational.

## First-time conversion

### Why it cannot currently be one P4 flash

The factory C6 has no working self-update command, and the P4 cannot force C6
IO9 low to enter the ROM downloader. A P4 image cannot safely replace the first
C6 firmware through the current wiring.

`AT+SYSFLASH` or other undocumented raw-partition writes must not be used for the
public migration unless a sacrificial-device test proves all of the following:

- the required C6 partitions can be written while ESP-AT is running
- bootloader, partition table, OTA metadata, and app offsets are known
- power loss cannot leave the C6 unrecoverable without opening the device
- image verification and rollback are available

Until then, use the C6 USB downloader.

### Guided web installer

Extend the WadaMesh install page with a P4-specific two-stage flow:

1. **Install C6 radio firmware**
   - Explain which USB connector reaches the C6.
   - Explain the required BOOT/IO9 and reset sequence with board photographs.
   - Launch an ESP Web Tools manifest with `chipFamily: ESP32-C6`.
   - Flash a full merged `wadamesh-c6` image containing bootloader, partition
     table, OTA metadata, and app.
   - Verify the chip and image before showing success.
2. **Install WadaMesh P4 firmware**
   - Move the cable to the P4 USB connector.
   - Launch the existing `ESP32-P4` WadaMesh manifest.
   - On first boot, require the expected C6 protocol handshake and show the
     detected C6 version in About/Diagnostics.

The page should remember only browser-local completion state. The device
handshake remains authoritative.

### Development flow

Use the existing helper for early testing:

```bash
C6_FLASH_HELPER=1 ./tdisplay_p4/build.sh build
./tdisplay_p4/build.sh flash -p <P4-port>
```

Then place the C6 in ROM download mode and flash `wadamesh-c6` through the C6
USB port. After the C6 image is installed, flash a normal hosted-backend P4
build.

Document a full factory-image backup and restore command before the first C6
write.

## Future seamless C6 updates

Once `wadamesh-c6` is installed, subsequent updates should not require the C6
USB connector.

### Image delivery

Preferred production path:

- publish immutable, versioned C6 OTA images beside each P4 release
- include required C6 protocol/version metadata in the P4 release manifest
- let the P4 download the C6 image over its working C6 Wi-Fi connection
- stream directly into the C6 inactive OTA slot over the management channel

Offline/full-install option:

- add a dedicated P4 `c6_fw` data partition
- place the matching C6 OTA image there in the full merged P4 artifact
- reduce the P4 FAT storage partition by the measured partition size
- do not embed the image in either 4 MB P4 application slot

The online path should remain available for P4 OTA updates where the full flash
image and data partition are not rewritten.

### Update state machine

1. P4 reads C6 identity and compatibility.
2. If compatible, continue boot without rewriting anything.
3. If an update is required, keep Wi-Fi/BLE user transports disabled.
4. Select the C6 inactive OTA slot.
5. Stream chunks with retries and a visible progress state.
6. Verify SHA-256 and signature on the C6.
7. Mark the inactive slot pending and reboot only the C6.
8. Re-establish SDIO and run health checks:
   - management handshake
   - Wi-Fi scan
   - Wi-Fi join where credentials exist
   - BLE controller initialization
   - BLE advertising start
9. Confirm the C6 slot only after health checks pass.
10. If the handshake does not recover, reset the C6 and allow its bootloader
    rollback. Keep the P4 UI and LoRa available with a recovery message.

Never update both P4 and C6 active slots without an independently recoverable
checkpoint between them.

## Release pipeline

Add a dedicated C6 build/release surface rather than hiding it inside the P4
build script:

```text
tdisplay_p4/c6_firmware/
  CMakeLists.txt
  sdkconfig.defaults
  partitions.csv
  main/
  build.sh
```

Release artifacts:

- `wadamesh-c6-full.bin`: first-time USB conversion
- `wadamesh-c6-ota.bin`: later in-band update
- `manifest-tdisplay-p4-c6.json`: ESP Web Tools C6 manifest
- `c6-version.json`: protocol, firmware version, hash, signature, minimum P4
  version, and release channel

Update these existing surfaces:

- `scripts/build-all-targets.sh`: build the C6 project in IDF sweeps
- `scripts/release.sh`: stage C6 artifacts with the P4 release
- `scripts/build/gen-flasher-meta.py`: create the ESP32-C6 manifest
- `deploy/site/index.html`: guided two-stage P4 install
- P4 About/Diagnostics: show C6 backend, protocol, firmware, active slot, and
  last update result

A release gate should reject a P4 build whose required C6 protocol is not
present in the staged C6 metadata.

## Security and integrity

- Sign C6 images with a WadaMesh release key.
- Verify the signature on the C6 before selecting the pending slot.
- Pin the expected SHA-256 in release metadata and report it in diagnostics.
- Reject downgrade unless a physical recovery flow explicitly requests it.
- Do not carry Wi-Fi credentials inside C6 update images.
- Clear partial-update state after abort or rollback.
- Rate-limit and authenticate update commands so a network client cannot invoke
  the management endpoint.

Secure Boot and flash encryption should be evaluated for both processors as a
separate rollout because enabling eFuses changes recovery procedures.

## Compatibility and fallback

During migration, a P4 build should detect three states:

| State | Behavior |
|---|---|
| Factory ESP-AT | Keep AT Wi-Fi available, report Bluetooth unavailable, offer C6 conversion instructions |
| Supported hosted protocol | Enable Wi-Fi and Bluetooth normally |
| Unknown/unresponsive C6 | Keep LoRa/UI running, disable wireless transports, expose diagnostics and wired recovery |

Do not infer the backend from one failed packet. Use a bounded handshake, reset
once through XL9535 IO14, then classify the C6.

## Test plan

### C6 unit/integration

- version/capability handshake
- chunk replay, duplication, and out-of-order rejection
- full hash and signature failure
- power loss at each update stage
- pending-slot rollback
- management request timeout and retry

### Wi-Fi

- scan in sparse and dense RF environments
- hidden SSIDs
- open, WPA2, WPA3, and wrong-password behavior
- reconnect after C6 reset
- TCP client and inbound companion server
- HTTP downloads, tiles, OTA, and SNTP

### Bluetooth LE

- advertising name and MeshCore service UUID
- Android and iOS discovery
- connect/disconnect/reconnect
- bidirectional MeshCore packet transport
- MTU negotiation and fragmented writes
- simultaneous Wi-Fi traffic
- BLE availability after P4 and C6 independent resets

### System

- LoRa receive/transmit while Wi-Fi and BLE are active
- SD card and C6 sharing separate SDMMC hosts
- display underrun/flicker during C6 update and heavy traffic
- screen-off and wake behavior
- low-memory operation
- C6 absent, factory, supported, outdated, and corrupt states
- P4 OTA with matching and mismatched C6 versions
- first-time web installation on macOS, Windows, Linux, Android Chrome, and
  ChromeOS where supported

## Rollout phases

### Phase 0: preserve and measure

- Back up the complete factory C6 flash.
- Record chip revision, flash size, partition table, MAC addresses, and USB IDs.
- Confirm the C6 USB/BOOT procedure on at least two boards.
- Capture current AT Wi-Fi performance as a regression baseline.

### Phase 1: hosted proof of life

- Build and manually flash the smallest esp-hosted C6 slave.
- Establish the SDIO handshake from P4.
- Validate Wi-Fi scan/join and Bluetooth controller discovery.
- Keep the existing AT build selectable for recovery.

### Phase 2: transport parity

- Port every P4 Wi-Fi/socket use away from `c6_at`.
- Enable the existing MeshCore BLE companion transport.
- Pass Wi-Fi, BLE, TCP server, OTA, tile, and time-sync tests.

### Phase 3: public first-time installer

- Publish signed full C6 artifacts and an ESP32-C6 web manifest.
- Add the two-stage installer UI and connector/BOOT instructions.
- Add backend/version diagnostics to P4.

### Phase 4: in-band updates

- Implement C6 A/B management protocol and host updater.
- Add online C6 artifact delivery.
- Add an optional offline P4 `c6_fw` partition after measuring image size.
- Exercise interruption and rollback tests before enabling automatic updates.

### Phase 5: default hosted backend

- Make hosted the normal P4 release backend.
- Retain an explicit AT recovery build for at least one stable release cycle.
- Remove temporary ESP-AT GATT provisioning only after migration coverage is
  acceptable.

## Acceptance criteria

The migration is complete when:

- a fresh user can follow one WadaMesh page to flash both chips without command
  line tools
- normal later WadaMesh updates need only the P4 connection or on-device OTA
- About reports compatible P4 and C6 versions
- Wi-Fi feature parity is maintained
- the MeshCore phone app can discover, connect, exchange packets, and reconnect
  over Bluetooth LE
- failed or interrupted C6 updates roll back without losing P4 LoRa/UI access
- a documented wired restore path returns the C6 to a known image
