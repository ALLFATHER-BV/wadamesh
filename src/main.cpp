#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"
#if defined(ESP32_PLATFORM) && defined(HAS_TOUCH_UI)
#include <Preferences.h>
#include <esp_system.h>
#include <esp_ota_ops.h>     // recovery-first boot: running slot + reset the boot pointer to factory
#include <esp_partition.h>   // find/erase otadata so the bootloader returns to the recovery
#include <helpers/TouchDiagTrace.h>
#include <helpers/MeshTouchTxTrace.h>
#include "helpers/esp32/TouchPrefsStore.h"   // QUOTED: get wadamesh's copy (touchPrefsReload), not the lib's stale one
#include "helpers/esp32/SdNvsPrefs.h"        // route prefs to file storage (SD/SPIFFS), off NVS
                                             // (quoted: use wadamesh's src/ copy, not the lib's stale one)
#include "wadamesh_mark_rgb.h"               // anti-aliased mesh-mark (RGB565) for the pre-LVGL boot screen
#include "ui-touch/TouchSleep.h"             // idle light-sleep controller (loopEnd called at end of loop())
#endif

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
    DataStore store(InternalFS, QSPIFlash, rtc_clock);
  #else
  #if defined(EXTRAFS)
    #include <CustomLFS.h>
    CustomLFS ExtraFS(0xD4000, 0x19000, 128);
    DataStore store(InternalFS, ExtraFS, rtc_clock);
  #else
    DataStore store(InternalFS, rtc_clock);
  #endif
  #endif
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
  DataStore store(LittleFS, rtc_clock);
#elif defined(ESP32)
  #include <SPIFFS.h>
  #if defined(HAS_TDECK_GT911)
    #include <SD.h>
    #include <Preferences.h>
    #ifndef PIN_SD_CS
      #define PIN_SD_CS 39      // T-Deck microSD chip-select
    #endif
  #endif
  extern "C" void set_boot_phase(int phase);
  namespace { struct MainBootTrace { MainBootTrace() { set_boot_phase(2); } } _main_boot_trace; }
  DataStore store(SPIFFS, rtc_clock);
  #if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
    #include "WiFiConfig.h"
  #endif
#endif

#ifdef ESP32
  #ifdef MULTI_TRANSPORT_COMPANION
    #include <helpers/esp32/MultiTransportCompanionInterface.h>
    MultiTransportCompanionInterface serial_interface;
    #ifndef TCP_PORT
      #define TCP_PORT 5000
    #endif
    #ifndef WS_PORT
      #define WS_PORT 8765
    #endif
  #elif defined(WIFI_SSID)
    #include <helpers/esp32/SerialWifiInterface.h>
    SerialWifiInterface serial_interface;
    #ifndef TCP_PORT
      #define TCP_PORT 5000
    #endif
  #elif defined(BLE_PIN_CODE)
    #include <helpers/esp32/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
  #elif defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(RP2040_PLATFORM)
  //#ifdef WIFI_SSID
  //  #include <helpers/rp2040/SerialWifiInterface.h>
  //  SerialWifiInterface serial_interface;
  //  #ifndef TCP_PORT
  //    #define TCP_PORT 5000
  //  #endif
  // #elif defined(BLE_PIN_CODE)
  //   #include <helpers/rp2040/SerialBLEInterface.h>
  //   SerialBLEInterface serial_interface;
  #if defined(SERIAL_RX)
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
    HardwareSerial companion_serial(1);
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(NRF52_PLATFORM)
  #ifdef BLE_PIN_CODE
    #include <helpers/nrf52/SerialBLEInterface.h>
    SerialBLEInterface serial_interface;
  #else
    #include <helpers/ArduinoSerialInterface.h>
    ArduinoSerialInterface serial_interface;
  #endif
#elif defined(STM32_PLATFORM)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface serial_interface;
#else
  #error "need to define a serial interface"
#endif

