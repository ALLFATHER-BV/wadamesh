# LilyGo T-Deck Pro port

PlatformIO environment:

```text
LilyGo_TDeck_Pro_companion_radio_touch
```

This is the 3.1-inch, 240x320 GDEQ031T10 e-paper T-Deck Pro. It is not the
T-Deck Max. Hardware definitions are based on the working `tdeck-pro` target in
`~/Projects/camillia/camillia-mt` at v4.8.0, with LilyGo and Meshtastic's
`t-deck-pro-v1_1` variant as secondary references.

## Hardware

| Function | Pins / device |
|---|---|
| Shared SPI | SCK 36, MISO 47, MOSI 33 |
| E-paper | CS 34, DC 35, reset 16, BUSY 37, frontlight 45 |
| SX1262 | CS 3, DIO1 5, reset 4, BUSY 6, module power 46, TCXO 2.4 V |
| microSD | CS 48 on shared SPI |
| I2C | SDA 13, SCL 14 |
| Keyboard | TCA8418 at 0x34, INT 15, backlight 42 |
| Touch | CST328 or CST3530 at 0x1A, INT 12, reset 38 |
| GPS | MIA-M10Q, host RX 44, host TX 43, enable 39, 38400 baud |
| Battery | BQ25896 at 0x6B |
| User button | GPIO0, active low |

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
the UI to keep the shared I2C bus single-owner. While asleep, touch and keyboard
events are drained but do not wake the display; GPIO0 is the wake control.

## Validation status

- PlatformIO configuration resolves and all declared dependencies install.
- Pro and Pager keyboard-state host tests pass.
- Static diagnostics and `git diff --check` pass.
- The firmware has been exercised on LilyGo T-Deck Pro hardware. This is initial
  validation only, not comprehensive release qualification.

Substantial additional testing is required before release, including repeated
boot/full-refresh cycles, partial-refresh cadence and ghosting, keyboard input
during BUSY, both touch-controller revisions, radio TX/RX, SD mount and I/O, GPS,
battery reporting, frontlight behavior, GPIO0 sleep/wake, OTA, companion
transports, and broad regression testing across the shared touch UI.