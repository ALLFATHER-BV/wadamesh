# Seeed SenseCAP Indicator D1L port

PlatformIO environment:

```text
sensecap_indicator_companion_radio_touch
```

The SenseCAP Indicator D1L is an ESP32-S3 (8 MB flash, 8 MB octal PSRAM) with a 4-inch 480x480 RGB panel behind an ST7701S, an FT6336 capacitive touch controller and an SX1262. A TCA9535 I/O expander carries the radio chip select, reset, BUSY and DIO1 lines as well as the display chip select and reset and the touch reset and interrupt. A separate RP2040 co-processor owns the buzzer, microSD, Grove connector and (on the D1Pro) the sensors; this build does not use it. The D1Pro shares the ESP32 side of the board and is expected to run this image, but it is untested; the D1L is the reference target.

The board is USB-powered and has no battery. `CAP_BATTERY` is 0, so the status bar has no battery glyph and the Battery settings page (history, calibrate, battery saver) is hidden. Power off is hidden.

## Hardware

| Function | Pins / device |
|---|---|
| I2C | SDA 39, SCL 40, 400 kHz |
| TCA9535 expander | 0x20; /INT on GPIO 42 (active low, falling edge) |
| Display | ST7701S, 16-bit RGB, 480x480; data D0..D15 = GPIO 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0; DE 18, VSYNC 17, HSYNC 16, PCLK 21; init commands over a bit-banged 3-wire SPI on SCLK 41, MOSI 48 (shared with the radio SPI) |
| Backlight | GPIO 45, LEDC channel 7, 12 kHz |
| Touch | FT6336 at 0x48 |
| SX1262 | SPI FSPI at 4 MHz: SCLK 41, MISO 47, MOSI 48; NSS, RESET, BUSY and DIO1 on the expander; DIO2 drives the RF switch; DIO3 supplies the TCXO (2.4 V) when the strap selects it; current limit 140 mA, boosted RX gain |
| User button | GPIO 38 |
| RP2040 | held out of reset by expander P1.0; not otherwise used |

Expander map (TCA9535, port 0 then port 1):

| Pin | Function |
|---|---|
| P0.0 | LoRa NSS (out) |
| P0.1 | LoRa RESET (out) |
| P0.2 | LoRa BUSY (in) |
| P0.3 | LoRa DIO1 (in) |
| P0.4 | LCD CS (out) |
| P0.5 | LCD RESET (out) |
| P0.6 | touch INT (in) |
| P0.7 | touch RESET (out) |
| P1.0 | RP2040 RESET (driven high) |
| P1.3 | radio strap (in; high = TCXO fitted) |

Every other port-1 bit keeps the state it had at boot. The output and configuration shadows are seeded from the chip, never from fixed defaults, so state left by a warm reset survives. A failed I2C write rolls both shadows back.

The radio pins are virtual numbers that the HAL maps to the expander: `P_LORA_NSS=0x40`, `P_LORA_RESET=0x41`, `P_LORA_BUSY=0x42`, `P_LORA_DIO_1=0x43`.

## Boot sequence

`IndicatorBoard::begin()` runs `ESP32Board::begin()`, then `IndicatorIo::begin()` under the shared I2C bus lock: 400 kHz clock, expander probe and shadow seed, polarity inversion cleared if a previous firmware set it, the LoRa NSS, LCD CS, LCD RESET, touch RESET and RP2040 RESET outputs driven high, the four inputs configured, and a five-sample read of the radio strap (a majority decides the TCXO selection, so a single glitch cannot flip it). `[indicator] expander seeded out=.. cfg=..` logs the registers as the previous firmware or a warm reset left them, `[indicator] expander out=..` the result. A failure names its step: seed read, output config, input config or strap read.

`display.begin()` follows: radio NSS high, LCD CS high, LCD RESET low for 10 ms and high again with a 120 ms settle, then with the bus lock held CS low, the LovyanGFX panel init, CS high. Holding the lock keeps any other task from moving the radio NSS while the init bit-bangs the shared GPIO 41/48. The panel init restores those two pins afterwards, and nothing writes panel commands after init. The backlight starts at about 63 % over a black frame. If the expander is not ready, `_lcd.init()` fails (`[indicator] display: lcd.init failed`) or an expander write during the sequence fails, `display.begin()` leaves the backlight off and returns false. The backlight then stays off: the UI still sends brightness and wake requests to the display, and the driver ignores them until the panel has come up.