/* GLOBAL OBJECTS */
#ifdef DISPLAY_CLASS
  #include "UITask.h"
  UITask ui_task(&board, &serial_interface);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
);

/* END GLOBAL OBJECTS */

#if defined(ESP32)
volatile int g_boot_phase = 0;
extern "C" void set_boot_phase(int phase) { g_boot_phase = phase; }
#endif


void halt() {
  while (1) ;
}

/* WIFI RECONNECT TRACKERS */
#if defined(ESP32) && defined(WIFI_SSID)
  bool wifi_needs_reconnect = false;
  unsigned long last_wifi_reconnect_attempt = 0;
#endif

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[BOOT] setup start");

#if defined(ESP32_PLATFORM) && defined(HAS_TOUCH_UI)
  // Record which slot we booted from so the recovery's "Boot firmware" can return
  // here. Recovery-first itself is enforced by the CUSTOM bootloader (it boots
  // factory by default and an ota slot only on its one-shot flag), so we must NOT
  // touch otadata here — otadata just tracks which A/B slot is current, and the
  // bootloader's default-to-factory is what makes recovery survive ANY app
  // (Meshtastic included). Skipped where there's no factory partition (V4 /
  // standalone dual-OTA T-Deck).
  {
    const esp_partition_t* fac =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (fac) {
      const esp_partition_t* run = esp_ota_get_running_partition();
      Preferences pslot;
      if (pslot.begin("mcboot", false)) {
        pslot.putString("slot", (run && run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) ? "ota_1" : "ota_0");
        pslot.end();
      }
    }
  }
#endif
  {
    bool aes_ok = mesh::Utils::selfTestAES();
    Serial.printf("[BOOT] AES self-test: %s\n", aes_ok ? "PASS" : "FAIL");
  #if defined(ESP32_PLATFORM) && defined(HAS_TOUCH_UI)
    mesh_touch_tx_tracef("AES_SELFTEST: %s", aes_ok ? "PASS" : "FAIL");
  #endif
  }

  board.begin();
  Serial.println("[BOOT] board ok");

#ifdef DISPLAY_CLASS
  DisplayDriver* disp = NULL;
  if (display.begin()) {
    disp = &display;
#if defined(ESP32_PLATFORM) && defined(HAS_TOUCH_UI)
    // Rotate the panel to the saved UI orientation BEFORE painting the boot
    // wordmark, so it's upright in landscape too (UITask applies the same
    // hardware rotation later for the LVGL UI). ROT_90->1, ROT_270->3.
    {
      uint8_t r = touchPrefsGetUiRotation();
      if (r == 1)      display.setDisplayRotation(1);
      else if (r == 3) display.setDisplayRotation(3);
    }
#endif
    // Paint the WADAMESH mesh mark the instant the panel is up, so the logo is on
    // screen from power-on — before LVGL is ready. Blitted as an anti-aliased
    // RGB565 bitmap via the full-res LVGL path (writePixelsRGB565), so the
    // diagonals are smooth, not 1-bit jagged. White-on-black here; the teal dots
    // arrive with the LVGL splash. Centred exactly: the artwork is centred within
    // the bitmap, and the colour splash mark is centred to the same point, so the
    // hand-off stays in place.
    display.startFrame();
    display.writePixelsRGB565((display.width()  - WADAMESH_MARK_W) / 2,
                              (display.height() - WADAMESH_MARK_H) / 2,
                              WADAMESH_MARK_W, WADAMESH_MARK_H, WADAMESH_MARK_RGB565);
    display.endFrame();
  }
#endif

  if (!radio_init()) { halt(); }
  Serial.println("[BOOT] radio ok");

  fast_rng.begin(radio_driver.getRngSeed());

