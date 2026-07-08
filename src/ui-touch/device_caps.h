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

#elif defined(TLORA_PAGER)              // ===== LilyGo T-LoRa Pager (ESP32-S3) =====
  #define CAP_TOUCH        0   // no touchscreen — keyboard + rotary encoder nav only
  #define CAP_ROTATABLE    0   // fixed 480x222 landscape via hardware MADCTL rotation
  #define CAP_LARGE_SCREEN 0   // native 480x222, no UI upscaling
  // CAP_SD/CAP_FILESYSTEM are 0 despite the hardware having a microSD slot:
  // the code these caps gate (fmSdTryMount(), the #include <SD.h> block, the
  // file manager's SD-vs-FFat backend selection) is still hardcoded to
  // HAS_TDECK_GT911/HAS_TANMATSU specifically, never migrated to be CAP_SD-
  // generic — turning these on here just hits "SD"/"CARD_NONE"/"fmSdTryMount"
  // undeclared, not real SD support. A real mount needs pager-specific wiring
  // (CS 21, its own shared-SPI helper), which is unscheduled follow-up work,
  // not part of this milestone.
  #define CAP_SD           0
  #define CAP_FILESYSTEM   0
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
// Physical keyboard: T-Deck matrix, Tanmatsu keypad, or the pager's TCA8418.
#if defined(HAS_TDECK_KEYBOARD) || defined(HAS_TANMATSU) || defined(HAS_PAGER_KEYBOARD)
  #define CAP_KEYBOARD 1
#else
  #define CAP_KEYBOARD 0
#endif

// Focus-group D-pad navigation (no pointer): Tanmatsu keypad, T-Deck trackball,
// or the pager (no touch at all — the rotary encoder is its only nav input,
// so like Tanmatsu this is always-on, not an optional toggle like the T-Deck's).
#if defined(HAS_TANMATSU) || defined(HAS_TDECK_TRACKBALL) || defined(TLORA_PAGER)
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

// Per-event WAV notification sounds + the file-browsing sound picker. This is
// deliberately NOT the same thing as CAP_SD/CAP_FILESYSTEM: it only means
// "can browse and play WAV files for notifications," which the T-Deck gets
// from its SD card and the pager gets from internal SPIFFS alone (the
// pager's CAP_SD/CAP_FILESYSTEM stay 0 -- see the comment on those above;
// this flag does not reopen real SD support, it only gates the
// SPIFFS-backed sound picker).
#if defined(HAS_TDECK_GT911) || defined(TLORA_PAGER)
  #define CAP_SOUND_FILES 1
#else
  #define CAP_SOUND_FILES 0
#endif
