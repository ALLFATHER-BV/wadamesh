#pragma once
// =============================================================================
//  device_caps.h — capability flags for the touch UI
//
//  Gate UI features on what a board CAN DO, not on its model name. This replaces
//  the scattered #if defined(HAS_TDECK_GT911) / HAS_TANMATSU / HELTEC_LORA_V4_TFT
//  checks throughout UITask.cpp, so adding a new device becomes "fill in one
//  capability block here" instead of hunting hundreds of device-name #ifs.
//
//  Rules:
//   * Every CAP_* is ALWAYS defined to 1 or 0 — gate with `#if CAP_X`, never
//     `#if defined(CAP_X)` (it is always defined).
//   * Caps newly factored out of the device-name macros are set per-board below.
//   * Caps that already had a clean, device-neutral macro (HAS_EXPANSION_KIT,
//     HAS_CC_*, HAS_UI_SOUND, MULTI_TRANSPORT_COMPANION, …) are ALIASED to it so
//     new code can be written all-CAP; existing sites can migrate opportunistically.
//   * Genuine one-off chip quirks (AppFS getSketchSize, FatFs f_mkfs, exact panel
//     rotation constants) stay on the raw device macro — they are not reusable
//     capabilities. Keep that allow-list short + commented at each site.
//
//  The board -D macros (HAS_TDECK_GT911, HAS_TANMATSU, HELTEC_LORA_V4_TFT, …) come
//  from platformio.ini / the Tanmatsu CMakeLists and are available everywhere, so
//  this header has no include dependencies. Only ui-touch/UITask.cpp includes it,
//  so the #else branch is always "the Heltec V4 TFT touch board".
// =============================================================================

// ---- Per-board structural capabilities (factored out of the device names) ----
#if defined(WADA_WEB) && defined(WADA_SIM_PAGER)   // ===== T-LoRa Pager simulator (web/, -DWADA_SIM_PAGER=ON) =====
  // The host build laid out as the Pager, for reviewing and scripting its screens off
  // the device: the Pager's own capability set (below), with the radio, card and
  // storage of the web board behind it (web/sim_pager/).
  #define CAP_TOUCH        0   // keyboard + rotary encoder only, as on the device
  #define CAP_ROTATABLE    0
  #define CAP_LARGE_SCREEN 0   // native 480x222
  #define CAP_SD           0   // no card in the simulator
  #define CAP_FILESYSTEM   0
  #define CAP_GPS          1
  #define CAP_GPS_RECEIVER 0
  #define CAP_OTA          0
  #define CAP_LOCK_SCREEN  1
  #define CAP_USB_FILES    0
  #define CAP_RADIO_TOOLS  0
  #define CAP_SCREEN_MIRROR 0
  #define CAP_WEB_BROWSER  0
  #define CAP_WIFI_SETUP   0
  #define CAP_BT_SETUP     0
  #define CAP_USB_FLASH    0
  #define CAP_FILE_HANDOFF 1
  #define CAP_NATIVE_KEYBOARD 0
  #define CAP_BUILTIN_LANGS    1
  #define CAP_BUILTIN_LUA_APPS 1
  #define CAP_STORE_DOWNLOADS  1
  #define CAP_FW_UPDATE_CHECK  0

#elif defined(WADA_WEB)                  // ===== Browser / desktop host build (web/, WEB_BUILD.md) =====
  // A virtual touch panel whose size the page picks at boot (WEB_BUILD.md "Phones and
  // tablets"): 800x480 landscape on a computer, the large-screen layouts the CrowPanel 5.0
  // and the Tanmatsu ship; on a phone or tablet the size of its screen below the page's
  // bar, portrait or landscape (e.g. 390x616 on an iPhone 14), on the same layouts, which
  // size themselves from the display (as on the portrait CrowPanel and the P4). Driven by
  // a mouse or a finger. The mesh behind it is a backend (the demo or a companion node),
  // so nothing radio- or hardware-bound is advertised.
  #define CAP_TOUCH        1   // mouse / touch
  #define CAP_ROTATABLE    0   // no rotation setting: the page follows the window, and a turn
                               // of the phone restarts the UI in the other layout (data kept)
  #define CAP_LARGE_SCREEN 1   // large-panel layouts and the UI-size fonts at every size
  #define CAP_SD           0   // no card; storage is the host directory behind SPIFFS
  #define CAP_FILESYSTEM   0
  #define CAP_GPS          1   // the computer's / phone's location (web/platform/WebLocation.h)
  #define CAP_GPS_RECEIVER 0   // ...not a serial receiver: no baud rate, no rest between checks
  #define CAP_OTA          0   // updated by reloading the page
  #define CAP_LOCK_SCREEN  0   // no unlock button
  #define CAP_BATTERY      0
  #define CAP_USB_FILES    0
  #define CAP_RADIO_TOOLS  0   // no radio of its own: no Signal / Discover / Spectrum
  #define CAP_SCREEN_MIRROR 0  // it IS the browser view: no Remote / VNC
  #define CAP_WEB_BROWSER  0   // a page cannot fetch arbitrary sites (CORS)
  #define CAP_WIFI_SETUP   0   // the page's bar picks the node and the link; the browser is online
  #define CAP_BT_SETUP     0
  #define CAP_USB_FLASH    0   // no bootloader to reboot into: no "Download mode"
  #define CAP_FILE_HANDOFF 1   // a backup is downloaded by the browser, imported with its file picker
  #define CAP_NATIVE_KEYBOARD 1   // on a phone the phone's own keyboard types (boardNativeKeyboard())
  #define CAP_BUILTIN_LANGS    1   // the translations and the catalog's apps ship in the
  #define CAP_BUILTIN_LUA_APPS 1   // build (offline, first load), and...
  #define CAP_STORE_DOWNLOADS  1   // ...the Store still installs and updates (same-origin proxy)
  #define CAP_FW_UPDATE_CHECK  0   // a reload is the update: no firmware check, no reports

#elif defined(HAS_CROWPANEL_35)          // ===== Elecrow CrowPanel Advance 3.5 (ESP32-S3) =====
  #define CAP_TOUCH        1   // GT911 capacitive touch
  #define CAP_ROTATABLE    1   // ILI9488 native portrait, upright landscape = rotation 3
  #define CAP_LARGE_SCREEN 0   // native 480x320, no upscaling
  #define CAP_SD           1   // microSD over software SPI, SD-compatible filesystem API
  #define CAP_FILESYSTEM   1   // internal SPIFFS + tiles LittleFS
  #define CAP_GPS          1   // optional external UART1 GPS
  #define CAP_OTA          1
  #define CAP_LOCK_SCREEN  0   // GPIO0 is LoRa NSS, no separate button to unlock
  #define CAP_BATTERY      0   // no battery voltage sensing