#if defined(ESP32_PLATFORM) && defined(HAS_TOUCH_UI)
  {
    Preferences prefs;
    if (prefs.begin("mcboot", false)) {
      uint32_t bn = prefs.getUInt("n", 0);
      ++bn;
      prefs.putUInt("n", bn);
      size_t nvs_free = prefs.freeEntries();   // NVS partition headroom; near 0 = full (the boot-loop trigger)
      prefs.end();
      meshcomod_touch_set_boot_stats(bn, static_cast<uint8_t>(esp_reset_reason()));
      Serial.printf("[BOOT] touch_boot_n=%lu reason=%u nvs_free_entries=%u\n",
                    static_cast<unsigned long>(bn),
                    static_cast<unsigned>(esp_reset_reason()),
                    static_cast<unsigned>(nvs_free));
    }
    the_mesh.initTxtTxUniquenessFromRng();
  }
#endif

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  #if defined(QSPIFLASH)
    if (!QSPIFlash.begin()) {
      // debug output might not be available at this point, might be too early. maybe should fall back to InternalFS here?
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: failed to initialize");
    } else {
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: initialized successfully");
    }
  #else
  #if defined(EXTRAFS)
      ExtraFS.begin();
  #endif
  #endif
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

#ifdef BLE_PIN_CODE
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#else
  serial_interface.begin(Serial);
#endif
  the_mesh.startInterface(serial_interface);
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );

  //#ifdef WIFI_SSID
  //  WiFi.begin(WIFI_SSID, WIFI_PWD);
  //  serial_interface.begin(TCP_PORT);
  // #elif defined(BLE_PIN_CODE)
  //   char dev_name[32+16];
  //   sprintf(dev_name, "%s%s", BLE_NAME_PREFIX, the_mesh.getNodeName());
  //   serial_interface.begin(dev_name, the_mesh.getBLEPin());
  #if defined(SERIAL_RX)
    companion_serial.setPins(SERIAL_RX, SERIAL_TX);
    companion_serial.begin(115200);
    serial_interface.begin(companion_serial);
  #else
    serial_interface.begin(Serial);
  #endif
    the_mesh.startInterface(serial_interface);
#elif defined(ESP32)
  // Storage selection. SPIFFS by default; use the SD card under /meshcomod when
  // SPIFFS is unavailable (e.g. installed under Launcher) OR the user opted in
  // ("Store data on SD"). The SD shares the LoRa SPI bus, already brought up by
  // radio_init() above, so SD.begin's spi.begin is a no-op. Graceful: any SD
  // failure falls back to SPIFFS so the device always boots.
  bool spiffs_ok = SPIFFS.begin(false);   // try first WITHOUT auto-format
  bool sd_storage = false;
#if defined(HAS_TDECK_GT911)
  {
    extern SPIClass* tdeckSharedSPI();
    bool use_sd_pref = false, setup_done = false;
    { Preferences _p; if (_p.begin("touch", true)) {
        use_sd_pref = _p.getBool("use_sd", false);    // explicit user choice
        setup_done  = _p.getBool("setup_ok", false);  // finished first-run setup
        _p.end();
    } }

    // First-run SD default: the very first time meshcomod boots on a brand-new
    // device — the user hasn't finished setup yet AND nothing is stored on
    // SPIFFS — prefer the SD card when one is present. Keeps internal flash free
    // and is Launcher-friendly. The "no SPIFFS data" guard is what makes this
    // safe: a device that already holds data on internal flash (e.g. one updated
    // from an earlier build) is never silently migrated onto an empty card.
    bool spiffs_has_data = spiffs_ok &&
        (SPIFFS.exists("/new_prefs") || SPIFFS.exists("/node_prefs") ||
         SPIFFS.exists("/identity/_main.id"));
    bool fresh_install = !use_sd_pref && !setup_done && !spiffs_has_data;

    bool want_sd = !spiffs_ok      // no usable SPIFFS partition -> must use SD
                || use_sd_pref     // user opted in
                || fresh_install;  // brand-new device: try SD first
    SPIClass* _spi = tdeckSharedSPI();
    if (want_sd && _spi) {
      for (int a = 0; a < 4 && !sd_storage; ++a) {   // short mount ladder (cold cards)
        SD.end();
        delay(a == 0 ? 40 : 220);
        if (SD.begin(PIN_SD_CS, *_spi, 4000000, "/sd", 3) && SD.cardType() != CARD_NONE) {
          sd_storage = store.useSdStorage();
        }
      }
      // On a genuine first run, persist the auto-pick so the "Store data on SD"
      // toggle reflects it and the choice sticks on every later boot.
      if (fresh_install && sd_storage && !use_sd_pref) {
        Preferences _p; if (_p.begin("touch", false)) { _p.putBool("use_sd", true); _p.end(); }
        Serial.println("[BOOT] first run + SD card present -> data defaults to SD");
      }
    }
  }
