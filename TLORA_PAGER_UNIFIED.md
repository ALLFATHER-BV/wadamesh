# T-LoRa Pager unified radio build

`tlora_pager_unified` is an experimental image for the SX1262 and LR1121
T-LoRa Pager variants. It reads each radio's identification twice at boot,
then initializes the detected chip using that variant's existing RF switch,
TCXO voltage, transmit power and MeshCore settings. An unknown or unresponsive
radio takes the normal radio-not-detected boot path; it is not guessed.

Build and prepare the USB-flashable image with PlatformIO:

```sh
pio run -e tlora_pager_unified
pio run -e tlora_pager_unified -t mergebin
```

On Windows, set `PYTHONUTF8=1` in the build shell first: PlatformIO reads the
partition CSV using Python's default encoding, and its comments contain UTF-8.

The app image is `.pio/build/tlora_pager_unified/firmware.bin`; the merged
image is `.pio/build/tlora_pager_unified/firmware-merged.bin`. The existing
partition table and user-data locations are unchanged. The release script
stages this image as `wadamesh-tlora-pager-unified`, including its own OTA
artifact and generated flasher manifest. The existing radio-specific images
and their update paths remain available; they do not migrate automatically.
The unified OTA path works only for releases that include the unified image.

This environment enables link-time optimization (`-flto`) and removes
Arduino's forced `-fno-lto` linker flag. Both radios and the existing features
remain enabled. The app is 3,916,736 bytes, 134,688 bytes smaller than the
first unified build, leaving 146,496 bytes of its 4,063,232-byte OTA slot.
Build options, dependencies and release tags can change that size, so check
the slot size again for every release.

An SX1262 Pager running commit `71808574` passed a full USB erase/install, radio detection and
initialization, mesh/UI startup, a hardware reset and USB device-info queries
on 2026-11-17. Its SD card did not mount; the internal SPIFFS fallback booted.
Status reported a latched RX-start-timeout flag (`0x4`) after startup; the
noise floor later settled to -115 dBm. Packet reception has not been verified.

At 911.15 MHz (62.5 kHz bandwidth, SF8, coding rate 4/5, 22 dBm), two zero-hop
node-discovery probes advanced the radio's TX-completion counter from 2 to 4
and left the outbound queue empty. No packets or matching replies arrived
during 105 seconds of observation, with no receive errors counted. This
verifies local TX completion only; an over-the-air RX/TX pass needs a matching
peer. The Pager was left configured for 911.15 MHz.

A read-only SD diagnostic initialized the inserted card and read
sectors at 4 MHz, 1 MHz and 400 kHz with the radio and display inactive. It
reported 127,139,840 bytes (about 128 MB). Sectors 0, 1, 63 and 2048 returned
identical contents beginning with a repeating `55 aa` pattern; sector 0 ended
in `58 58` instead of the required `55 aa` signature. FatFs returned
`FR_NO_FILESYSTEM` (13). Power cycling and changing the pull-control setting
did not recover a filesystem. This points to card contents or card behavior;
checking the card in a separate reader or testing a known-good card is still
needed to distinguish them. No card writes or formatting were performed. The
unified firmware was restored, its flash hash verified, and mesh/UI startup
passed again using internal storage.

**Full hardware validation, including LR1121, remains pending.** Keep the
existing radio-specific choices as the public defaults until both Pager
variants pass the following checks with the same image:

1. Cold boot and reboot several times. The serial log and Settings > Device
   should identify the installed SX1262 or LR1121. Confirm that an unknown or
   unresponsive radio stops at the radio error screen.
2. Send and receive channel and direct messages with another node, including
   after changing frequency, spreading factor, power and boosted receive gain.
   Repeat with buffered receive enabled and disabled.
3. Open and close the spectrum analyzer, then verify mesh reception and
   transmission still work. Check sleep, power-off and wake/reboot behavior.
4. Check that data and settings survive a reboot and an update. Once a release
   containing the unified artifact is available, verify an on-device OTA to a
   newer unified release on each radio variant.

The probe can also be tested without hardware:

```sh
g++ -std=c++17 -I variants/lilygo_tlora_pager test/test_pager_radio_probe.cpp -o pager_probe_test
./pager_probe_test
g++ -std=c++17 -I test/pager_radio_fakes -I variants/lilygo_tlora_pager test/test_pager_radio_spi_probe.cpp -o pager_spi_test
./pager_spi_test
```
