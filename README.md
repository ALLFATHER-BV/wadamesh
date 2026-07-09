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

- LilyGo T-Deck / T-Deck Plus — env `LilyGo_TDeck_companion_radio_touch`
- Heltec V4 + TFT + CHSC6x touch — env `heltec_v4_tft_companion_radio_usb_tcp_touch`
- RAK WisMesh Tap V2 (RAK3312) — env `rak_tap_v2_companion_radio_touch`

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

## Build

[PlatformIO](https://platformio.org/) pulls the core fork and all libraries
automatically:

```bash
pio run -e heltec_v4_tft_companion_radio_usb_tcp_touch   # Heltec V4 TFT
pio run -e LilyGo_TDeck_companion_radio_touch            # LilyGo T-Deck
# or just `pio run` to build both
```

Flash with the NVS-preserving 4-component chain (bootloader / partitions /
boot_app0 / firmware at `0x0 / 0x8000 / 0xe000 / 0x10000`) so saved Wi-Fi
credentials survive — not a merged image, which 0xFF-pads and wipes NVS.

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