`radio_init()` runs after the display, so the radio's FSPI claims GPIO 41/47/48 once LovyanGFX has released them.

## Radio

The SX1262 sits behind the expander, so RadioLib runs on `IndicatorRadioHal`, a subclass of `ArduinoHal`:

- Pins with the expander flag (`0x40` and up) go through the TCA9535; every other pin goes straight to the Arduino pin and interrupt functions, as in `ArduinoHal`. `RADIOLIB_NC` is not an expander pin.
- NSS toggles around every SPI transfer, each edge one expander write. RESET adds a 20 ms settle after its rising edge.
- Every expander access holds the shared I2C bus lock, which is a recursive mutex with priority inheritance.

Interrupt dispatch: the expander's /INT output is wired to GPIO 42. Its ISR (in IRAM) only notifies the `ind_dio1` task (3072-byte stack, high priority, core 1). That task reads port 0 under the bus lock and never touches RadioLib or SPI. `Dio1Edge` watches bit 3 of every port-0 read and fires the RadioLib receive callback exactly once per 0-to-1 transition. This holds whichever reader sees the edge: the dispatch task, or a BUSY poll inside an SPI transfer (that read clears /INT, so the edge is still delivered from there). A DIO1 level that stays high while another input changes (touch INT, BUSY) does not fire again. The task also wakes every 50 ms as a net for a /INT that was already low when the ISR attached.

/INT wake log: several port-1 inputs float and the expander has no pull-ups, so /INT can toggle continuously, and every toggle costs an I2C read at high priority. The dispatch task counts ISR notifications and 50 ms timeouts and logs `[indicator] /INT wakes=<n>/10s timeouts=<n>` every 10 s for the first minute, then only while the ISR rate is above 500/s, at most once a minute.

`MESH_RADIO_DIO1_POLLED` is off. Every input read reaches `Dio1Edge`, and the dispatch task reads at least every 50 ms, so a rise whose /INT was missed still fires the callback, at most 50 ms late. Polling the SX1262 IRQ register as well would add an SPI transfer, and with it expander writes for NSS, on every pass of the receive path.

TCXO strap: P1.3 high means a TCXO is fitted. `indicatorTcxoVoltage()` returns 2.4 V in that case and 0 V (crystal) otherwise; `SX126X_DIO3_TCXO_VOLTAGE` calls it at radio init. The flag is quoted in `platformio.ini` so that SCons' shell does not choke on the parentheses. When the radio does not start with the strap's setting, `radio_init()` tries the other one (0 V if the strap said TCXO, 2.4 V if it said crystal) before it gives up, since `std_init` only falls back from TCXO to 0 V by itself; `main.cpp`'s one retry runs both again. `[indicator] radio tcxo requested=..` logs the request and `[indicator] radio up, tcxo used=..` the voltage the radio came up with.

`IndicatorBoard` overrides `getIRQGpio()` (42) and `sleep()` (a 1 ms delay) so the base class never uses the virtual expander pin for GPIO wake.

## Display

LovyanGFX 1.2.27 `Panel_ST7701` on `Bus_RGB`, 480x480, `use_psram` framebuffer, panel init list 1 as in the Meshtastic device-ui reference (vertical flip and horizontal flip), rotation fixed at 0 (UI rotation is `LV_DISP_ROT_NONE`; the keyboard-rotate controls are compiled out).

Timings: horizontal 8 / 50 / 10 (pulse / back porch / front porch), vertical 8 / 20 / 10, polarities 0, DE idle high, PCLK idle low and rising-edge active.

Pixel clock: `INDICATOR_PCLK_HZ=12000000` (12 MHz, 240 MHz / 20, about 42 Hz with these timings). The RGB bus reads the PSRAM framebuffer continuously and this core has no bounce buffers, so the limit is PSRAM bandwidth under load rather than the panel: 12 MHz keeps the picture stable under a busy UI with Wi-Fi traffic; higher clocks make it shift or flicker under that load. The value is a build flag, so trying another clock needs no source change.