#elif defined(HAS_CROWPANEL_50)          // ===== Elecrow CrowPanel Advance 5.0 (ESP32-S3) =====
  #define CAP_TOUCH        1   // GT911 capacitive touch
  #define CAP_ROTATABLE    1   // ST7262 RGB panel, native 800x480 landscape = UI rotation 3
  #define CAP_LARGE_SCREEN 1   // native 800x480: large-panel layouts and the UI-size fonts
  // The function DIP switch gives GPIO4/5/6 to the radio, so the card slot is dead
  // while the radio works; File Transfer uses the internal tiles partition instead.
  #define CAP_SD           0
  #define CAP_FILESYSTEM   0
  #define CAP_GPS          0   // no free GPIO pair for a UART (the RGB bus takes 17/18)
  #define CAP_OTA          1
  #define CAP_LOCK_SCREEN  0   // no user button to unlock with
  #define CAP_BATTERY      0   // no battery voltage sensing

#elif defined(HAS_SENSECAP_INDICATOR)    // ===== Seeed SenseCAP Indicator D1L (ESP32-S3) =====
  #define CAP_TOUCH        1
  #define CAP_ROTATABLE    0
  #define CAP_LARGE_SCREEN 1   // 480x480
  #define CAP_SD           0   // the microSD slot is wired to the RP2040 co-processor, not the S3
  #define CAP_FILESYSTEM   1
  #define CAP_GPS          0
  #define CAP_OTA          0   // one factory app slot, no in-place update
  #define CAP_TILE_STORAGE 0   // no tiles partition and no card the map can use (#692)
  #define CAP_LOCK_SCREEN  0
  #define CAP_BATTERY      0   // no battery fitted

#elif defined(HAS_WIO_TRACKER_L2)        // ===== Seeed Wio Tracker L2 (ESP32-S3) =====
  #define CAP_TOUCH        1
  #define CAP_ROTATABLE    1   // native 320x240 landscape = UI rotation 3, as on the CrowPanels (#698)
  #define CAP_LARGE_SCREEN 0   // 320x240
  #define CAP_SD           0   // SD_MMC, not Arduino SD
  #define CAP_FILESYSTEM   1
  #define CAP_GPS          1
  #define CAP_OTA          1
  #define CAP_LOCK_SCREEN  1

#elif defined(HAS_TDECK_PRO)             // ===== LilyGo T-Deck Pro (ESP32-S3) =====
  #define CAP_TOUCH        1   // CST328 or CST3530 over I2C
  #define CAP_ROTATABLE    0   // fixed portrait 240x320 e-paper panel
  #define CAP_LARGE_SCREEN 0
  #define CAP_SD           1   // microSD on the shared display/radio SPI bus
  #define CAP_FILESYSTEM   1
  #define CAP_GPS          1   // u-blox MIA-M10Q
  #define CAP_OTA          1
  #define CAP_LOCK_SCREEN  1

#elif defined(HAS_TDECK_GT911)          // ===== LilyGo T-Deck (ESP32-S3) =====
  #define CAP_TOUCH        1   // capacitive touchscreen (pointer input)
  #define CAP_ROTATABLE    0   // panel is fixed landscape
  #define CAP_LARGE_SCREEN 0   // 320x240
  #define CAP_SD           1   // microSD on the shared SPI bus
  #define CAP_FILESYSTEM   1   // browsable filesystem (the SD card)
  #define CAP_GPS          1
  #define CAP_OTA          1   // native dual-OTA slot
  #define CAP_LOCK_SCREEN  1

#elif defined(TLORA_PAGER)              // ===== LilyGo T-LoRa Pager (ESP32-S3) =====
  #define CAP_TOUCH        0   // no touchscreen — keyboard + rotary encoder nav only
  #define CAP_ROTATABLE    0   // fixed 480x222 landscape via hardware MADCTL rotation
  #define CAP_LARGE_SCREEN 0   // native 480x222, no UI upscaling
  #define CAP_SD           1   // microSD on the shared display/radio SPI bus
  #define CAP_FILESYSTEM   1   // browsable filesystem (the SD card)
  #define CAP_GPS          1   // u-blox MIA-M10Q
  #define CAP_OTA          1   // dual-OTA partition layout, same shape as the T-Deck
  #define CAP_LOCK_SCREEN  1

#elif defined(HAS_TANMATSU)             // ===== Tanmatsu (ESP32-P4) =====
  #define CAP_TOUCH        0   // no touchscreen — keypad nav only
  #define CAP_ROTATABLE    0   // fixed (software ROT_270 portrait->landscape)
  #define CAP_LARGE_SCREEN 1   // 800x480 -> UI upscaling
  #define CAP_SD           0
  #define CAP_FILESYSTEM   1   // internal FFat partition
  #define CAP_GPS          0   // no GPS module
  #define CAP_OTA          0   // AppFS app — updated via launcher, no spare slot
  #define CAP_LOCK_SCREEN  1

#elif defined(HAS_THINKNODE_M9)         // ===== Heltec ThinkNode M9 (ESP32-S3) =====
  #define CAP_TOUCH        0   // no touchscreen — QWERTY keyboard + d-pad only
  #define CAP_ROTATABLE    0   // fixed landscape panel
  #define CAP_LARGE_SCREEN 0   // 240x320, same panel size as the T-Deck
  // microSD: on the shared LoRa SPI bus, same as T-Deck (Arduino SD, not
  // SD_MMC). CS is GPIO48 (schematic net SPICLK_N) — confirmed free on this
  // board's plain ESP32-S3R8 (the GPIO47/48 SPICLK_P/N differential-clock
  // reservation only applies to the R8V/R16V variants). See platformio.ini
  // for PIN_SD_CS.
  #define CAP_SD           1
  #define CAP_FILESYSTEM   1
  #define CAP_GPS          1   // CC1167Q on UART
  #define CAP_OTA          1   // 16 MB flash, dual A/B app slots (see partitions_tdeck_touch.csv)
  #define CAP_LOCK_SCREEN  1

