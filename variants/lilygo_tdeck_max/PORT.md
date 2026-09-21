# LilyGo T-Deck Max port

PlatformIO environment:

```text
LilyGo_TDeck_Max_companion_radio_touch
```

This is the T-Deck Max: the same 3.1-inch 240x320 GDEQ031T10 e-paper, ESP32-S3, SX1262, TCA8418 keyboard and CST328/CST3530 touch as the T-Deck Pro, with the peripheral power enables, resets and switches moved onto an XL9555 I/O expander and the battery reported by a BQ27220 fuel gauge. Hardware definitions, the XL9555 line map, the boot state and the reset timings follow the Meck firmware's working bring-up of this board. The Pro's display and touch drivers (`variants/lilygo_tdeck_pro/TDeckProDisplay.cpp`, `TDeckProTouch.cpp`) are compiled unchanged; only the board, GPS provider and `target` files are Max-specific.

## Hardware

| Function | Pins / device |
|---|---|
| Shared SPI | SCK 36, MISO 47, MOSI 33 (as Pro) |
| E-paper | CS 34, DC 35, reset 9, BUSY 37, frontlight 41 |
| SX1262 | CS 3, DIO1 5, reset 4, BUSY 6, TCXO 2.4 V; supply via XL9555 line 1; antenna switch XL9555 line 4 (HIGH internal) |
| microSD | CS 48 on shared SPI |
| I2C | SDA 13, SCL 14 |
| XL9555 expander | 0x20; lines 0 modem supply, 1 LoRa supply, 2 GPS supply, 3 1.8 V, 4 antenna select, 5 motor, 6 speaker amp, 7 touch reset, 8 modem PWRKEY, 9 keyboard reset, 10 audio mux |
| Keyboard | TCA8418 at 0x34, INT 15, backlight 42; reset via XL9555 line 9 |
| Touch | CST328 or CST3530 at 0x1A, INT 12; reset via XL9555 line 7 |
| GPS | host RX 2, host TX 16, 38400 baud; supply via XL9555 line 2 |
| Battery | BQ27220 fuel gauge at 0x55 (charger is an SY6970 at 0x6A, not read) |
| User button | GPIO0, active low |

Differences from the T-Deck Pro that the env encodes: e-paper reset 16 -> 9, frontlight 45 -> 41, GPS UART 44/43 -> 2/16, and no `PIN_PERF_POWERON`, `PIN_GPS_EN` or GPIO touch reset (all three are expander lines here). GPIO 38, the Pro's touch reset, is the ES8311 codec's MCLK on the Max and must not be driven.

## Boot sequence

`TDeckMaxBoard::begin()` brings the I2C bus up, probes the XL9555, writes both output ports to the boot state before switching the lines to outputs (LoRa on, 1.8 V on, internal antenna, modem/motor/amp off, resets released, audio mux to the ES8311), waits 10 ms for the LoRa rail, then pulses the touch reset and the keyboard reset (LOW 20 ms, HIGH, 50 ms settle each) and probes the fuel gauge. `display.begin()` runs after this and finds an already-reset touch controller; with `PIN_TOUCH_RST=-1` the Pro driver's own GPIO reset pulse is a no-op and `CSE_CST328` skips its GPIO pulse.

The touch controller is identified at boot: `[BOOT] T-Deck Pro e-paper ready touch=CST328` (or `CST3530`, or `missing`).

## GPS power

The GPS supply follows the Pro's behaviour: off at boot, on when GPS is enabled, off when it is disabled. On the Pro the provider drives GPIO 39 for this; on the Max `TDeckMaxGps` (a subclass of the wadamesh provider) switches XL9555 line 2 around the provider's `begin()`/`stop()` and reports `isEnabled()` from the rail state.

## Not in this port

ES8311 audio, the NS4150B speaker amplifier, the A7682E modem, the DRV2605 haptic and the BHI260AP IMU are powered or muxed by the expander but not driven by this build. The Pro's `PIN_PERF_POWERON` radio power-cycle rescue in `main.cpp` compiles out on the Max; a Max equivalent through XL9555 line 1 is a follow-up.

## Validation status

Exercised on a T-Deck Max (CST3530 touch revision) on 2026-09-21: boot, XL9555 and BQ27220 probes, e-paper full and partial refresh, touch (interrupt-driven), the three front pads, TCA8418 keyboard including the both-Shifts chord, radio TX/RX (channel messages sent and received), contacts and channels on the SD card, battery reporting, front-light, idle sleep and wake. Not yet exercised: GPS fix, OTA, deep sleep.
