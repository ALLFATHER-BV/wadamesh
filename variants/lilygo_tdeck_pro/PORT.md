# LilyGo T-Deck Pro port

PlatformIO environments:

```text
LilyGo_TDeck_Pro_v1_0_companion_radio_touch
LilyGo_TDeck_Pro_v1_1_companion_radio_touch
```

This is the 3.1-inch, 240x320 GDEQ031T10 e-paper T-Deck Pro. It is not the
T-Deck Max. V1.0 and V1.1 have incompatible reset and frontlight wiring, so
select the target matching the PCB revision. The legacy
`LilyGo_TDeck_Pro_companion_radio_touch` environment remains a V1.1-only local
compatibility alias and is not a release target.

V1.0 has no DRV2605 at I2C address `0x5A`. V1.1 has the DRV2605 at `0x5A` and
no XL9555; the T-Deck Max is the model with an XL9555 at `0x20`. Check the PCB
marking first and use these I2C identities when the marking is unclear.

## Hardware

| Function | Shared / V1.0 | V1.1 |
|---|---|---|
| Shared SPI | SCK 36, MISO 47, MOSI 33 | Same |
| E-paper | CS 34, DC 35, BUSY 37, no reset | Reset 16; other pins the same |
| Frontlight | None | GPIO45 |
| Touch | CST328 or CST3530 at `0x1A`, INT 12, reset 45 | Reset 38; other pins the same |
| 1.8 V enable | GPIO38, driven high before display/touch setup | Not required |
| SX1262 | CS 3, DIO1 5, reset 4, BUSY 6, module power 46, TCXO 2.4 V | Same |
| microSD | CS 48 on shared SPI | Same |
| I2C | SDA 13, SCL 14 | Same |
| Keyboard | TCA8418 at `0x34`, INT 15, backlight 42 | Same |
| GPS | MIA-M10Q, host RX 44, host TX 43, enable 39, 38400 baud | Same |
| Battery | BQ25896 at `0x6B` | Same |
| User button | GPIO0, active low | Same |

The V1.0 build never initializes frontlight PWM or shows a brightness control.
Using the V1.1 map on V1.0 drives GPIO45 as frontlight PWM and can leave the
touch controller permanently asserted in reset.

The e-paper SPI tuple follows the hardware-tested Camillia implementation.
Meshtastic currently publishes a conflicting e-paper MOSI alias while its
shared-SPI definitions still name MOSI 33; do not change this port to that alias
without a logic trace or hardware result.

## Display and input

Wadamesh still renders LVGL in RGB565. `TDeckProDisplay` thresholds each dirty
band into a full 1-bit shadow, then coalesces the completed LVGL frame into an
e-paper update. Every tenth update is full; the intervening updates use the
panel's partial-refresh mode. The UI is fixed to the light palette.

Map tiles receive a map-specific monochrome pass before LVGL displays them. It
uses chroma-aware luminance and an 8x8 ordered dither so roads, water, terrain,
and labels survive the 1-bit conversion without seams between adjacent tiles.

The Spectrum app uses an e-paper snapshot layout: a large two-pixel black trace
on white with monochrome labels and sparse grid lines. It omits the LCD color
waterfall and only invalidates the display after a complete radio sweep, while
the radio sampling itself continues in short responsive chunks.

The panel can remain BUSY for roughly 0.7-1.1 seconds. Its BUSY callback pumps
the TCA8418 driver into a 64-byte software queue so fast typing does not overflow
the controller's ten-event FIFO. The Pro matrix order is covered by
`test/test_tdeck_pro_keyboard_state.cpp`.

Touch probes CST3530 first and falls back to CST328. It is polled inline with
the UI and from the panel BUSY callback during e-paper refreshes, keeping the
shared I2C bus single-owner while still observing releases during a blocked
refresh. While asleep, touch and keyboard events are drained but do not wake the
display; GPIO0 is the wake control.

## Validation status

- PlatformIO configuration resolves and all declared dependencies install.
- Pro and Pager keyboard-state host tests pass.
- Static diagnostics and `git diff --check` pass.
- The V1.1 firmware has been exercised on LilyGo T-Deck Pro hardware. The V1.0
  target follows LilyGo's V1.0 hardware definitions and factory power-enable
  sequence but still needs direct hardware validation.

Substantial additional testing is required before release, including repeated
boot/full-refresh cycles, partial-refresh cadence and ghosting, keyboard input
during BUSY, both touch-controller revisions, radio TX/RX, SD mount and I/O, GPS,
battery reporting, frontlight behavior, GPIO0 sleep/wake, OTA, companion
transports, and broad regression testing across the shared touch UI.