#elif defined(HAS_RAK_TAP_V2)         // ===== RAK WisMesh Tap V2 (ESP32-S3) =====
  #define CAP_TOUCH        1   // FT5x06 capacitive touch
  #define CAP_ROTATABLE    0   // fixed landscape 320x240
  #define CAP_LARGE_SCREEN 0
  #define CAP_SD           0
  #define CAP_FILESYSTEM   1   // SPIFFS + tiles LittleFS
  #define CAP_GPS          1
  #define CAP_OTA          1
  #define CAP_LOCK_SCREEN  1

#elif defined(HELTEC_LORA_V4_R8)      // ===== Heltec WiFi LoRa 32 V4-R8 (ESP32-S3R8, 8 MB PSRAM) =====
  // Same board family + panel as the V4 TFT (the build also defines
  // HELTEC_LORA_V4_TFT to reuse all its UI code); the deltas are 8 MB octal
  // PSRAM (→ web browser) and a micro-SD slot on the Expansion Kit V2.
  #define CAP_TOUCH        1   // CHSC6x capacitive touch (Expansion Kit V2)
  // Rotation is OFF until a tester verifies the R8-specific landscape touch
  // maps (HeltecV4CapTouch.cpp, TESTER-VERIFY): the boot guard at UITask.cpp
  // ("V4-R8: force PORTRAIT at every boot") reverts any landscape pref anyway,
  // so with 1 the Orientation control was a reboot trap that always landed
  // back on Portrait (plus one garbled session, since the boot wordmark had
  // already rotated the panel). Flip back to 1 together with removing that
  // guard once landscape touch is confirmed on hardware.
  #define CAP_ROTATABLE    0
  #define CAP_LARGE_SCREEN 0   // 240x320
  #define CAP_SD           1   // micro-SD on Expansion Kit V2 (shared TFT SPI bus, CS=3)
  #define CAP_FILESYSTEM   1   // browsable filesystem (the SD card)
  #define CAP_GPS          1
  #define CAP_OTA          1   // native dual-OTA slot
  // 0: no unlock gesture exists yet — touch is deliberately inert while
  // hard-locked and BOOT already wake-unlocks in the generic branch. Enabling
  // needs a reveal+hold-to-unlock input path plus the DSEC_LOCK row gates
  // (see UITask.cpp lockScreen()/noteUserInput) — a feature, not a cap flip.
  #define CAP_LOCK_SCREEN  0

#elif defined(HAS_TDISPLAY_P4)        // ===== LilyGo T-Display P4 (ESP32-P4 + C6) =====
  // Phone-class handheld: RM69A10 AMOLED or HI8561 TFT, capacitive touch, SX1262 or LR2021,
  // C6 Wi-Fi/BLE (esp-hosted), SD_MMC. 32 MB PSRAM — web browser fits easily.
  #define CAP_TOUCH        1
  // The MIPI-DSI panel cannot MADCTL-rotate, so landscape is LVGL's software
  // rotation, exactly as the Tanmatsu does it on the same silicon. Opt-in from
  // Settings, Display; portrait stays the default.
  #define CAP_ROTATABLE    1
  #define CAP_LARGE_SCREEN 1   // 568x1232 -> UI upscaling like the Tanmatsu
  // CAP_SD gates the *Arduino SD* (shared-SPI) path used by the T-Deck/M9/R8. The P4's
  // card is SD_MMC (slot 0), mounted in main.cpp and used as the DataStore backend —
  // exposed to the UI via CAP_FILESYSTEM, exactly like the Tanmatsu's FFat. So CAP_SD=0
  // here keeps the Arduino-`SD` UI blocks (battery-log-on-SD, WAV sounds, fm SD mount)
  // compiled out; SD_MMC file-manager browsing can be wired via the filesystem path later.
  #define CAP_SD           0
  #define CAP_FILESYSTEM   1   // SD_MMC + internal FFat 'storage'
  #define CAP_GPS          1   // L76K
  #define CAP_OTA          1   // standalone dual-OTA app
  // The BOOT button (GPIO35, PIN_USER_BTN) works like the Wio Tracker L2's wake
  // button: a press wakes the screen or lights the lock screen, holding it 2 s
  // locks / unlocks. That is both the way to reach the lock and the deliberate
  // unlock #451 found missing; the lock hint names the button, and the Lock screen
  // settings page carries the "Lock when screen off" switch (as on the L2).
  #define CAP_LOCK_SCREEN  1

#elif defined(HAS_CROWPANEL_P4_101)   // ===== Elecrow CrowPanel Advanced 10.1" (ESP32-P4 + C6) =====
  // EXPERIMENTAL. 1024x600 EK79007 IPS on MIPI-DSI, GT911 touch, SX1262 module on the wireless
  // header, C6 Wi-Fi/BLE (esp-hosted; Elecrow documents 2.12.3, but V1.0 units have been seen with
  // 2.3.0, which still comes up: main.cpp logs the version), SD_MMC 1-bit. 32 MB PSRAM.
  #define CAP_TOUCH        1   // GT911 capacitive touch
  #define CAP_ROTATABLE    1   // native 1024x600 landscape = UI rotation 3; the DSI panel cannot rotate in
                               // hardware, so LVGL turns it in software like the T-Display P4 (#701)
  #define CAP_LARGE_SCREEN 1   // large-panel layouts + the UI-size fonts, rendered at native resolution
  #define CAP_SD           0   // card is SD_MMC (slot 0), exposed through CAP_FILESYSTEM like the T-Display P4
  #define CAP_FILESYSTEM   1   // internal LittleFS 'storage' + SD_MMC
  #define CAP_GPS          0   // no GPS fitted
  #define CAP_OTA          1   // released per revision from beta_97 (wadamesh-crowpanel-p4-101-v1x.bin)
  #define CAP_LOCK_SCREEN  0   // no wake/unlock path wired yet (the BOOT key on GPIO35 could become one)
  #define CAP_BATTERY      0   // battery charging + sensing belong to the on-board STC8, not the P4

#elif defined(ATTAKY_MESH_SERIES)
  #define CAP_TOUCH        1
  #define CAP_ROTATABLE    1   // landscape (ROT_90) by default; portrait for the build without a keyboard
  #define CAP_LARGE_SCREEN 0
  #define CAP_SD           0
  #define CAP_FILESYSTEM   0
  #define CAP_GPS          1
  #define CAP_OTA          1
  #define CAP_LOCK_SCREEN  0