#endif
  if (!sd_storage && !spiffs_ok) SPIFFS.begin(true);   // last resort: format SPIFFS
  Serial.printf("[BOOT] storage: %s\n", sd_storage ? "SD /meshcomod" : "SPIFFS");
#if defined(ESP32_PLATFORM) && defined(HAS_TOUCH_UI)
  // Route touch settings + Wi-Fi creds to the active filesystem (SD when that's
  // the data store, else SPIFFS) instead of NVS. Old NVS values still load and
  // migrate on their next save, so this is a transparent in-place upgrade.
  #if defined(HAS_TDECK_GT911)
    SdNvsPrefs::useFile(sd_storage ? (fs::FS*)&SD : (fs::FS*)&SPIFFS,
                        sd_storage ? "/meshcomod" : "/prefs");
  #else
    SdNvsPrefs::useFile((fs::FS*)&SPIFFS, "/prefs");   // no SD on this board
  #endif
  // The boot wordmark already read a pref (UI rotation) BEFORE useFile switched
  // the backend, caching the settings blob from legacy NVS. Re-read it now so
  // file-saved values (theme accent, brightness, language, …) take effect this
  // boot — otherwise a theme change "reverts" on every restart.
  touchPrefsReload();
#endif
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
  Serial.println("[BOOT] mesh ok");

#if defined(WIFI_SSID) || defined(MULTI_TRANSPORT_COMPANION)
  wifiConfigBegin();
  Serial.println("[BOOT] wifiConfig ok");
#endif

#ifdef MULTI_TRANSPORT_COMPANION
  board.setInhibitSleep(true);
  serial_interface.begin(Serial, TCP_PORT, WS_PORT);
  Serial.println("[BOOT] serial_interface ok");
  serial_interface.setBroadcastResponses(true);  // RX log, channel messages, etc. go to all clients (USB + TCP + WS [+ BLE]), not only last sender
  /* Pick BLE vs WiFi at boot. The ESP32-S3 doesn't have enough internal heap
   * (esp_wifi_init needs ~50KB for DMA buffers) to run Bluedroid BLE +
   * LVGL/TFT + WiFi all at once — esp_wifi_init silently returns ESP_ERR_NO_MEM,
   * leaving WiFi.getMode() at WIFI_MODE_NULL. So we mutex them: if the user
   * has saved WiFi credentials AND the radio is enabled, skip BLE init and
   * use WiFi exclusively. Otherwise init BLE. Toggle by saving/clearing creds
   * + reboot (saveWifiCb auto-restarts). On the touch build the user can also
   * pick Wi-Fi with no creds yet (to scan/configure on-device) — wantsWifi()
   * returns true for that case so the radio comes up scannable. */
  bool want_wifi = wifiConfigWantsWifi();
  /* Wi-Fi + BLE now COEXIST (NimBLE host is light enough — the old Bluedroid
   * heap clash is gone). Bring Wi-Fi up FIRST: esp_wifi_init grabs a big
   * contiguous DMA block, so let it claim memory before BLE. (Association
   * happens later in loop(); this just inits the stack.) */
  if (want_wifi) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);   // let esp_wifi rejoin on its own after a beacon loss / AP reboot
                                   // (was false — the 10 s poll below was the ONLY recovery, and a
                                   // bare begin() on a wedged supplicant is a no-op -> never reconnected)
    WiFi.persistent(false);
    // NOTE: do NOT enable modem-sleep here. On a fresh, *unassociated* STA (the
    // setup wizard, no creds yet) DTIM modem-sleep naps the radio through the
    // scan dwell, so WiFi.scanNetworks() comes back empty ("no networks found").
    // It's enabled once we actually associate — see the GOT_IP handler below.
  }