LVGL draw buffer: a 480x24 band (23040 bytes), allocated in internal DMA RAM with PSRAM as the fallback. The other CPU-fed boards draw into a larger band in PSRAM; here the panel already scans its frame buffer out of PSRAM, so the band stays internal and the flush is a single copy into PSRAM. LVGL is compiled at -O2 (`custom_lvgl_o2`); its IRAM option (`custom_lvgl_iram`) is left off: it costs +5.9 KB IRAM, and with the 23 KB internal band that would leave less internal heap than the T-Deck's.

Home screen: the widget grid needs nothing board-specific. It sizes its cells from the shorter side of the panel, so the square panel gets about four columns by four rows at Normal, three by three at Large and two by two at Huge, and it keeps the layout saved for landscape, since the panel is not taller than wide. Other screens keep their portrait layout.

Brightness: the UI slider drives `Light_PWM`; 0 switches the backlight off (duty 0). Screen sleep writes 0 and wake restores the slider level.

Colour byte order: pixels are written as on the other LovyanGFX boards (`setSwapBytes(true)` with `LV_COLOR_16_SWAP` 0). Red, green and blue render correctly on the panel.

## Touch

FT6336 at 0x48 on the shared I2C bus, polled by the `indicator_touch` task on core 0 (3072-byte stack). Registers: status plus first point at 0x02 (5 bytes), chip id at 0xA3 (0x64 expected, logged, not fatal), vendor id at 0xA8 (logged). The reset pulse goes through the expander (P0.7) and runs on the first probe only. If the controller does not answer, the UI retries the probe at most every 2 s, and the failure is logged once.

Rotation: the controller reports both axes mirrored relative to the panel, so `x = 479 - raw_x` and `y = 479 - raw_y`, clamped to 0..479. The raw point is kept for the diagnostic overlay. Gesture thresholds (40 and 16 px, 12 ms) are shared with the Wio Tracker L2 touch code.

## Flash layout and image size

Partition table `variants/sensecap_indicator/partitions_sensecap_indicator.csv`, 8 MB:

| Name | Offset | Size |
|---|---|---|
| nvs | 0x9000 | 0x5000 |
| otadata | 0xe000 | 0x2000 |
| factory | 0x10000 | 0x480000 |
| spiffs | 0x490000 | 0x360000 |
| coredump | 0x7F0000 | 0x10000 |

One factory app slot of 4718592 bytes (0x480000) holds the image (about 3.4 MB). It is larger than the 0x3E0000 slots of the 16 MB boards, so an image that fits those fits here. SPIFFS is 3.375 MB, as on those boards. There is no second app slot, so in-place updates would need a new partition table and one USB install.

## Not in this port

- RP2040 peripherals: the buzzer, the microSD slot, the Grove I2C connector and the D1Pro sensors are not used. The RP2040 is only held out of reset. There is no storage card, so there is no SD-backed data.
- Maps need storage: the partition table has no map tile partition, so map tiles are unavailable until the SD slot or a tile partition is added.
- Deep sleep and power off: nothing can wake the board, so the UI hides Power off. The CLI `poweroff` command still enters deep sleep, as on the Wio Tracker L2; a reset or replug brings the board back.
- OTA release artifacts: this is a development target; no web-flasher or OTA release image is produced.
- Battery reporting: there is no battery; `getBattMilliVolts()` returns 0.

## Validation status

Exercised on a D1L: boot after a cold power-on, a reset and a reboot from the UI; colours and orientation; touch; the UI at Normal, Large and Huge; the Constellation screensaver and a radar widget on Home at 12 MHz; the user button; channel and direct messages sent and received with ACKs, including a two-hour direct-message soak with no reset; DM timestamps on the receiving node; the idle `/INT` wake rate. Not yet exercised: the D1Pro, the first boot after Meshtastic or the Seeed stock firmware, every backlight level, the spectrum scan.