#else                                    // ===== Heltec V4 TFT (default) =====
  #define CAP_TOUCH        1   // capacitive touch panel
  #define CAP_ROTATABLE    1   // user can flip portrait/landscape
  #define CAP_LARGE_SCREEN 0   // 240x320
  #define CAP_SD           0   // SPIFFS only
  #define CAP_FILESYSTEM   0   // no browsable filesystem
  #define CAP_GPS          1
  #define CAP_OTA          1
  #define CAP_LOCK_SCREEN  0
#endif

// Somewhere to keep map tiles: a tiles partition, or a card the map can fall back to.
// 0 = the map says this board has none, instead of "reflash the tiles partition".
#ifndef CAP_TILE_STORAGE
  #define CAP_TILE_STORAGE 1
#endif

// Boards without a battery ADC must not advertise battery history/calibration.
#ifndef CAP_BATTERY
  #define CAP_BATTERY 1
#endif

// Physical removable microSD slot, independent of the filesystem API used to
// drive it. CAP_SD means the SD-compatible SPI API (hardware or software);
// SD_MMC boards still need the same user-facing card diagnostics.
#if CAP_SD || defined(HAS_WIO_TRACKER_L2) || defined(HAS_TANMATSU) || defined(HAS_TDISPLAY_P4) || \
    defined(HAS_CROWPANEL_P4_101)
  #define CAP_MICROSD 1
#else
  #define CAP_MICROSD 0
#endif

// Offline maps from PMTiles PNG archives on the card (Map options -> Map file) and
// the "Zoom: map levels only" limit. ~11 KB of reader + UI, on every board with a card.
#ifndef CAP_MAP_PMTILES
  #if CAP_MICROSD
    #define CAP_MAP_PMTILES 1
  #else
    #define CAP_MAP_PMTILES 0
  #endif
#endif

// Map overzoom: a tile no offline source holds, while no download can bring it (Wi-Fi
// down), is drawn from its nearest ancestor up to 3 levels up, cut out and scaled 2x to
// 8x, instead of leaving a hole. It decodes into the slot's own pool buffer and needs one
// transient crop copy of at most 32 KB, so the 2 MB V4 can afford it like every board.
#ifndef CAP_MAP_OVERZOOM
  #define CAP_MAP_OVERZOOM 1
#endif

// No dedicated "tiles" LittleFS partition in the board's own partition table, and no free flash to
// add one without moving partitions that hold user data (16 MB maps: 2 x 4 MB OTA + 8000 KB storage
// + coredump fill the chip). The Wi-Fi map tile cache then goes to the microSD when one is mounted,
// else to the internal LittleFS 'storage' partition, where tilesFsLowSpace() keeps headroom free for
// identity, contacts and chat history.
#if defined(HAS_TDISPLAY_P4) || defined(HAS_CROWPANEL_P4_101)
  #define CAP_TILES_ON_STORAGE 1
#else
  #define CAP_TILES_ON_STORAGE 0
#endif

// Persisted, restart-to-apply UI-size selector. Large-screen boards already
// expose it; the Pager, V4-R8, ThinkNode M9, T-Deck and T-Deck Pro / Max add
// font-only presets because their compact viewports cannot safely take global
// geometry scaling.
// (The V4 stays without: its flash has no room for the bigger faces.)
#if CAP_LARGE_SCREEN || defined(TLORA_PAGER) || defined(HELTEC_LORA_V4_R8) || defined(HAS_THINKNODE_M9) || defined(HAS_TDECK_GT911) || \
    defined(HAS_TDECK_PRO)
  #define CAP_UI_SIZE 1
#else
  #define CAP_UI_SIZE 0
#endif

// ---- Derived input capabilities ---------------------------------------------
// Physical keyboard: T-Deck matrix, Tanmatsu keypad, the pager's TCA8418, or
// the ThinkNode M9 keyboard.
#if defined(HAS_TDECK_KEYBOARD) || defined(HAS_TANMATSU) || defined(HAS_PAGER_KEYBOARD) || defined(HAS_M9_KEYBOARD)
  #define CAP_KEYBOARD 1
#else
  #define CAP_KEYBOARD 0
#endif

// External Bluetooth LE keyboard (BleKeyboard.cpp): needs NimBLE-Arduino's central
// role, which every ESP32-S3 board's phone-app link already brings. Keyboard
// boards route its keys through handleHwKey(); the others type into the bound
// field and keep the on-screen keyboard down while it is connected. All of them
// drive the focus group with its arrows, Enter and Esc. The P4 boards are out:
// the T-Display P4's C6 firmware has no Bluetooth, and the Tanmatsu runs another
// NimBLE wrapper.
#if defined(BLE_PIN_CODE) && defined(MULTI_TRANSPORT_COMPANION) && \
    !defined(HAS_TANMATSU) && !defined(HAS_TDISPLAY_P4) && !defined(HAS_CROWPANEL_P4_101)
  #define CAP_BLE_KEYBOARD 1
#else
  #define CAP_BLE_KEYBOARD 0
#endif

// A keyboard that comes and goes at runtime and is routed like the Bluetooth
// one above (bleKbdDispatch() and friends): Bluetooth itself, an M5Stack CardKB,
// or the T-Display P4's clip-on keyboard expansion. CAP_BLE_KEYBOARD stays the
// gate for what is Bluetooth's alone (pairing, the Bluetooth page, BleKbd::).
#if CAP_BLE_KEYBOARD || defined(HAS_CARDKB) || defined(HAS_TDISPLAY_P4_KEYBOARD)
  #define CAP_EXT_KEYBOARD 1
#else
  #define CAP_EXT_KEYBOARD 0
#endif

