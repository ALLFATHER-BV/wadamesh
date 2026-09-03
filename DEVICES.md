# Supported devices

Current hardware support, install paths and maturity. "Stable" means the board
ships on the stable release channel; "Beta" means it is new and lives on the
test channel until the next stable promote. Install links:
[flasher.wadamesh.com](https://flasher.wadamesh.com) (browser, Chrome/Edge over
USB) and the [GitHub releases](https://github.com/ALLFATHER-BV/wadamesh/releases).

| Device | MCU / radio | Display and input | Install | Channel | Status |
|---|---|---|---|---|---|
| LilyGo T-Deck / T-Deck Plus | ESP32-S3, SX1262 | 2.8" 320x240 touch, QWERTY, trackball | Web flasher (standalone) or Launcher app image | Stable | Fully supported, reference device |
| Heltec V4 + TFT | ESP32-S3, SX1262 | 2.4" 240x320 touch (CHSC6x) | Web flasher | Stable | Fully supported; Expansion Kit sensors, V4.3 high-gain RX toggle |
| Tanmatsu | ESP32-P4 + ESP32-C6, SX1262 | 4" 800x480, 69-key keyboard (no touch) | Tanmatsu app store on the device (runs under the badge.team launcher, not web-flashable) | Store tracks the test channel | Fully supported; LoRa + Wi-Fi + Bluetooth simultaneously, standalone and companion in one |
| Elecrow ThinkNode M9 | ESP32-S3, LR1110 | 2.4" 240x320 (no touch), I2C QWERTY + d-pad | Web flasher | Beta (new in beta_38) | Hardware-complete community port by ded (#138): GPS, microSD, buzzer, lock screen, d-pad navigation |
| RAK WisMesh Tap V2 (RAK3312) | ESP32-S3, SX1262 | Touch display (LovyanGFX, 30+ fps) | Web flasher | Beta (new in beta_38) | Early community port by Ethac.chen (#136); core mesh, chat and map working |
| LilyGo T-Lora Pager | ESP32-S3, LR1121 or SX1262 | 2.33" IPS LCD, 480x222 (no touch), QWERTY + rotary encoder | Web flasher (pick the build matching your radio) | Beta (new in beta_45) | Fully supported; keyboard-first navigation, microSD, map tile packs |
| Heltec V4-R8 + Expansion Kit V2 | ESP32-S3 (8 MB octal PSRAM), SX1262 | 2.4" touch (CHSC6x) | Web flasher | Beta (new in beta_45) | As the V4 plus microSD and the Expansion Kit sensors; buzzer supported |
| LilyGo T-Display P4 | ESP32-P4 + ESP32-C6, SX1262 | AMOLED or TFT-LCD, touch | Web flasher (AMOLED) or the .bin for the LCD SKU | Beta (new in beta_45) | Two screen SKUs; fuel-gauge battery reporting, full TX power |
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
- **Heltec V4 + TFT**: touch UI with the on-screen keyboard; the optional
  Expansion Kit adds environment sensors (home-screen chart) and a piezo
  buzzer. V4.3 boards get the switchable high-gain receive LNA toggle.
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
- **RAK WisMesh Tap V2**: newest port, touch-driven. The browser-flash path is
  fresh; if the flasher cannot open the serial port, put the board in download
  mode manually and retry, and please report it.
- **Seeed Wio Tracker L2**: pre-release touch target under active bring-up. Builds are for
  development and hardware validation only; no public release artifact is
  promised until the port is verified.

## Requested boards

Open hardware requests, roughly in demand order. Ports are welcome, see
[CONTRIBUTING.md](CONTRIBUTING.md); the M9 and Tap V2 both started as community
PRs.

- LilyGo T-Deck Pro Max: [#62](https://github.com/ALLFATHER-BV/wadamesh/issues/62)
- SenseCAP Indicator D1L: [#14](https://github.com/ALLFATHER-BV/wadamesh/issues/14)

## Channels

- **Stable**: the tested default. The flasher installs it unless you pick Beta.
- **Beta**: new features and fixes earlier; on-device opt-in via Settings,
  About, "Get test builds (beta)". New boards debut here and move to Stable
  with the next promote.
