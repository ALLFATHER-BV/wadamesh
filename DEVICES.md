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

## Feature notes per board

- **T-Deck**: the everything device: touch, physical keyboard, trackball cursor
  or d-pad navigation, microSD (deep 5000-message chat history, map tile packs,
  data storage), GPS on the Plus, notification sounds through the I2S speaker.
- **Heltec V4 + TFT**: touch UI with the on-screen keyboard; the optional
  Expansion Kit adds environment sensors (home-screen chart) and a piezo
  buzzer. V4.3 boards get the switchable high-gain receive LNA toggle.
- **Tanmatsu**: keyboard-driven UI (no touchscreen) with the coloured function
  keys mapped to tabs, ALT accent picker, UI scaling (Normal/Large/Huge),
  microSD for all persistent data. Ships through the Tanmatsu launcher store,
  updates arrive as store updates.
- **ThinkNode M9**: keyboard plus d-pad navigation (no touch), same feature set
  as the other boards where the hardware allows. New in beta_38; expect rough
  edges and report them.
- **RAK WisMesh Tap V2**: newest port, touch-driven. The browser-flash path is
  fresh; if the flasher cannot open the serial port, put the board in download
  mode manually and retry, and please report it.

## Requested boards

Open hardware requests, roughly in demand order. Ports are welcome, see
[CONTRIBUTING.md](CONTRIBUTING.md); the M9 and Tap V2 both started as community
PRs.

- LilyGo T-Deck Pro Max: [#62](https://github.com/ALLFATHER-BV/wadamesh/issues/62)
- LilyGo T-pager: [#60](https://github.com/ALLFATHER-BV/wadamesh/issues/60)
- SenseCAP Indicator D1L: [#14](https://github.com/ALLFATHER-BV/wadamesh/issues/14)

## Channels

- **Stable**: the tested default. The flasher installs it unless you pick Beta.
- **Beta**: new features and fixes earlier; on-device opt-in via Settings,
  About, "Get test builds (beta)". New boards debut here and move to Stable
  with the next promote.