// Focus-group D-pad navigation (no pointer): Tanmatsu keypad, T-Deck trackball,
// the pager (no touch at all — the rotary encoder is its only nav input, so
// like Tanmatsu this is always-on, not an optional toggle like the T-Deck's),
// the ThinkNode M9's d-pad, or the Attaky's front D-pad + SELECT. The underlying
// machinery (navFifo, navMoveDir, the focus group, the secondary KEYPAD indev)
// is generic — only the *pump* that feeds it differs per board: Tanmatsu's
// navPump() reads bsp-input events; T-Deck's WASDZ-letter nav and the M9's raw
// d-pad bytes are both fed straight from handleHwKey() instead (see UITask.cpp's
// `#elif defined(HAS_M9_KEYBOARD)` block, parallel to the T-Deck's `#if
// CAP_TRACKBALL` block); the Attaky drains its expander queue in attakyNavPump().
// NOTE: the Attaky is the first board here with CAP_KEYBOARD == 0, so anything
// this flag pulls in must not assume a physical keyboard is also compiled.
// Touchscreen-only boards that take a Bluetooth, CardKB or clip-on keyboard join
// too: the group stays empty (and invisible) until a keyboard connects.
#if defined(HAS_TANMATSU) || defined(HAS_TDECK_TRACKBALL) || defined(HAS_TDECK_PRO) || defined(TLORA_PAGER) || defined(HAS_THINKNODE_M9) || defined(ATTAKY_MESH_SERIES) || \
    (CAP_EXT_KEYBOARD && !CAP_KEYBOARD) || (defined(WADA_WEB) && defined(HAS_M9_KEYBOARD))
  // (The last term is the M9 flavour of the host build, web/CMakeLists.txt WADA_HOST_BOARD=m9:
  // the ThinkNode M9's key path on the web board, for scripted navigation tests.)
  #define CAP_KEYPAD_NAV 1
#else
  #define CAP_KEYPAD_NAV 0
#endif

// Battery-backed clock that keeps wall time through a TRUE power-off (issue #383).
// This is a DECLARED hardware fact, never "the generic 0x51 probe found something":
// address-only auto-detection is what read the Pager's PCF85063A as a PCF8563, and
// what let a chipless T-Deck look RTC-backed. A board earns a 1 here only when its
// target.cpp drives a documented chip (HardwareRtcClock) or calls the core probe
// against a chip the schematic confirms.
//
// 1 does NOT mean "the time is right": the chip's own integrity bit can say its
// contents are untrustworthy after a power cut (the ThinkNode M9 report on #383),
// and a dead backup cell reads the same way. That is a RUNTIME question — ask
// rtc_clock.timeIsCurrent() / .source(), not this flag. Use this one for what is
// physically fitted: diagnostics wording, and whether to probe at all.
#if defined(TLORA_PAGER) || defined(HAS_THINKNODE_M9) || defined(HAS_TDISPLAY_P4)
  #define CAP_HARDWARE_RTC 1
#else
  #define CAP_HARDWARE_RTC 0
#endif

// A true power cut can leave these boards without trustworthy wall time. The
// T-Deck has no RTC; the M9's PCF8563 can report lost integrity after shutdown.
// Both can opt into the bounded, pre-transport saved-Wi-Fi sync from #383. The
// T-Display P4's PCF8563 has come back asserting impossible dates, so it gets the
// same fallback -- on the esp-hosted C6 build only: the sync drives Arduino's real
// WiFi, which the legacy ESP-AT build must never touch. The Wio Tracker L2 has
// no RTC and an ESP32-S3 radio, so it gets the same opt-in.
#if defined(HAS_TDECK_GT911) || defined(HAS_TDECK_PRO) || defined(HAS_THINKNODE_M9) || \
    defined(HAS_WIO_TRACKER_L2) || (defined(HAS_TDISPLAY_P4) && TDP4_C6_HOSTED) || \
    defined(HAS_CROWPANEL_P4_101)
  #define CAP_BOOT_TIME_SYNC 1
#else
  #define CAP_BOOT_TIME_SYNC 0
#endif

// Round-cornered, tall/narrow phone-class panel (LilyGo T-Display P4, 568x1232). The
// AMOLED corners are arcs, so content is inset from all four corners, and the very tall
// aspect lets the status bar wrap to TWO rows (row 1 = name + clock, row 2 = the wifi/
// ble/sd/signal/battery cluster) instead of cramming everything onto one narrow line.
// Square-cornered panels leave this 0 and keep the single-row bar with no insets.
#if defined(HAS_TDISPLAY_P4)
  #define CAP_ROUND_CORNERS 1
#else
  #define CAP_ROUND_CORNERS 0
#endif

// An e-paper panel (the T-Deck Pro and Max, both HAS_TDECK_PRO). LVGL draws into a
// band that the driver copies, with the CPU, into its own PSRAM canvas, and every change
// on screen costs a panel update that blocks the loop for about 0.65 s (a full one for
// longer). So: the band needs no DMA-capable RAM, and anything that repaints often
// (progress, activity lights) has to be paced or left out.
#if defined(HAS_TDECK_PRO)
  #define CAP_EPAPER 1
#else
  #define CAP_EPAPER 0
#endif

// The Constellation screensaver (Constellation.cpp) animates continuously. E-paper
// cannot show that, and refreshing it to try would wear the panel, so the T-Deck
// Pro and Max (both HAS_TDECK_PRO) go without. Every other display gets it.
#if CAP_EPAPER
  #define CAP_SCREENSAVER 0
#else
  #define CAP_SCREENSAVER 1
#endif

// The GPS serial speed is found, not just set: these boards build their location
// provider with the UART probe (WadaNmeaLocationProvider), which tries the other
// speeds when the receiver does not answer and saves the one that works. The
// Settings choice is only where it starts (#613: the P4 "reverted" to 9600 because
// that is the speed its receiver talks).
#if defined(TLORA_PAGER) || defined(HAS_TDECK_PRO) || defined(HAS_TDISPLAY_P4)
  #define CAP_GPS_BAUD_AUTO 1
#else
  #define CAP_GPS_BAUD_AUTO 0
#endif

// The same probe, but only while the user has not picked a speed (Settings > GPS >
// Serial speed > Auto, the default): the T-Deck Plus GPS talks 38400 or 9600
// depending on the unit, newer ThinkNode M9s 115200. The speed it locks on is kept
// apart from the user's choice (touchPrefsSetGpsBaudFound), and a picked speed is
// used as it is, never probed past.
#if (defined(HAS_TDECK_GT911) && !defined(HAS_TDECK_PRO)) || defined(HAS_THINKNODE_M9)
  #define CAP_GPS_BAUD_FIND 1
#else
  #define CAP_GPS_BAUD_FIND 0
#endif

// A switchable LoRa antenna: an on-board antenna plus an external socket, picked at
// run time (T-Display P4: SKY13453 on XL9535 IO1). Gates the Radio settings antenna
// rows, the "Always use the external antenna" boot option and the control panel
// antenna tile. The board glue lives in UITask.cpp (antSelMode/antSelSet).
#if defined(HAS_TDISPLAY_P4)
  #define CAP_ANTENNA_SELECT 1
