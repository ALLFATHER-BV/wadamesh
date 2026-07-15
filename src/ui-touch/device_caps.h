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
#if defined(HAS_TDECK_GT911)            // ===== LilyGo T-Deck (ESP32-S3) =====
  #define CAP_TOUCH        1   // capacitive touchscreen (pointer input)
  #define CAP_ROTATABLE    0   // panel is fixed landscape
  #define CAP_LARGE_SCREEN 0   // 320x240
  #define CAP_SD           1   // microSD on the shared SPI bus
  #define CAP_FILESYSTEM   1   // browsable filesystem (the SD card)
  #define CAP_GPS          1
  #define CAP_OTA          1   // native dual-OTA slot
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
  #define CAP_ROTATABLE    1   // user can flip portrait/landscape
  #define CAP_LARGE_SCREEN 0   // 240x320
  #define CAP_SD           1   // micro-SD on Expansion Kit V2 (shared TFT SPI bus, CS=3)
  #define CAP_FILESYSTEM   1   // browsable filesystem (the SD card)
  #define CAP_GPS          1
  #define CAP_OTA          1   // native dual-OTA slot
  #define CAP_LOCK_SCREEN  0

#elif defined(HAS_TDISPLAY_P4)        // ===== LilyGo T-Display P4 (ESP32-P4 + C6) =====
  // Phone-class AMOLED handheld: RM69A10 MIPI-DSI 568x1232 portrait, HI8561 cap touch, SX1262,
  // C6 Wi-Fi/BLE (esp-hosted), SD_MMC. 32 MB PSRAM — web browser fits easily.
  #define CAP_TOUCH        1
  #define CAP_ROTATABLE    0   // fixed portrait panel (SW-rotate later if wanted)
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
  #define CAP_LOCK_SCREEN  1

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

// ---- Derived input capabilities ---------------------------------------------
// Physical keyboard: T-Deck matrix OR Tanmatsu keypad OR ThinkNode M9 keyboard.
#if defined(HAS_TDECK_KEYBOARD) || defined(HAS_TANMATSU) || defined(HAS_M9_KEYBOARD)
  #define CAP_KEYBOARD 1
#else
  #define CAP_KEYBOARD 0
#endif

// Focus-group D-pad navigation (no pointer): Tanmatsu keypad OR T-Deck trackball
// OR the ThinkNode M9's d-pad. The underlying machinery (navFifo, navMoveDir,
// the focus group, the secondary KEYPAD indev) is generic — only the *pump*
// that feeds it differs per board: Tanmatsu's navPump() reads bsp-input events;
// T-Deck's WASDZ-letter nav and the M9's raw d-pad bytes are both fed straight
// from handleHwKey() instead (see UITask.cpp's `#elif defined(HAS_M9_KEYBOARD)`
// block, parallel to the T-Deck's `#if CAP_TRACKBALL` block).
#if defined(HAS_TANMATSU) || defined(HAS_TDECK_TRACKBALL) || defined(HAS_THINKNODE_M9)
  #define CAP_KEYPAD_NAV 1
#else
  #define CAP_KEYPAD_NAV 0
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

// ---- Capabilities aliased to existing device-neutral macros -----------------
//  (behaviour-identical; lets new code use CAP_* while old sites migrate lazily)
#if defined(HAS_TDECK_TRACKBALL)
  #define CAP_TRACKBALL 1
#else
  #define CAP_TRACKBALL 0
#endif

#if defined(HAS_EXPANSION_KIT)
  #define CAP_SENSORS 1
#else
  #define CAP_SENSORS 0
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

#if defined(HAS_UI_SOUND)
  #define CAP_SOUND 1
#else
  #define CAP_SOUND 0
#endif

#if defined(MULTI_TRANSPORT_COMPANION)
  #define CAP_COMPANION 1
#else
  #define CAP_COMPANION 0
#endif

// ---- On-device web browser (the "Web" reader app) ---------------------------
// The reader fetches pages over on-device HTTPS, and a TLS handshake needs ~30 KB of
// free INTERNAL heap. Only the 8 MB-PSRAM boards (T-Deck, Tanmatsu, ThinkNode M9, RAK
// Tap) have that headroom; the 2 MB Heltec V4 TFT can't complete the handshake
// (fetch returns -1), so the Web app is gated out there. Tapping a link in chat still
// offers "Create QR" everywhere — only "Open in web" is gated to these boards.
#if defined(HELTEC_LORA_V4_TFT) && !defined(HELTEC_LORA_V4_R8)
  #define CAP_WEB_BROWSER 0   // 2 MB V4: TLS handshake can't fit
#else
  #define CAP_WEB_BROWSER 1   // 8 MB boards incl. the V4-R8
#endif