#if defined(BLE_PIN_CODE)
  /* Always stash the BLE params so the toggle can bring BLE up live later, even
   * if we defer it now. Then co-init BLE if the user has it enabled AND there's
   * comfortable internal heap left after Wi-Fi — otherwise defer to Wi-Fi-only
   * this boot rather than risk an OOM at NimBLE init (recoverable via the live
   * toggle once memory frees). */
  // Defensive: force node_name NUL-terminated before it builds the BLE device
  // name. Under Launcher (degraded storage) it can load non-terminated, which
  // is what overran the BLE name buffer; the snprintf there now bounds the write,
  // and this bounds the read so the name is the first <=31 chars, not garbage.
  { NodePrefs* _np = the_mesh.getNodePrefs();
    _np->node_name[sizeof(_np->node_name) - 1] = '\0'; }
  serial_interface.prepareBle(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
  if (wifiConfigGetBleEnabled()) {
    const size_t BLE_COEXIST_MIN_FREE  = 50 * 1024;   // free heap after Wi-Fi to also start BLE
    const size_t BLE_COEXIST_MIN_BLOCK = 20 * 1024;   // largest contiguous block (NimBLE controller/host)
    const size_t freeh  = ESP.getFreeHeap();
    const size_t maxblk = ESP.getMaxAllocHeap();
    if (!want_wifi || (freeh >= BLE_COEXIST_MIN_FREE && maxblk >= BLE_COEXIST_MIN_BLOCK)) {
      serial_interface.beginBle(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
      Serial.printf("[boot] BLE co-init OK (wifi=%d free=%u maxblk=%u)\n", (int)want_wifi, (unsigned)freeh, (unsigned)maxblk);
    } else {
      Serial.printf("[boot] BLE deferred: low heap (free=%u maxblk=%u) — Wi-Fi only\n", (unsigned)freeh, (unsigned)maxblk);
    }
  }
#endif
#elif defined(WIFI_SSID)
  board.setInhibitSleep(true);   // prevent sleep when WiFi is active
  WiFi.setAutoReconnect(true);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
      if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
          WIFI_DEBUG_PRINTLN("WiFi disconnected. Flagging for reconnect...");
          wifi_needs_reconnect = true;
      } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
          WIFI_DEBUG_PRINTLN("WiFi connected successfully!");
          wifi_needs_reconnect = false;
      }
  });

  if (wifiConfigHasRuntime()) {
    char ssid[WIFI_CONFIG_SSID_MAX];
    char pwd[WIFI_CONFIG_PWD_MAX];
    wifiConfigGetSsid(ssid, sizeof(ssid));
    wifiConfigGetPwd(pwd, sizeof(pwd));
    WiFi.begin(ssid, pwd[0] ? pwd : nullptr);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PWD);
  }
  serial_interface.begin(TCP_PORT);
#elif defined(BLE_PIN_CODE)
  serial_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#elif defined(SERIAL_RX)
  companion_serial.setPins(SERIAL_RX, SERIAL_TX);
  companion_serial.begin(115200);
  serial_interface.begin(companion_serial);
#else
  serial_interface.begin(Serial);
#endif
  the_mesh.startInterface(serial_interface);
#else
  #error "need to define filesystem"
#endif

