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