#else
  #define CAP_ANTENNA_SELECT 0
#endif

// ---- Capabilities aliased to existing device-neutral macros -----------------
//  (behaviour-identical; lets new code use CAP_* while old sites migrate lazily)
#if defined(HAS_TDECK_TRACKBALL)
  #define CAP_TRACKBALL 1
#else
  #define CAP_TRACKBALL 0
#endif

// Console mode (CONSOLE_MODE.md): a text front end drawn straight to the panel
// with no LVGL. It does NOT imply the device boots into it; that is a user pref
// read at startup.
//
// A DISPLAY IS NOT ENOUGH. This used to be gated on DISPLAY_CLASS alone, which
// reasons about output and says nothing about whether the user can type. The
// console's only input paths are the T-Deck keyboard, the M9 keyboard and the
// on-screen keypad (CAP_TOUCH), so on any other board it booted, painted
// "type 'ui' to go back", and accepted nothing: an unbootable-out state that
// survived a power cycle and a reset, because the pref persists and the panic
// self-heal never fires when nothing has actually crashed. A ThinkNode M9 was
// stranded exactly this way on beta_70 (gadgeteerza).
//
// Any board added here must have a drain in the console branch of UITask::loop.
#if defined(HAS_TDECK_KEYBOARD) || defined(HAS_M9_KEYBOARD) || CAP_TOUCH
  #define CAP_CONSOLE_INPUT 1
#else
  #define CAP_CONSOLE_INPUT 0
#endif
#ifndef CAP_CONSOLE
  #if defined(DISPLAY_CLASS) && CAP_CONSOLE_INPUT
    #define CAP_CONSOLE 1
  #else
    #define CAP_CONSOLE 0
  #endif
#endif

#if defined(HAS_EXPANSION_KIT)
  #define CAP_SENSORS 1
#else
  #define CAP_SENSORS 0
#endif

// Magnetometer readable by apps (wada.sys.compass(), caps().compass). A HARDWARE
// gate, deliberately separate from CAP_SENSORS (the V4 expansion-kit env
// sensors) and from the CAP_LUA_SDK_EXT memory gate: the ThinkNode M9's
// QMC6309 is the only one wired so far (variants/thinknode_m9/M9Compass.*).
#if defined(HAS_M9_COMPASS)
  #define CAP_COMPASS 1
#else
  #define CAP_COMPASS 0
#endif

// Accelerometer readable by apps (wada.sys.accel(), caps().accel). Same kind of
// hardware gate as CAP_COMPASS: the ThinkNode M9's QMI8658 is the only one
// driven so far (variants/thinknode_m9/M9Imu.*). Its point is tilt — a 2-axis
// magnetic heading is wrong by ~1.5 degrees per degree of tilt at mid latitudes.
#if defined(HAS_M9_IMU) || defined(TLORA_PAGER)   // + the Pager's BHI260AP (variants/lilygo_tlora_pager/PagerImu.*)
  #define CAP_IMU 1
#else
  #define CAP_IMU 0
#endif

// The constellation screensaver's optional note left of the battery
// (CstStatus::batt_note). Only the Pager fills it, from its fuel gauge, so only
// the Pager carries the label (the Tanmatsu has no flash to spare for an empty one).
#if defined(TLORA_PAGER)
  #define CAP_CST_BATT_NOTE 1
#else
  #define CAP_CST_BATT_NOTE 0
#endif

#if defined(HAS_CC_BRIGHTNESS)
  #define CAP_BACKLIGHT 1
#else
  #define CAP_BACKLIGHT 0
#endif

#if defined(HAS_CC_VOLUME)
  #define CAP_VOLUME 1
#else
  #define CAP_VOLUME 0
#endif

#if defined(HAS_CC_KBD_BACKLIGHT)
  #define CAP_KBD_BACKLIGHT 1
#else
  #define CAP_KBD_BACKLIGHT 0
#endif

// Notification-chime hardware: T-Deck I2S speaker, Pager codec, Heltec V4 / M9 GPIO
// piezo, T-Display P4 / T-Deck Max / Wio Tracker L2 ES8311. Kept in step with the HAS_UI_SOUND definition in
// UITask.cpp, which now derives FROM this rather than duplicating the list.
//
// This used to read `#if defined(HAS_UI_SOUND)`, but HAS_UI_SOUND is defined inside
// UITask.cpp roughly 1400 lines AFTER this header is included -- so CAP_SOUND was
// evaluated before it existed and came out 0 on every board, every time. Nothing
// consumed it, so it was silent rather than broken, but any future `#if CAP_SOUND`
// would have compiled sound out everywhere. Every macro tested here is a -D on the
// compiler command line, so they are all genuinely visible at this point.
#if defined(HAS_TDECK_GT911) || defined(HELTEC_V4_BUZZER_PIN) || defined(TLORA_PAGER) || \
    defined(THINKNODE_M9_BUZZER_PIN) || defined(HAS_TDISPLAY_P4) || defined(HAS_TDECK_MAX) || \
    defined(HAS_WIO_TRACKER_L2)
  #define CAP_SOUND 1
#else
  #define CAP_SOUND 0
#endif

#if defined(MULTI_TRANSPORT_COMPANION)
  #define CAP_COMPANION 1
#else
  #define CAP_COMPANION 0
#endif

// Per-event WAV notification sounds + the file-browsing sound picker. This is
// deliberately NOT the same thing as CAP_SD/CAP_FILESYSTEM: it only means
// "can browse and play WAV files for notifications." Both the T-Deck and the
// Pager provide that audio path; other boards may expose a filesystem without
// having compatible notification-sound hardware.
#if defined(HAS_TDECK_GT911) || defined(TLORA_PAGER) || defined(HAS_TDECK_MAX)
  #define CAP_SOUND_FILES 1
#else
  #define CAP_SOUND_FILES 0
#endif

// Sustained PCM output for media playback. This is deliberately independent
// of storage: Lua app files may live on internal flash, SD, or SD_MMC.
#if defined(HAS_TDECK_GT911) || defined(TLORA_PAGER) || \
    defined(HAS_TDISPLAY_P4) || defined(HAS_TANMATSU) || defined(HAS_TDECK_MAX)
  #define CAP_AUDIO_STREAM 1
#else
  #define CAP_AUDIO_STREAM 0
#endif

