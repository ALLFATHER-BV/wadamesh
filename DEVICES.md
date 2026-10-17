# Supported devices

Current hardware support, install paths and maturity. "Stable" means the board
ships on the stable release channel; "Beta" means it is new and lives on the
test channel until the next stable promote. Install links:
[flasher.wadamesh.com](https://flasher.wadamesh.com) (browser, Chrome/Edge over
USB) and the [GitHub releases](https://github.com/ALLFATHER-BV/wadamesh/releases).

| Device | MCU / radio | Display and input | Install | Channel | Status |
|---|---|---|---|---|---|
| LilyGo T-Deck / T-Deck Plus | ESP32-S3, SX1262 | 2.8" 320x240 touch, QWERTY, trackball | Web flasher (standalone) or Launcher app image | Stable | Fully supported, reference device |
| LilyGo T-Deck Pro | ESP32-S3, SX1262 | 3.1" 240x320 e-paper touch, TCA8418 QWERTY | Development build only | Experimental | Initial target; display, touch, keyboard, radio, GPS and microSD validation pending (#62) |
| LilyGo T-Deck Max | ESP32-S3, SX1262 | 3.1" 240x320 e-paper touch, TCA8418 QWERTY | Web flasher | Experimental (new in beta_84) | The Pro's hardware with the peripheral power rails and resets on an XL9555 expander and a BQ27220 fuel gauge; contributed and tested by the author of PR #545 |
| Heltec V4 + TFT | ESP32-S3, SX1262 | 2.4" 240x320 touch (CHSC6x) | Web flasher | Stable | Fully supported; Expansion Kit sensors, V4.3 high-gain RX toggle |
| Tanmatsu | ESP32-P4 + ESP32-C6, SX1262 | 4" 800x480, 69-key keyboard (no touch) | Tanmatsu app store on the device (runs under the badge.team launcher, not web-flashable) | Store tracks the test channel | Fully supported; LoRa + Wi-Fi + Bluetooth simultaneously, standalone and companion in one |
| Elecrow ThinkNode M9 | ESP32-S3, LR1110 | 2.4" 240x320 (no touch), I2C QWERTY + d-pad | Web flasher | Beta (new in beta_38) | Hardware-complete community port by ded (#138): GPS, microSD, buzzer, lock screen, d-pad navigation |
| Elecrow CrowPanel Advance 3.5 | ESP32-S3, external SX1262 | 3.5" ILI9488 480x320 touch (GT911) | Web flasher (beta) | Beta, hardware validation pending | Requires an SX1262 module in the LoRa expansion slot; not for other CrowPanel models |
| Elecrow CrowPanel Advance 5.0 | ESP32-S3, external SX1262 | 5" ST7262 RGB 800x480 touch (GT911) | Web flasher (beta) | Beta, hardware-tested (display, touch, radio) | V1.1+ boards; SX1262 in the wireless slot with the function DIP switch on the wireless module; no SD, GPS or battery gauge |
| Seeed SenseCAP Indicator D1L | ESP32-S3, SX1262 | 4" 480x480 touch (FT6336) | Development build only | Experimental | Hardware-tested (display, touch, radio); USB-powered, no battery; the RP2040's buzzer and microSD are not used |
| RAK WisMesh Tap V2 (RAK3312) | ESP32-S3, SX1262 | Touch display (LovyanGFX, 30+ fps) | Web flasher | Beta (new in beta_38) | Early community port by Ethac.chen (#136); core mesh, chat and map working |
| LilyGo T-Lora Pager | ESP32-S3, LR1121 or SX1262 | 2.33" IPS LCD, 480x222 (no touch), QWERTY + rotary encoder | Web flasher (pick the build matching your radio) | Beta (new in beta_45) | Fully supported; keyboard-first navigation, microSD, map tile packs |
| Heltec V4-R8 + Expansion Kit V2 | ESP32-S3 (8 MB octal PSRAM), SX1262 | 2.4" touch (CHSC6x) | Web flasher | Beta (new in beta_45) | As the V4 plus microSD and the Expansion Kit sensors; buzzer supported |
| LilyGo T-Display P4 | ESP32-P4 + ESP32-C6, SX1262 or LR2021 | AMOLED or TFT-LCD, touch | Web flasher | Beta (new in beta_45) | Four panel/radio builds; AMOLED + SX1262 hardware-tested, the other combinations need validation |
| Elecrow CrowPanel Advanced 10.1 | ESP32-P4 + ESP32-C6, external SX1262 | 10.1" EK79007 MIPI-DSI 1024x600 touch (GT911) | Web flasher (beta) | Experimental (new in beta_97), not yet tested on hardware | One image per board revision (V1.0, V1.1, V1.2); SX1262 in the wireless slot; no battery reading, sound or GPS |
| Attaky Mesh Series | ESP32-S3, SX1262 | Touch display + front D-pad | Web flasher | Beta (new in beta_47) | Community port by attakygit (#158/#169); detachable keyboard supported; the front D-pad + SELECT navigate the UI |

## Feature notes per board

- **T-Deck**: the everything device: touch, physical keyboard, trackball cursor
  or d-pad navigation, microSD (deep 5000-message chat history, map tile packs,
  data storage), GPS on the Plus, notification sounds through the I2S speaker.
  Because the board has no battery-backed clock, Clock settings can optionally
  use a saved Wi-Fi network once after a true cold boot to obtain the time, then
  return Wi-Fi to off. This is off by default; saved open networks require a
  second explicit opt-in and unknown networks are never joined.
  Tap Sym or Alt for one symbol, or double-tap either to lock the symbol layer;
  this needs [LilyGO keyboard-controller firmware with raw matrix mode](https://github.com/Xinyuan-LilyGO/T-Deck/tree/master/examples/Keyboard_ESP32C3)
  (June 2025 or newer). Older controller firmware keeps normal typing and
  reports the unavailable latch mode on Serial.
- **T-Deck Pro**: separate `LilyGo_TDeck_Pro_v1_0_companion_radio_touch` and
  `LilyGo_TDeck_Pro_v1_1_companion_radio_touch` targets for the incompatible PCB
  revisions. V1.0 has no frontlight and uses GPIO45 for touch reset; V1.1 uses
  GPIO45 for its frontlight and GPIO38 for touch reset. Flashing the V1.1 image
  onto V1.0 holds touch in reset. Both use the GDEQ031T10 e-paper panel,
  CST328/CST3530 touch detection, and the Pro-specific TCA8418 matrix. See
  [variants/lilygo_tdeck_pro/PORT.md](variants/lilygo_tdeck_pro/PORT.md) before
  choosing a target.
- **T-Deck Max**: `LilyGo_TDeck_Max_companion_radio_touch`. The same e-paper,
  keyboard and radio as the Pro, whose display and touch drivers it reuses; the
  differences are an XL9555 expander in front of the peripheral power enables and
  resets, a BQ27220 fuel gauge, and the three capacitive pads under the glass.
  See variants/lilygo_tdeck_max/PORT.md.
- **Heltec V4 + TFT**: touch UI with the on-screen keyboard; the optional
  Expansion Kit adds environment sensors (home-screen chart) and a piezo
  buzzer. On the original Expansion Kit, tap **IO** for Back or hold it for one
  second to return Home; this reuses GPIO35, so that target no longer drives the
  same line as a TX LED. **PWR** remains the expansion board's hardware power
  control and is not readable by firmware. V4.3 boards get the switchable
  high-gain receive LNA toggle. Both V4 targets auto-detect an optional M5Stack
  CardKB at address `0x5F` on the board I2C bus (GPIO4/3 on V4, GPIO17/18 on
  V4-R8). It provides text and focus navigation; the on-screen keyboard remains
  available when CardKB is absent or when the active field is tapped again.
  V4-R8 also offers font-only Normal, Large and Huge text presets under Display;
  controls retain their normal geometry so the compact screen stays navigable.
- **Tanmatsu**: keyboard-driven UI (no touchscreen) with the coloured function
  keys mapped to tabs, ALT accent picker, UI scaling (Normal/Large/Huge),
  microSD for all persistent data. Ships through the Tanmatsu launcher store,
  updates arrive as store updates.
- **T-Lora Pager**: no touchscreen at all — the QWERTY keyboard and the rotary
  encoder drive everything (Alt+turn free-scrolls a page; see
  [TLORA_PAGER_SHORTCUTS.md](TLORA_PAGER_SHORTCUTS.md) for the full key map).
  Tap Fn/Alt for one symbol or double-tap it to lock the symbol layer; physical
  hold combinations keep their existing behavior.
  GPS, microSD, keyboard backlight, lock screen and notification sound through
  the onboard codec and amp all work. Two builds, one per radio: LR1121 and
  SX1262 — flashing the wrong one leaves you with no radio, so check the label
  on your unit. The onboard PCF85063A keeps time through a full power-off and is
  synchronized automatically whenever the firmware accepts time from NTP, GPS,
  a companion, CLI, or mesh bootstrap. A card the Pager detects but cannot read shows up in File
  Manager greyed, and tapping it retries the mount; formatting is deliberately
  left to a computer (FAT32) rather than done on-device.
- **ThinkNode M9**: keyboard plus d-pad navigation (no touch), same feature set
  as the other boards where the hardware allows. Every key and mode is covered
  in the [keyboard & d-pad guide](THINKNODE_M9_SHORTCUTS.md). Its PCF8563 is
  validated at boot; if it reports lost integrity, the same optional saved-Wi-Fi
  cold-boot sync offered on T-Deck is available in Clock settings. New in beta_38;
  report anything that feels off.
- **T-Display P4**: touch UI with the on-screen keyboard. The BOOT button wakes
  the screen (or lights the lock screen) and holding it for two seconds locks or
  unlocks, as the Wio Tracker L2's wake button does; "Lock when screen off" is
  under Settings, Lock screen. The optional clip-on
  keyboard expansion can be attached or removed while running and is picked up
  within a couple of seconds; while it is on, it types into the focused field
  and the on-screen keyboard stays down (tap the field again to bring it up).
  Arrows move the caret or the focus highlight, Esc goes back, F1-F5 open the
  tabs left to right, F11 steps the keyboard light through off, low, medium and
  high (the level is kept across reboots), Caps Lock lights its LEDs,
  and Shift, Sym and Alt apply to the next key. Alt+Backspace goes back.
- **RAK WisMesh Tap V2**: newest port, touch-driven. The browser-flash path is
  fresh; if the flasher cannot open the serial port, put the board in download
  mode manually and retry, and please report it.
- **Seeed Wio Tracker L2**: pre-release touch target under active bring-up. Builds are for
  development and hardware validation only; no public release artifact is
  promised until the port is verified. It has no battery-backed clock, so the
  same optional saved-Wi-Fi cold-boot time sync offered on T-Deck is available
  in Clock settings.
- **CrowPanel Advance 3.5**: `crowpanel_35_companion_radio_touch` is a
  16 MB flash / 8 MB OPI PSRAM build for the ILI9488/GT911 panel and an SX1262
  in the expansion slot. It starts upright in 480x320 landscape; Display
  settings can switch to 320x480 portrait, rotated 180 degrees. The selected
  orientation persists across reboots; landscape is only the default when no
  saved choice exists.
  It uses the same compact font and control sizing as Wio Tracker L2.
  Fit the radio before booting. GPIO0 is shared by BOOT and the radio's chip
  select, so BOOT is not configured as a wake/unlock input on stock wiring.
  Screen-off remains touch-wakeable; hard lock is disabled without a separate
  unlock button. See the
  [official schematic](https://github.com/Elecrow-RD/CrowPanel-Advance-HMI-ESP32-AI-Display/blob/master/3.5/schematic/ESP32%20Display%203.5%20inch%20V1.0.sch)
  and [reference radio pin map](https://github.com/meshtastic/firmware/blob/master/variants/esp32s3/elecrow_panel/variant.h).
  An optional external GPS can be connected to UART1 (CrowPanel RX GPIO18 /
  TX GPIO17, 9600 baud). Data
  can use a FAT32 microSD card for data, offline maps, File Manager and browser
  Transfer. The card uses software SPI (SCLK 5, MISO 4, MOSI 6, CS 7), leaving
  the display and radio SPI hosts untouched; missing or unreadable cards fall
  back to SPIFFS. The dedicated LittleFS partition remains the internal tile
  cache. Cards are never auto-formatted; format them on a computer. Serial
  uses the board's USB-to-UART bridge. Battery voltage monitoring is not
  supported. Display, touch, radio and SD still need validation on a physical
  CrowPanel. Like Wio Tracker L2, it is included in the all-target builds and local beta release
  scripts, not the two-board stable CI workflow. Select it in the upload
  helper with `scripts/build-upload-monitor.sh --crowpanel-35`.
- **CrowPanel Advance 5.0**: `crowpanel_50_companion_radio_touch` is a
  16 MB flash / 8 MB OPI PSRAM build for the 800x480 ST7262 RGB panel, the
  GT911 touch controller and an SX1262 in the wireless-module slot (J8/J9). The
  board quirks follow Camillia-MT's port:
  - The panel is driven over the 16-bit RGB bus from a PSRAM framebuffer at a
    14 MHz pixel clock (Elecrow's demo uses 21 MHz); the slower clock leaves
    PSRAM bandwidth for WiFi and LVGL so the picture does not drift or tear.
    LVGL's draw band stays in internal RAM for the same reason.
  - Flash writes (saving chats and settings) take the bus that the panel's DMA
    shares with PSRAM, which made the picture jump sideways. The firmware stops
    the LCD clock for each flash write or erase, so the screen briefly freezes
    or flickers instead.
  - Backlight and touch reset sit behind an I2C controller on GPIO15/16 that
    changed between revisions: a PCA9557 at 0x18 on V1.0, a board MCU at 0x30
    from V1.1. The driver detects which one answers, resets the GT911 through
    it and accepts the touch controller at either 0x5D or 0x14. Below full
    brightness the MCU dims by PWM, which can whine; 100% is steady.
  - The function DIP switch on the back routes GPIO 2/4/5/6/19/20 to the
    wireless slot, the microSD slot or the speaker/microphone. Set it to the
    wireless module (S1=0, S0=1), or the radio is not found (the serial log
    says so). The microSD slot is therefore unusable; browser Transfer uses the
    internal tiles partition instead, as on the Attaky.
  - The radio's chip select is GPIO8 on V1.2/V1.3 boards (current production)
    and GPIO0 on V1.0. The build targets GPIO8; a V1.0 board is detected at boot
    and logged, and needs `P_LORA_NSS=0`.
  - No GPS (no free GPIO pair for a UART; 17/18 are panel data), no battery
    voltage sensing and no free user button, so hard lock is disabled and the
    screen wakes by touch.
  It starts in native 800x480 landscape; Display settings can switch to
  480x800 portrait. It uses the large-screen layouts and UI-size presets of the
  Tanmatsu. Serial uses the board's CH340 USB-to-UART bridge. Tested on a physical
  unit: a steady picture with smooth gradients, responsive typing with key
  highlighting, the chat header and composer, and sending and receiving messages
  over the radio (flash writes show as a brief freeze or flicker), and a fresh
  full install and an on-device update, whose long flash erases the panel
  recovers from. Select it in
  the upload helper with `scripts/build-upload-monitor.sh --crowpanel-50`.
- **SenseCAP Indicator D1L**: `sensecap_indicator_companion_radio_touch` drives
  the 480x480 ST7701S RGB panel, the FT6336 touch controller and the SX1262,
  whose control lines sit on a TCA9535 I/O expander. It is USB-powered with no
  battery, so the battery readouts and Power off are hidden. The RP2040
  peripherals (buzzer, microSD, Grove, D1Pro sensors) are not used, and without
  storage there are no map tiles. The D1Pro shares the ESP32 side and is expected
  to run the same image, but is untested. See
  [variants/sensecap_indicator/PORT.md](variants/sensecap_indicator/PORT.md).

## Requested boards

Open hardware requests, roughly in demand order. Ports are welcome, see
[CONTRIBUTING.md](CONTRIBUTING.md); the M9 and Tap V2 both started as community
PRs.

- LilyGo T-Deck Pro hardware validation / T-Deck Max variants: [#62](https://github.com/ALLFATHER-BV/wadamesh/issues/62)

## Channels

- **Stable**: the tested default. The flasher installs it unless you pick Beta.
- **Beta**: new features and fixes earlier; on-device opt-in via Settings,
  About, "Get test builds (beta)". New boards debut here and move to Stable
  with the next promote.
