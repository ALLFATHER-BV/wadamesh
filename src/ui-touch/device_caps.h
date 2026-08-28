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

#elif defined(ATTAKY_MESH_SERIES)
  #define CAP_TOUCH        1
  #define CAP_ROTATABLE    0
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

// Persisted, restart-to-apply UI-size selector. Large-screen boards already
// expose it; the Pager adds font-only presets because its 480x222 viewport is
// wide enough for larger type but too short for global geometry scaling.
#if CAP_LARGE_SCREEN || defined(TLORA_PAGER)
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
#if defined(HAS_TANMATSU) || defined(HAS_TDECK_TRACKBALL) || defined(TLORA_PAGER) || defined(HAS_THINKNODE_M9) || defined(ATTAKY_MESH_SERIES)
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
#if defined(HAS_M9_IMU)
  #define CAP_IMU 1
#else
  #define CAP_IMU 0
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
// "can browse and play WAV files for notifications." Both the T-Deck and the
// Pager provide that audio path; other boards may expose a filesystem without
// having compatible notification-sound hardware.
#if defined(HAS_TDECK_GT911) || defined(TLORA_PAGER)
  #define CAP_SOUND_FILES 1
#else
  #define CAP_SOUND_FILES 0
#endif

// Sustained PCM output for media playback. This is deliberately independent
// of storage: Lua app files may live on internal flash, SD, or SD_MMC.
#if defined(HAS_TDECK_GT911) || defined(TLORA_PAGER) || \
    defined(HAS_TDISPLAY_P4) || defined(HAS_TANMATSU)
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
#if defined(HELTEC_LORA_V4_TFT) && !defined(HELTEC_LORA_V4_R8)
  #define CAP_WEB_BROWSER 0   // 2 MB V4: TLS handshake can't fit
#else
  #define CAP_WEB_BROWSER 1   // 8 MB boards incl. the V4-R8
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

// Read-only access to a physical SD card's directory tree from Lua. This first
// pass follows the existing shared-SPI Arduino SD lifecycle; P4 SD_MMC needs its
// own removal/recovery contract before it can safely expose the same API.
#ifndef CAP_LUA_SD_LIST
  #if CAP_LUA_SDK_EXT && CAP_SD
    #define CAP_LUA_SD_LIST 1
  #else
    #define CAP_LUA_SD_LIST 0
  #endif
#endif

// Compile the translations into the image (i18n_builtin.h, generated from
// deploy/apps/lang/*.lang) instead of relying on downloading a .lang file.
// ON for boards where the Lua Store is not dependable — the V4 runs at ~95%
// internal RAM with Wi-Fi up, so its net worker and the store are fragile and
// it would otherwise be stuck on English. Costs ~400 KB of FLASH, no RAM.
#ifndef CAP_BUILTIN_LANGS
  #if defined(HELTEC_LORA_V4_TFT) || defined(HELTEC_LORA_V4)
    #define CAP_BUILTIN_LANGS 1
  #else
    #define CAP_BUILTIN_LANGS 0
  #endif
#endif

// Ship the catalog's Lua apps inside the image (lua_builtin.h, generated from
// out/firmware/apps/) so a board that cannot reach the Lua Store still has
// them in the drawer. A downloaded <data>/apps/<id>.lua always wins, because
// luaAppLaunchFile() is file-first. Costs ~10 KB of FLASH, no RAM.
#ifndef CAP_BUILTIN_LUA_APPS
  #if defined(HELTEC_LORA_V4_TFT) || defined(HELTEC_LORA_V4)
    #define CAP_BUILTIN_LUA_APPS 1
  #else
    #define CAP_BUILTIN_LUA_APPS 0
  #endif
#endif