// ---- On-device web browser (the "Web" reader app) ---------------------------
// The reader fetches pages over on-device HTTPS, and a TLS handshake needs ~30 KB of
// free INTERNAL heap. Only the 8 MB-PSRAM boards (T-Deck, Tanmatsu, ThinkNode M9, RAK
// Tap) have that headroom; the 2 MB Heltec V4 TFT can't complete the handshake
// (fetch returns -1), so the Web app is gated out there. Tapping a link in chat still
// offers "Create QR" everywhere — only "Open in web" is gated to these boards.
#ifndef CAP_WEB_BROWSER
  #if defined(HELTEC_LORA_V4_TFT) && !defined(HELTEC_LORA_V4_R8)
    #define CAP_WEB_BROWSER 0   // 2 MB V4: TLS handshake can't fit
  #else
    #define CAP_WEB_BROWSER 1   // 8 MB boards incl. the V4-R8
  #endif
#endif

// ---- Radio tools + screen mirror ----------------------------------------------
// The drawer's Signal / Discover / Spectrum tiles drive the board's OWN LoRa radio,
// and Remote / VNC serve this screen to a browser. Every radio board has both; a
// board without its own radio (the browser build, WADA_WEB) opts out in its block.
#ifndef CAP_RADIO_TOOLS
  #define CAP_RADIO_TOOLS 1
#endif
#ifndef CAP_SCREEN_MIRROR
  #define CAP_SCREEN_MIRROR 1
#endif

// ---- Links the board sets up itself ------------------------------------------
// CAP_WIFI_SETUP / CAP_BT_SETUP: Settings > Wi-Fi and > Bluetooth configure the board's
// own radios (networks, the pairing code). A board whose links are chosen elsewhere
// (the browser build: the page's bar) shows what to do instead of switches that
// change nothing. CAP_USB_FLASH: the Power menu's "Download mode" (reboot into the
// ROM bootloader for a USB flash). CAP_GPS_RECEIVER: the GPS is a serial receiver
// (Settings > GPS offers its baud rate and the rest-between-checks saver); a position
// from elsewhere (the browser's location) has neither.
#ifndef CAP_WIFI_SETUP
  #define CAP_WIFI_SETUP 1
#endif
#ifndef CAP_BT_SETUP
  #define CAP_BT_SETUP 1
#endif
#ifndef CAP_USB_FLASH
  #define CAP_USB_FLASH 1
#endif
#ifndef CAP_GPS_RECEIVER
  #define CAP_GPS_RECEIVER 1
#endif
// CAP_FILE_HANDOFF: files leave and enter through the host instead of a card (the
// browser build: an exported backup is downloaded, an import is picked from the
// computer). The board provides boardHandOffFile / boardPickFile / boardTakePickedFile.
#ifndef CAP_FILE_HANDOFF
  #define CAP_FILE_HANDOFF 0
#endif
// CAP_NATIVE_KEYBOARD: the host may supply its own text input (a phone's keyboard in the
// browser build); while boardNativeKeyboard() is true the UI never brings up its own keys.
#ifndef CAP_NATIVE_KEYBOARD
  #define CAP_NATIVE_KEYBOARD 0
#endif

// ---- Lua app host (LUA_APPS.md) --------------------------------------------
// Sandboxed Lua 5.4 apps from the store catalog. Board-agnostic by design: the
// VM costs +96 KB flash and allocates exclusively from PSRAM (256 KB/app cap),
// so every touch board carries it. Opt out per-board above (define it 0 in the
// board's block) only if a flash ceiling ever demands it.
#ifndef CAP_LUA_APPS
  #define CAP_LUA_APPS 1
#endif

#ifndef CAP_LUA_AUDIO
  #if CAP_LUA_APPS && CAP_AUDIO_STREAM
    #define CAP_LUA_AUDIO 1
  #else
    #define CAP_LUA_AUDIO 0
  #endif
#endif

// ---- Extended Lua SDK ------------------------------------------------------
// The BASE SDK (drawing, timers, key/value store, read-only mesh + radio stats,
// http_get) is board-agnostic and ships everywhere CAP_LUA_APPS is on.
//
// The EXTENDED SDK is the part with real running cost, and it is gated:
//   wada.sys.battery/gps/sensors   live device state
//   wada.fs.*                      scoped file read/write/list under /apps
//   wada.net.http_post + wifi_scan
//   wada.mesh.send + on_message    the only WRITE path into the mesh
//   wada.ui.input/list + on_key    text entry, lists, key events
//
// Why it is not everywhere: these add per-app RAM (an inbound message queue, a
// scan buffer, retained Lua callbacks) and invite apps that hold buffers and do
// I/O. The 2 MB-PSRAM Heltec V4 already runs at ~95% internal RAM with Wi-Fi up
// — the same headroom problem that gates CAP_WEB_BROWSER and forces
// CAP_BUILTIN_LANGS there — so it gets the base SDK only.
//
// TO GATE A FUTURE LOW-RESOURCE BOARD, pick either:
//   * define WADA_LOW_RESOURCE_BOARD in the board's block  (also gates future extras), or
//   * define CAP_LUA_SDK_EXT 0 in the board's block        (gates only this)
// Apps must feature-detect with wada.sys.caps().sdk_ext rather than assume.
#ifndef CAP_LUA_SDK_EXT
  #if !CAP_LUA_APPS || defined(WADA_LOW_RESOURCE_BOARD) || \
      (defined(HELTEC_LORA_V4_TFT) && !defined(HELTEC_LORA_V4_R8))
    #define CAP_LUA_SDK_EXT 0   // 2 MB V4 (and anything marked low-resource)
  #else
    #define CAP_LUA_SDK_EXT 1   // 8 MB boards incl. the V4-R8
  #endif
#endif

// ---- USB Files ---------------------------------------------------------------
// files.wadamesh.com browses and edits the SD card and internal storage over the
// USB cable (Web Serial) while the USB Files app is open (UsbFilesSession.h).
// Every ESP32-S3 board whose USB socket is the chip's own USB (ARDUINO_USB_CDC_ON_BOOT):
// UsbFilesSession handles both of its serial drivers (HW-CDC and TinyUSB CDC), proven on
// the T-Deck and the Heltec V4. The T-Deck Pro and Max run the T-Deck's HW-CDC; on their
// e-paper the screen repaints paced (UsbFilesPaint.h) and the SD activity light stays
// off, so a transfer does not keep the panel refreshing and the loop blocked. Not yet:
// boards behind a USB-serial bridge chip (M9, CrowPanel 3.5/5.0, Attaky, SenseCAP
// Indicator: 115200 baud and a UART receive buffer that is fixed once running) and the
// ESP32-P4 builds (IDF).
#ifndef CAP_USB_FILES
  #if defined(ESP32) && ARDUINO_USB_CDC_ON_BOOT && \
      !defined(HAS_TDISPLAY_P4) && !defined(HAS_CROWPANEL_P4_101) && !defined(HAS_TANMATSU)
    #define CAP_USB_FILES 1
  #else
    #define CAP_USB_FILES 0
  #endif