#if defined(ENV_INCLUDE_GPS) && (ENV_INCLUDE_GPS == 1)
  // GPS UART resilience (slow / never-acquires TTFF fix): the core opens Serial1 for the
  // NMEA GPS with Arduino's default 256-byte RX ring — only ~66 ms of slack at 38400 baud
  // (~270 ms at 9600). A single long LVGL/map frame stalls this loop past that, dropping
  // UART bytes and corrupting NMEA ephemeris subframes. Each corrupted subframe costs the
  // receiver ~30 s of re-acquisition, so a busy UI turns a ~1-minute fix into several
  // minutes — or, with frequent stalls, never. A larger ring absorbs the stalls. MUST
  // precede the core's Serial1.begin() inside sensors.begin(); setRxBufferSize is a no-op
  // once the UART is already running.
  Serial1.setRxBufferSize(4096);
#endif
  sensors.begin();

#ifdef DISPLAY_CLASS
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());  // still want to pass this in as dependency, as prefs might be moved
#endif

  board.onBootComplete();
}

void loop() {
  // Run UI first every iteration so splash can dismiss at 3s even if mesh/serial blocks later (was stuck on version screen when the_mesh.loop() ran before ui_task.loop()).
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
#ifdef MULTI_TRANSPORT_COMPANION
  static bool wifi_started = false;
  static uint32_t last_wifi_retry_ms = 0;
  static const uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
  static bool wifi_radio_prev = true;
  static bool wifi_radio_inited = false;
  /* BLE-vs-WiFi mutex (chosen at setup based on saved creds + radio_en pref):
   * if BLE was initialized, do NOT attempt to bring WiFi up here — esp_wifi_init
   * would fail with ESP_ERR_NO_MEM after Bluedroid grabbed the internal heap,
   * and the resulting OOM cascade freezes LVGL. Only run the WiFi state
   * machine if creds are saved AND the radio pref is on, mirroring `want_wifi`
   * in setup(). (Touch may also want Wi-Fi up with no creds, to scan.) */
  bool wifi_radio_en = wifiConfigWantsWifi();
  if (!wifi_radio_inited) {
    wifi_radio_inited = true;
    wifi_radio_prev = wifi_radio_en;
  } else if (wifi_radio_en != wifi_radio_prev) {
    wifi_radio_prev = wifi_radio_en;
    if (!wifi_radio_en) {
      WiFi.disconnect(true);
      delay(50);
      WiFi.mode(WIFI_OFF);
    }
    wifi_started = false;
  }
  /* UI may have changed SSID/PWD and asked for a re-apply. Trigger re-begin
   * by forcing wifi_started=false; on next iter the block below will WiFi.begin
   * with the freshly-saved credentials. Also handles toggling radio_en off
   * from the UI (the transition above already covered the on case). */
  if (wifiConfigConsumeApplyRequest()) {
    /* Only touch WiFi state if it was actually started this session. When
     * BLE is the active transport (no creds saved), WiFi was never inited
     * and calling WiFi.disconnect()/mode(WIFI_OFF) would trigger esp_wifi_init
     * under low heap → crash. Setting wifi_started=false here is harmless;
     * setup() will re-pick BLE-vs-WiFi on the auto-reboot from saveWifiCb. */
    if (wifi_started) {
      if (!wifi_radio_en) {
        WiFi.disconnect(true);
        delay(50);
        WiFi.mode(WIFI_OFF);
      } else {
        WiFi.disconnect(false, false);
        delay(50);
      }
    }
    wifi_started = false;
    last_wifi_retry_ms = 0;
  }
  if (wifi_radio_en) {
    if (!wifi_started) {
      wifi_started = true;
      WiFi.mode(WIFI_STA);
      if (wifiConfigHasRuntime()) {
        char ssid[WIFI_CONFIG_SSID_MAX];
        char pwd[WIFI_CONFIG_PWD_MAX];
        wifiConfigGetSsid(ssid, sizeof(ssid));
        wifiConfigGetPwd(pwd, sizeof(pwd));
        if (strlen(ssid) > 0) {
          WiFi.begin(ssid, pwd[0] ? pwd : nullptr);
          last_wifi_retry_ms = millis();
        }
      }
    }
    // Automatic WiFi recovery for TCP mode: retry connection periodically if link drops.
    if (wifiConfigHasRuntime() && WiFi.status() != WL_CONNECTED) {
      uint32_t now = millis();
      if ((uint32_t)(now - last_wifi_retry_ms) >= WIFI_RETRY_INTERVAL_MS) {
        last_wifi_retry_ms = now;
        char ssid[WIFI_CONFIG_SSID_MAX];
        char pwd[WIFI_CONFIG_PWD_MAX];
        wifiConfigGetSsid(ssid, sizeof(ssid));
        wifiConfigGetPwd(pwd, sizeof(pwd));
        if (strlen(ssid) > 0) {
          // A bare begin() on a supplicant wedged after a silent drop (AP reboot /
          // beacon loss) can be a no-op — clear its state first so this forces a
          // fresh association. Backs up setAutoReconnect(true) for the stuck case.
          WiFi.disconnect(false, true);
          WiFi.begin(ssid, pwd[0] ? pwd : nullptr);
        }
      }
    }
    /* SNTP: kick off when Wi-Fi associates; once system time syncs, push it
     * into the mesh RTC so timestamps on messages are accurate. */
    static bool sntp_kicked = false;
    static bool sntp_pushed = false;
    static uint32_t sntp_kick_ms = 0;
    if (WiFi.status() == WL_CONNECTED) {
      // Now that we're associated, enable DTIM modem-sleep (saves power + gives
      // BLE coexistence airtime). Deferred to here on purpose: enabling it on the
      // unassociated STA naps the radio through a scan dwell and breaks the setup
      // wizard's WiFi.scanNetworks() ("no networks found"). One-shot.
      static bool modem_sleep_set = false;
      if (!modem_sleep_set) { WiFi.setSleep(true); modem_sleep_set = true; }
      if (!sntp_kicked) {
        /* Brussels timezone with DST rules baked in (POSIX "CET-1CEST,...").
         * On touch builds the base is shifted by the user's manual hour offset
         * (Settings -> Device -> Time offset) so localtime() matches what they
         * set. configTzTime only affects localtime() display; the mesh RTC
         * still stores UTC seconds (protocol-facing). */
        char _tz[48];
#if defined(HAS_TOUCH_UI)
        touchPrefsBuildLocalTz(_tz, sizeof _tz);
#else
        strncpy(_tz, "CET-1CEST,M3.5.0,M10.5.0/3", sizeof _tz);
        _tz[sizeof _tz - 1] = '\0';
#endif
        configTzTime(_tz, "pool.ntp.org", "time.google.com");
        sntp_kicked = true;
        sntp_kick_ms = millis();
      } else if (!sntp_pushed && (uint32_t)(millis() - sntp_kick_ms) >= 1500) {
        time_t t = time(nullptr);
        if (t > 1700000000) {
          /* Mesh RTC stores UTC seconds (protocol-facing); display layer
           * converts to local via localtime_r() using the TZ from configTzTime. */
          rtc_clock.setCurrentTime((uint32_t)t);
          sntp_pushed = true;
        }
      }
    } else {
      // Link dropped: allow re-sync on next reconnect.
      if (sntp_kicked && !sntp_pushed) sntp_kicked = false;
    }
  }
  // Defer TCP and WebSocket until after splash dismisses so the_mesh.loop() never blocks on accept() before ui_task.loop() runs.
  static const uint32_t TCP_DEFER_MS = 5000;   // 5 s: don't start TCP/WS until version screen has dismissed
  /* Only start TCP / WS when WiFi was actually brought up. In BLE-only mode
   * (no saved creds) the lwIP stack is never initialized — calling
   * WiFiServer::begin() crashes with a tcpip_adapter assert. */
  if (millis() > TCP_DEFER_MS && wifi_started) {
    serial_interface.startTcpServer(WiFi.status() == WL_CONNECTED);
    serial_interface.tickWebSocketHandshake();
  }
#endif
  the_mesh.loop();
#if defined(GPS_BUF_DEBUG)
  // Bench diagnostic (build with -DGPS_BUF_DEBUG only; absent in releases): peak GPS UART
  // backlog accumulated between sensors.loop() drains. A peak above the old 256-byte default
  // proves loop stalls were overflowing the default ring — i.e. NMEA was being lost, which
  // is the slow/never-acquires TTFF mechanism. With the 4096 ring above it can climb past
  // 256 without loss, so a >256 reading is direct proof the fix matters on this unit.
  { static size_t s_gps_peak = 0; static uint32_t s_gps_log = 0;
    size_t bl = Serial1.available();
    if (bl > s_gps_peak) s_gps_peak = bl;
    if (millis() - s_gps_log > 5000) {
      s_gps_log = millis();
      Serial.printf("[GPSBUF] peak=%u B / 5s (old cap 256, now 4096)\n", (unsigned)s_gps_peak);
      s_gps_peak = 0;
    }
  }
#endif
  sensors.loop();
#if defined(ESP32)
  // GPS time guard (Ricky Leong's "stuck at 1902"): MicroNMEALocationProvider
  // sets the mesh RTC from a GPS *position* fix even before the date fields are
  // valid (getYear()==0 -> ~1902-10-11), and re-syncs every 30 min — so it
  // periodically clobbers a good time. A 1902 clock stamps our adverts as
  // decades old and every other node rejects them as stale.
  //
  // On the T-Deck the mesh RTC, NTP and GPS ALL share the one ESP32 system clock
  // (ESP32RTCClock == settimeofday/gettimeofday), so we can't recover by
  // "re-reading NTP" — it was already overwritten. Instead keep a millis()-
  // anchored copy of the last good time and rebuild from it whenever the clock
  // reads garbage, undoing the clobber before the next the_mesh.loop() sends an
  // advert. The anchor refreshes every loop while the clock is sane, so the
  // rebuilt time is accurate to the second.
  {
    static uint32_t good_epoch = 0, good_millis = 0;
    const uint32_t live = rtc_clock.getCurrentTime();
    if (live > 1700000000UL) {                 // clock sane -> remember it (anchor)
      good_epoch  = live;
      good_millis = millis();
    } else if (good_epoch != 0) {              // clock went bad -> rebuild from anchor
      const uint32_t rebuilt = good_epoch + (uint32_t)((millis() - good_millis) / 1000UL);
      rtc_clock.setCurrentTime(rebuilt);
    }
  }
#endif
  rtc_clock.tick();

  // (1.16) sleep when there's no pending work — nRF power saving
  if (!the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#endif
  }

  // (1.16) non-blocking WiFi auto-reconnect (event-flagged in setup). Touch /
  // multi-transport builds run their own WiFi reconnect state machine above and
  // don't include SerialWifiInterface's WIFI_DEBUG_PRINTLN, so skip it there.
#if defined(ESP32) && defined(WIFI_SSID) && !defined(MULTI_TRANSPORT_COMPANION)
  if (wifi_needs_reconnect && (millis() - last_wifi_reconnect_attempt > 10000)) {
    WIFI_DEBUG_PRINTLN("Attempting manual WiFi reconnect...");
    WiFi.disconnect();
    WiFi.reconnect();
    last_wifi_reconnect_attempt = millis();
  }
#endif

  // (fork) drive the in-firmware OTA staged-reboot
#if defined(ESP32_PLATFORM)
  board.pollHttpOtaReboot();
#endif

#if defined(HAS_TOUCH_UI)
  // Idle light-sleep gate: evaluated every loop tick. g_enabled is false by
  // default (Task 1 is inert); Task 2 sets it from the NVS pref and wires
  // the real predicates so the gate can actually pass and arm light sleep.
  touchSleep::loopEnd(millis());
#endif
}
