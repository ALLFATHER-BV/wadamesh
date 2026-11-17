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

The new detection and shared driver have **not yet been verified on physical
devices**. Keep the existing radio-specific choices as the public defaults
until both Pager variants pass the following checks with the same image:

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