#endif

// Read-only access to a physical SD card's directory tree from Lua. This first
// pass follows the existing shared-SPI Arduino SD lifecycle; P4 SD_MMC needs its
// own removal/recovery contract before it can safely expose the same API.
#ifndef CAP_LUA_SD_LIST
  #if CAP_LUA_SDK_EXT && CAP_SD
    #define CAP_LUA_SD_LIST 1
  #else
    #define CAP_LUA_SD_LIST 0
  #endif

// Reading the card from Lua: wada.sd.list and wada.audio.play("sd:/...").
// CAP_LUA_SD_LIST boards, plus the SD_MMC boards (T-Display P4, Tanmatsu, Wio
// Tracker L2) through the firmware's own SD_MMC mount. Only reads: the malware
// removal (sd_clean) stays CAP_LUA_SD_LIST, so an app never writes an SD_MMC card.
// Streaming a track is a few large sequential reads a second, the same traffic
// as the map tiles, not the bursts of small writes that flash the P4 panel (#167).
#ifndef CAP_LUA_SD_READ
  #if CAP_LUA_SD_LIST || (CAP_LUA_SDK_EXT && CAP_MICROSD && !CAP_SD)
    #define CAP_LUA_SD_READ 1
  #else
    #define CAP_LUA_SD_READ 0
  #endif
#endif
#endif

// Compile the translations into the image (i18n_builtin.h, generated from
// deploy/apps/lang/*.lang) instead of relying on downloading a .lang file.
// ON for boards where the Lua Store is not dependable — the V4 runs at ~95%
// internal RAM with Wi-Fi up, so its net worker and the store are fragile and
// it would otherwise be stuck on English. Costs ~400 KB of FLASH, no RAM.
// NOT the V4-R8, although its build defines HELTEC_LORA_V4_TFT to reuse the UI:
// it has 8 MB of PSRAM, so its net worker and the store work as well as on any
// other 8 MB board and it downloads its language like they do. Carrying the
// table anyway put its image over the app partition once the text-size presets
// added the larger fonts (101.3% of 3.875 MB).
#ifndef CAP_BUILTIN_LANGS
  #if !defined(HELTEC_LORA_V4_R8) && (defined(HELTEC_LORA_V4_TFT) || defined(HELTEC_LORA_V4))
    #define CAP_BUILTIN_LANGS 1
  #else
    #define CAP_BUILTIN_LANGS 0
  #endif
#endif

// Ship the catalog's Lua apps inside the image (lua_builtin.h, generated from
// deploy/apps/) so a board that cannot reach the Lua Store still has them in
// the drawer. A downloaded <data>/apps/<id>.lua always wins, because
// luaAppLaunchFile() is file-first. A board with built-in apps gets a Store
// without its Apps tab (nothing to browse), so this is for the 2 MB V4 only.
// NOT the V4-R8, although its build defines HELTEC_LORA_V4_TFT: with 8 MB of
// PSRAM its Store works like any other 8 MB board's, and baking the apps in had
// taken its Apps tab away (reported 2026-10-08).
#ifndef CAP_BUILTIN_LUA_APPS
  #if !defined(HELTEC_LORA_V4_R8) && (defined(HELTEC_LORA_V4_TFT) || defined(HELTEC_LORA_V4))
    #define CAP_BUILTIN_LUA_APPS 1
  #else
    #define CAP_BUILTIN_LUA_APPS 0
  #endif
#endif

// The Store downloads apps and languages (its Apps tab, the Languages downloads). A
// board that bakes them in (CAP_BUILTIN_*) shows a picker instead, unless it can also
// reach the Store (the browser build: CAP_STORE_DOWNLOADS 1, the baked copies are the
// offline first load and a download wins, as everywhere). CAP_FW_UPDATE_CHECK: the
// firmware version check (and the beta report / install count it carries); off, the
// Store's update badge fetches the catalog on its own. Defined after
// CAP_BUILTIN_LUA_APPS, which it reads: above it, an undefined macro reads 0 and the
// 2 MB V4 would get the downloading Store instead of its picker.
#ifndef CAP_STORE_DOWNLOADS
  #if CAP_BUILTIN_LUA_APPS
    #define CAP_STORE_DOWNLOADS 0
  #else
    #define CAP_STORE_DOWNLOADS 1
  #endif
#endif
#ifndef CAP_FW_UPDATE_CHECK
  #define CAP_FW_UPDATE_CHECK 1
#endif

// Settings > Contact Management: delete contacts not heard from in N days (#719).
// On every board; the Tanmatsu got room for it back when its image shrank by
// ~195 KB (silent asserts, deflated emoji), see sdkconfigs/wadamesh_tanmatsu.
#ifndef CAP_CONTACT_PURGE
  #define CAP_CONTACT_PURGE 1
#endif

// The two-line date of the lock screen and the Constellation screensaver (#742):
// a portrait or narrow panel at Large / Huge text wraps the day name over the rest.
// Off on the Tanmatsu, whose fixed 800 x 480 landscape panel always fits the date
// on one line, so the check is only image size its AppFS ceiling has no room for.
#ifndef CAP_DATE_WRAP
  #if defined(HAS_TANMATSU)
    #define CAP_DATE_WRAP 0
  #else
    #define CAP_DATE_WRAP 1
  #endif
#endif

// Settings > Display > Screensaver > "Keep north up" (#706): the Constellation stops
// its slow turn, so north stays at the top.
#ifndef CAP_SSAVER_NORTH_LOCK
  #if CAP_SCREENSAVER
    #define CAP_SSAVER_NORTH_LOCK 1
  #else
    #define CAP_SSAVER_NORTH_LOCK 0
  #endif
#endif

// Backups listed by their date, or "Default", instead of the file name (#739).
#ifndef CAP_BACKUP_DATE_LABELS
  #define CAP_BACKUP_DATE_LABELS 1
#endif
