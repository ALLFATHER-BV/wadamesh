#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"
#if defined(ESP32_PLATFORM)
  #include <new>               // placement-new for the PSRAM-resident the_mesh
  #include "esp_heap_caps.h"   // heap_caps_malloc(MALLOC_CAP_SPIRAM)
#endif
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
  #if defined(HAS_TDECK_GT911) || defined(HELTEC_LORA_V4_R8) || defined(TLORA_PAGER)
    #include <SD.h>
    #include <Preferences.h>
    #ifndef PIN_SD_CS
      #if defined(TLORA_PAGER)
        #define PIN_SD_CS PAGER_PIN_SD_CS
      #else
        #define PIN_SD_CS 39    // T-Deck microSD chip-select (V4-R8 sets 3 in the env)
      #endif
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
    #include "helpers/esp32/MqttBridge.h"
    #include <esp_heap_caps.h>
    #include <new>
    // ~9.2 KB of TCP/WS/USB framing buffers. Internal DRAM is the scarce pool on
    // the touch boards (Wi-Fi + BLE coexistence needs ~50 KB free), and none of
    // these buffers are touched from ISR context, so build the whole object in
    // PSRAM (heap is up before C++ static init on ESP32; falls back to internal
    // RAM if PSRAM is absent). In-TU init order runs this before
    // ui_task(&serial_interface) further down.
    static void* s_si_mem = [] {
      void* p = heap_caps_malloc(sizeof(MultiTransportCompanionInterface),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      return p ? p : malloc(sizeof(MultiTransportCompanionInterface));
    }();
    MultiTransportCompanionInterface& serial_interface =
        *new (s_si_mem) MultiTransportCompanionInterface();
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
#if defined(ESP32_PLATFORM)
// the_mesh is ~42 KB (dominated by the MAX_CONTACTS ContactInfo array) and was the
// single biggest static internal-DRAM consumer. Place the whole object in PSRAM —
// the contacts array rides along inside it — and bind a reference so every
// `the_mesh.foo()` call site is unchanged. The constructor still runs HERE at
// static-init (PSRAM is already up; the UITask psAlloc statics rely on the same),
// so timing/behaviour are identical to the old direct global — only the address
// moves off internal DRAM. heap_caps falls back to internal RAM if PSRAM is absent.
static MyMesh& makeTheMesh() {
  void* mem = heap_caps_malloc(sizeof(MyMesh), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) mem = malloc(sizeof(MyMesh));   // no-PSRAM fallback: behaves as before
  return *new (mem) MyMesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
  );
}
MyMesh& the_mesh = makeTheMesh();
#else
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
);
#endif

/* END GLOBAL OBJECTS */

#if defined(ESP32)
volatile int g_boot_phase = 0;
extern "C" void set_boot_phase(int phase) { g_boot_phase = phase; }
// True once contacts/channels are actually routed to the SD card at boot (SD mounted). The
// "Store data on SD" toggle is only a stored intent — if the card didn't mount at boot, contacts
// silently stay on internal flash. The Storage settings page reads this to show the REAL location.
bool g_contacts_on_sd = false;
// True only when DataStore adopted SD as the primary identity/preferences
// backend for this boot. The UI history selector uses the same decision so an
// established card identity can never be paired with another profile's
// internal chat history merely because an old card lacks a migration marker.
bool g_full_data_on_sd = false;
// True when full SD adoption was requested but the guarded SPIFFS -> SD copy
// could not be proven complete. Settings surfaces the recovery action; contacts
// may still use SD as the secondary store while identity/prefs remain internal.
bool g_sd_migration_blocked = false;
#endif


void halt() {
  while (1) ;
}

/* WIFI RECONNECT TRACKERS */
#if defined(ESP32) && defined(WIFI_SSID)
  bool wifi_needs_reconnect = false;
  unsigned long last_wifi_reconnect_attempt = 0;
#endif

#include "esp_task_wdt.h"   // task-watchdog reconfigure — see setup() (GH #56)

#if defined(HAS_TDECK_GT911) || defined(HELTEC_LORA_V4_R8) || defined(TLORA_PAGER)
// ---- SPIFFS -> SD migration (fixes the beta_36 "lost my profile" upgrades) ----
// Users who flipped "Store data on SD" before beta_36 ran with the toggle IGNORED
// (the flag never survived a reboot), so their identity/prefs/contacts kept living
// on SPIFFS. When beta_36 made the flag finally take effect, useSdStorage() pointed
// the store at an EMPTY card and the identity store generated a brand-new node:
// "lost profile settings". The data was never gone — it was orphaned on SPIFFS.
// Copies every SPIFFS file into SD:/meshcomod ("/prefs/<ns>.kv" flattens to
// "/meshcomod/<ns>.kv", matching SdNvsPrefs's SD layout). Returns true only when
// every file landed on the card; Pager additionally requires a last-written
// completion marker. The caller must NOT adopt the card otherwise. force=true
// (the Settings "Copy internal data to SD" recovery button) overwrites whatever
// the card holds on legacy targets. Pager recovery is deliberately non-clobbering;
// its boot and manual paths only fill missing files and commit the marker last.
// Both filesystems must be mounted by the caller.
#if defined(TLORA_PAGER)
static constexpr const char* kSdMigrationComplete = "/meshcomod/.spiffs-migration-v1";
static constexpr const char* kSdMigrationCompleteTmp = "/meshcomod/.spiffs-migration-v1.tmp";
bool meshcomodSdMigrationComplete() { return SD.exists(kSdMigrationComplete); }
#endif

bool meshcomodMigrateSpiffsToSd(bool force) {
  if (!SPIFFS.exists("/identity/_main.id")) return false;   // nothing worth adopting
  // On Pager the marker is the cross-file commit record. Remove it before an
  // explicit overwrite so a reset halfway through cannot leave the old marker
  // blessing mixed data.
  if (force) {
#if defined(TLORA_PAGER)
    SD.remove(kSdMigrationComplete);
    SD.remove(kSdMigrationCompleteTmp);
#endif
  }
  SD.mkdir("/meshcomod");
  SD.mkdir("/meshcomod/identity");
  SD.mkdir("/meshcomod/bl");
  SD.mkdir("/meshcomod/lock");
  SD.mkdir("/meshcomod/msgs");   // chat segments: SPIFFS names them flat ("/msgs/seg_*.bin"),
                                 // but the FAT card needs the real parent dir or every copy fails
  bool identity_ok = false;
  bool identity_deferred = false;
  int copied = 0, failed = 0;
  static uint8_t buf[4096];
  // This routine runs only on loopTask. Keep the working paths off its stack:
  // the manual recovery used to be called from a deep LVGL event callback and
  // field evidence showed this buffer being corrupted after dozens of files.
  static char src[96];
  static char dst[112];
  static char tmp[120];
  const UBaseType_t low_water = uxTaskGetStackHighWaterMark(nullptr);
  Serial.printf("[BOOT] SD migration start, loop stack low-water: %u bytes\n",
                (unsigned)(low_water * sizeof(StackType_t)));
  auto ensureSdParents = [](const char* path) -> bool {
    char parent[112];
    strlcpy(parent, path, sizeof(parent));
    for (char* slash = strchr(parent + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
      *slash = '\0';
      const bool ok = SD.exists(parent) || SD.mkdir(parent);
      *slash = '/';
      if (!ok) return false;
    }
    return true;
  };
  // Copy identity last. Its presence is the boot-time adoption key, so landing
  // it before a later history/settings failure could make a partial card look
  // complete if the NVS in-progress breadcrumb were ever lost. Existing card
  // identities are never replaced by the non-clobbering Pager path.
  auto copyFile = [&](File& source, const char* destination,
                      const char*& fail_stage) -> bool {
    snprintf(tmp, sizeof tmp, "%s.mig", destination);
    fail_stage = "parents";
    bool ok = ensureSdParents(destination);
    if (ok && SD.exists(tmp)) {
      fail_stage = "stale temp";
      ok = SD.remove(tmp);   // only our own prior migration residue
    }
    File d;
    if (ok) {
      fail_stage = "open temp";
      d = SD.open(tmp, FILE_WRITE);
      ok = (bool)d;
    }
    size_t since_feed = 0;
    const size_t source_size = source.size();
    size_t source_read = 0;
    while (ok && source_read < source_size) {
      fail_stage = "read source";
      const size_t remain = source_size - source_read;
      const size_t n = source.read(buf, remain < sizeof(buf) ? remain : sizeof(buf));
      if (n == 0) { ok = false; break; }
      fail_stage = "write temp";
      if (d.write(buf, n) != n) { ok = false; break; }
      source_read += n;
      // A large history file on a slow card can exceed the task-WDT window.
      since_feed += n;
      if (since_feed >= 32768) { esp_task_wdt_reset(); since_feed = 0; }
    }
    source.close();
    if (d) d.close();
    // Commit only a complete temporary file, preserving the old destination
    // until its replacement is ready.
    if (ok) {
      fail_stage = "replace destination";
      if (force && SD.exists(destination) && !SD.remove(destination)) ok = false;
      if (ok) {
        fail_stage = "commit rename";
        if (!SD.rename(tmp, destination)) ok = false;
      }
    }
    return ok;
  };
  File root = SPIFFS.open("/");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    Serial.println("[BOOT] SD migrate FAILED: open SPIFFS root");
    return false;
  }
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }
    // Arduino-ESP32 File::name() is only the basename (pathToFileName()). Use
    // path() so nested SPIFFS names retain identity/, prefs/, msgs/, etc. The
    // old name() assumption made us reopen "/_main.id" instead of
    // "/identity/_main.id", causing the first full-SD boot migration to fail.
    const char* sp = f.path();
    snprintf(src, sizeof src, "%s%s", sp[0] == '/' ? "" : "/", sp);
    if (strncmp(src, "/prefs/", 7) == 0)
      snprintf(dst, sizeof dst, "/meshcomod/%s", src + 7);   // kv files sit flat on the card
    else
      snprintf(dst, sizeof dst, "/meshcomod%s", src);
    if (!force && SD.exists(dst)) {
      if (strcmp(src, "/identity/_main.id") == 0) identity_ok = true;
      f.close();
      continue;   // boot path: never clobber
    }
    if (strcmp(src, "/identity/_main.id") == 0) {
      identity_deferred = true;
      f.close();
      continue;
    }
    const char* fail_stage = nullptr;
    const bool ok = copyFile(f, dst, fail_stage);
    if (ok) {
      ++copied;
    } else {
      ++failed;
      Serial.printf("[BOOT] SD migrate FAILED (%s): %s\n", fail_stage, src);
#if defined(TLORA_PAGER)
      // A failed FAT operation can mean the card/volume is wedged. Do not issue
      // remove(), exists(), or another copy against that same volume: the old
      // cleanup call was exactly where the first requested SD boot could hang.
      // The .mig name is never adopted and a later explicit retry removes it.
      break;
#else
      SD.remove(tmp);
#endif
    }
    esp_task_wdt_reset();
    yield();
  }
  root.close();
  if (failed != 0) {
    Serial.printf("[BOOT] SPIFFS -> SD migration stopped after %d copied, %d failed\n",
                  copied, failed);
    return false;   // breadcrumb remains armed; no more SD calls on this boot
  }
  if (identity_deferred) {
    strlcpy(src, "/identity/_main.id", sizeof(src));
    strlcpy(dst, "/meshcomod/identity/_main.id", sizeof(dst));
    File identity = SPIFFS.open(src, FILE_READ);
    const char* fail_stage = "open source";
    const bool identity_opened = (bool)identity;
    bool ok = identity_opened;
    if (ok) ok = copyFile(identity, dst, fail_stage);
    if (!ok) {
      if (identity) identity.close();
      ++failed;
      Serial.printf("[BOOT] SD migrate FAILED (%s): %s\n", fail_stage, src);
#if !defined(TLORA_PAGER)
      if (identity_opened) SD.remove(tmp);
#endif
      Serial.printf("[BOOT] SPIFFS -> SD migration stopped after %d copied, %d failed\n",
                    copied, failed);
      return false;
    }
    ++copied;
    identity_ok = true;
    esp_task_wdt_reset();
    yield();
  }
  // The boot path skips files the card already has — an identity already on the
  // card counts as "landed" (nothing needed migrating). Identity alone is not
  // enough, though: adopting after a larger history/prefs copy failed hides the
  // complete SPIFFS store behind a partial SD tree.
  Serial.printf("[BOOT] SPIFFS -> SD migration: %d copied, %d failed, identity %s\n",
                copied, failed, identity_ok ? "ok" : "MISSING");
  bool complete = identity_ok && failed == 0;
#if defined(TLORA_PAGER)
  if (complete) {
    if (SD.exists(kSdMigrationCompleteTmp) &&
        !SD.remove(kSdMigrationCompleteTmp)) {
      Serial.println("[BOOT] SD migrate FAILED: stale completion temp");
      return false;
    }
    File marker = SD.open(kSdMigrationCompleteTmp, FILE_WRITE);
    bool marker_ok = marker && marker.print("complete v1\n") == 12;
    if (marker) marker.close();
    if (marker_ok) {
      if (SD.exists(kSdMigrationComplete) &&
          !SD.remove(kSdMigrationComplete)) {
        marker_ok = false;
      } else {
        marker_ok = SD.rename(kSdMigrationCompleteTmp, kSdMigrationComplete);
      }
    }
    if (!marker_ok) {
      Serial.println("[BOOT] SD migrate FAILED: completion marker");
      // Treat marker failure like any other volume failure. Leaving the
      // marker temp is harmless; touching the failed card again can hang.
      return false;
    }
  }
#endif
  return complete;
}

bool meshcomodArmSdMigLatch() {
  Preferences prefs;
  if (!prefs.begin("touch", false)) return false;
  const bool armed = prefs.putBool("sd_mig_busy", true) == 1;
  prefs.end();
  return armed;
}

// Clear the boot safe-mode latch (see the SPIFFS->SD migration above): called after a
// successful manual "Copy internal data to SD" so a deliberate retry re-arms boot-time
// auto-adoption. The boot path re-latches on its own if a later migration wedges. GH #142/#148.
void meshcomodClearSdMigLatch() {
  Preferences _mp;
  if (_mp.begin("touch", false)) { _mp.remove("sd_mig_busy"); _mp.end(); }
  g_sd_migration_blocked = false;
}
#endif

void setup() {
  Serial.begin(115200);
#if defined(HAS_RAK_TAP_V2)
  delay(1500);  // USB-CDC enumeration before boot logs
#else
  delay(200);
#endif
  Serial.println("[BOOT] setup start");

  // Widen the task-watchdog grace period. The ~5 s default trips during a legitimate-but-slow flash
  // burst — a SPIFFS garbage-collect, or a bulk save (DataStore issues ~12 flash ops per contact,
  // thousands for a full address book). A flash op parks BOTH cores with the cache disabled, so both
  // IDLE tasks miss their reset and the watchdog panics (decoded coredumps: task_wdt CPU0=IDLE0
  // CPU1=IDLE1, core 0 in spi_flash_op_block_func). The burst can't be chunked under the limit easily
  // and the per-core WdtHeavyGuard only covers core 0, so give the dog enough headroom to ride the
  // burst out while still catching a genuine multi-second hang. (Fixes the random WDT reboots, GH #56.)
  esp_task_wdt_init(20, true);   // 20 s grace (was ~5 s), keep panic. Re-init reconfigures the
                                 // already-running TWDT + keeps the idle-task subscriptions.

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

#if defined(HAS_RAK_TAP_V2)
  // Quick PSRAM sanity check — silent crash before SPIFFS could be bad PSRAM config
  {
    void* p = heap_caps_malloc(64, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool psram_ok = (p != NULL);
    if (p) {
      memset(p, 0x55, 64);
      bool match = true;
      for (int i = 0; i < 64; ++i) match &= (((uint8_t*)p)[i] == 0x55);
      free(p);
      psram_ok = match;
    }
    Serial.printf("[BOOT] psram probe: %s\n", psram_ok ? "OK" : "FAIL"); Serial.flush();
    if (!psram_ok) { Serial.println("[BOOT] FATAL: PSRAM readback mismatch — halting"); halt(); }
  }
#endif

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
#if defined(HAS_RAK_TAP_V2)
      Serial.flush();
    }
    Serial.println("[BOOT] about to call initTxtTxUniquenessFromRng..."); Serial.flush();
    the_mesh.initTxtTxUniquenessFromRng();
    Serial.println("[BOOT] initTxtTxUniqueness done"); Serial.flush();
#else
    }
    the_mesh.initTxtTxUniquenessFromRng();
#endif
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
#if defined(HAS_TDECK_GT911) || defined(HELTEC_LORA_V4_R8) || defined(TLORA_PAGER)
  {
   #if defined(TLORA_PAGER)
    extern SPIClass* tloraPagerSharedSPI();    // display/radio/SD shared bus
   #elif defined(HELTEC_LORA_V4_R8)
    extern SPIClass* heltecV4R8SharedSPI();   // FSPI, shared with the TFT (CS=3)
   #else
    extern SPIClass* tdeckSharedSPI();        // LoRa SPI bus
   #endif
    bool setup_done = false;
    { Preferences _p; if (_p.begin("touch", true)) {
        setup_done = _p.getBool("setup_ok", false);  // finished first-run setup
        _p.end();
    } }
    const bool use_sd_pref = touchPrefsReadUseSdAtBoot();   // NVS + /prefs/touch.kv

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

    // Move the WHOLE store (identity/prefs/contacts) to SD:/meshcomod when the
    // device has no usable SPIFFS, the user opted in, or it's a brand-new device.
    bool want_full_sd = !spiffs_ok || use_sd_pref || fresh_install;

   #if defined(TLORA_PAGER)
    SPIClass* _spi = tloraPagerSharedSPI();
   #elif defined(HELTEC_LORA_V4_R8)
    SPIClass* _spi = heltecV4R8SharedSPI();
   #else
    SPIClass* _spi = tdeckSharedSPI();
    #endif
    bool sd_mounted = false;
#if defined(TLORA_PAGER)
    if (!_spi) {
      Serial.println("[BOOT] SD: shared SPI unavailable");
    } else if (!board.sdCardPresent()) {
      Serial.println("[BOOT] SD: no card detected");
    } else {
      // Match LilyGo's pager bring-up: the card shares the already-running
      // display/radio SPIClass and gets one conservative 4 MHz mount attempt.
      // Do not tear down that live shared bus or hide arbitration bugs behind
      // delays/retry ladders.
      Serial.println("[BOOT] SD: card detected; mounting at 4 MHz");
      const bool sd_begin_ok = SD.begin(PIN_SD_CS, *_spi, 4000000, "/sd", 6);
      sd_mounted = sd_begin_ok && SD.cardType() != CARD_NONE;
      if (!sd_mounted) {
        if (sd_begin_ok) {
          // Release only the unusable mount created above. SD.end() unregisters
          // this SD VFS; it does not stop the shared SPIClass used by TFT/radio.
          SD.end();
        }
        // Keep the board bring-up invariant explicit even if the SD library
        // changed this pin while unwinding a failed mount.
        pinMode(PIN_SD_CS, OUTPUT);
        digitalWrite(PIN_SD_CS, HIGH);
      }
      Serial.printf("[BOOT] SD mount: %s\n", sd_mounted ? "ok" : "failed");
    }
#else
    if (_spi) {
      // Try to mount the card on EVERY boot: even a device that keeps identity on SPIFFS
      // wants its churn-heavy contacts/channels on the card.
      //
      // Use the SAME forgiving ladder the map-tile mount (fmSdTryMount) uses — dropping to
      // 1 MHz then 400 kHz — not a fast 4 MHz-only ladder. A cold / cheap / slow-to-wake card
      // fails a 4 MHz-only mount here but later succeeds the slow tile mount, so contacts got
      // silently pushed back to SPIFFS *even though the toggle said SD and tiles worked* — then
      // SPIFFS churn eventually lost them (gubbinsgalore's "100 repeaters gone overnight").
      // Matching the tile ladder means contacts land on the card wherever tiles do. A card-less
      // device still bails fast (3 quick tries at 4 MHz, ~480 ms); only a present-but-cold card
      // walks down to the slow clocks. delay() feeds the task WDT, so the ~2.8 s worst case
      // (present cold card only) doesn't trip it.
      static const struct { uint16_t settle_ms; uint32_t hz; } kBootMount[] = {
        {  40, 4000000 }, { 220, 4000000 }, { 220, 4000000 },
        { 300, 1000000 }, { 450, 1000000 }, { 650,  400000 }, { 900, 400000 },
      };
      int tries = want_full_sd ? 7 : 3;
      uint32_t mounted_hz = 0;
      for (int a = 0; a < tries && !sd_mounted; ++a) {
        SD.end();
        delay(kBootMount[a].settle_ms);
        if (SD.begin(PIN_SD_CS, *_spi, kBootMount[a].hz, "/sd", 6) && SD.cardType() != CARD_NONE) {
          sd_mounted = true;
          mounted_hz = kBootMount[a].hz;
        }
      }
      // RENEGOTIATE UPWARD after a slow-rung success (GH #194). Standard SD bring-up is
      // "initialise at a conservative clock, then raise it" — but SD.begin's clock is the
      // operating clock for the whole session, so a card that only WOKE at 400 kHz then ran
      // its entire life at 400 kHz (~25 KB/s). With the card as the primary store that was a
      // ~3-minute boot (contacts + history at modem speed), runtime f_getfree timeouts (the
      // file manager showing an inserted card as 0 KB/empty), and wedged backups. An
      // initialised card almost always sustains 4 MHz even when its power-up handshake needed
      // 400 kHz; if the fast re-begin fails, fall back to the clock that just worked.
      if (sd_mounted && mounted_hz < 4000000) {
        SD.end();
        delay(60);
        if (SD.begin(PIN_SD_CS, *_spi, 4000000, "/sd", 6) && SD.cardType() != CARD_NONE) {
          Serial.printf("[BOOT] SD renegotiated %lu -> 4000000 Hz\n", (unsigned long)mounted_hz);
        } else {
          SD.end();
          delay(120);
          sd_mounted = SD.begin(PIN_SD_CS, *_spi, mounted_hz, "/sd", 6) && SD.cardType() != CARD_NONE;
          if (sd_mounted) Serial.printf("[BOOT] SD stays at %lu Hz (4 MHz renegotiation failed)\n", (unsigned long)mounted_hz);
        }
      }
    }
#endif
    if (sd_mounted) {
      g_contacts_on_sd = true;   // every branch below routes contacts/channels to the card
      if (want_full_sd) {
        // beta_36 upgrade heal: adopting the card while the LIVE data still sits
        // on SPIFFS (the pre-beta_36 "toggle ignored" state) must migrate FIRST,
        // or the identity store mints a fresh node on the empty card and the user
        // "loses" their profile. Only fires when SPIFFS holds an identity the
        // card lacks; a failed migration keeps the device on SPIFFS this boot
        // rather than adopting a card without the identity on it.
        bool adopt = true;
        if (SPIFFS.exists("/identity/_main.id")) {
          // Boot safe-mode (GH #142/#148): a wedged or corrupt SD card can hang / WDT-reboot the
          // device mid-migration, stranding it on the boot screen EVERY boot (reset can't escape,
          // only a downgrade could). Drop an NVS breadcrumb before migrating and clear it only if
          // the copy fully completes. If it's still set at the next boot, the last migration failed
          // -> skip it and boot from SPIFFS (the data is safe there); Settings > "Copy internal data
          // to SD" re-arms a deliberate retry. A merely-slow (healthy) card completes thanks to the
          // in-loop WDT feed, so it never latches here.
          Preferences _mp;
          const bool mp_ok = _mp.begin("touch", false);
          const bool mig_busy = mp_ok && _mp.getBool("sd_mig_busy", false);
#if defined(TLORA_PAGER)
          // An identity makes this an existing SD profile, even if it predates
          // the migration-complete marker. Overlaying its missing files from
          // SPIFFS would mix two profiles (for example, history from the card
          // with the internal channel table). Only migrate onto an empty card;
          // sd_mig_busy still rejects a migration interrupted after identity.
          const bool needs_migration =
              !SD.exists("/meshcomod/identity/_main.id");
#else
          const bool needs_migration =
              !SD.exists("/meshcomod/identity/_main.id");
#endif
          if (mig_busy) {
            if (mp_ok) _mp.end();
            adopt = false;
            g_sd_migration_blocked = true;
            Serial.println("[BOOT] prior SD migration did not complete -> skipping (staying on SPIFFS); retry via Settings > Copy internal data to SD");
          } else if (needs_migration) {
            // Do not start without a durable rollback breadcrumb. If NVS is
            // unavailable, a reset after identity lands would otherwise leave
            // no evidence that the SD tree is only partially migrated.
            const bool armed = mp_ok && _mp.putBool("sd_mig_busy", true) == 1;
            if (mp_ok) _mp.end();
            adopt = armed && meshcomodMigrateSpiffsToSd(false);
            // Keep the breadcrumb latched after a returned-but-incomplete copy.
            // That matters when identity landed before a larger history file
            // failed: the next boot must not adopt the partial SD tree merely
            // because the identity now exists. Manual Copy-to-SD clears it only
            // after a fully successful retry.
            if (adopt) {
              Preferences _mp2; if (_mp2.begin("touch", false)) { _mp2.remove("sd_mig_busy"); _mp2.end(); }
            }
            g_sd_migration_blocked = !adopt;
            if (!adopt) Serial.println(armed
                ? "[BOOT] SD migration incomplete -> staying on SPIFFS this boot"
                : "[BOOT] SD migration breadcrumb unavailable -> staying on SPIFFS this boot");
          } else {
            if (mp_ok) _mp.end();
          }
        }
        if (adopt) {
          sd_storage = store.useSdStorage();
          g_full_data_on_sd = sd_storage;
          // On a genuine first run, persist the auto-pick so the "Store data on SD"
          // toggle reflects it and the choice sticks on every later boot.
          if (fresh_install && sd_storage && !use_sd_pref) {
            Preferences _p; if (_p.begin("touch", false)) { _p.putBool("use_sd", true); _p.end(); }
            Serial.println("[BOOT] first run + SD card present -> data defaults to SD");
          }
        } else {
          store.setSecondaryFS(&SD);
          Serial.println("[BOOT] contacts/channels -> SD card (identity/prefs stay on SPIFFS)");
        }
      } else {
        // Upgraded device: identity + prefs stay on SPIFFS (no node-identity
        // change, safe if the card is later pulled), but route the frequently
        // rewritten contacts + channels to the card. On a near-full 3.375 MB
        // SPIFFS every 5-second saveContacts triggers a multi-second GC pass
        // that starves the loop task and trips the task watchdog (the beta_25
        // reboot loop). DataStore::begin() migrates the existing SPIFFS copies
        // to the card once, so the contact list is preserved.
        store.setSecondaryFS(&SD);
        Serial.println("[BOOT] contacts/channels -> SD card (identity/prefs stay on SPIFFS)");
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
  #if defined(HAS_TDECK_GT911) || defined(HELTEC_LORA_V4_R8) || defined(TLORA_PAGER)
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
#if defined(HAS_RAK_TAP_V2)
  Serial.println("[BOOT] prefs_backend ok"); Serial.flush();
  Serial.println("[BOOT] touchPrefsReload ok"); Serial.flush();
#endif
#endif
  store.begin();
#if defined(HAS_RAK_TAP_V2)
  Serial.println("[BOOT] store ok"); Serial.flush();
  Serial.println("[BOOT] calling mesh.begin..."); Serial.flush();
#endif
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
  Serial.println("[BOOT] mesh ok");
#if defined(HAS_RAK_TAP_V2)
  Serial.flush();
#endif

#if defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)
  {
    char nodeHex[13] = {};
    for (int i = 0; i < 6; ++i) snprintf(nodeHex + i * 2, 3, "%02x", the_mesh.self_id.pub_key[i]);
    mqtt_bridge.begin(nodeHex);
  }
#endif

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
  //
  // Gate on gps_enabled: sensors.begin()'s GPS-detect opens Serial1 on EVERY boot —
  // including the many V4s with no GPS module — and never calls Serial1.end(), so an
  // unconditional 4 KB ring permanently costs ~3.8 KB of scarce internal DRAM for nothing
  // on the GPS-off majority (the "RAM is higher now" reports). GPS-on users (who actually
  // hit the overflow) still get the big ring; default GPS-off keeps the stock 256 B. A user
  // who enables GPS mid-session picks it up on the next reboot (gps_enabled is persisted).
  {
#if defined(ATTAKY_MESH_SERIES)
    // This fixed stack always carries the GPS, so take the larger RX ring
    // unconditionally; the default 256 B ring gives the slowest first fix.
    Serial1.setRxBufferSize(4096);
#else
    auto* np = the_mesh.getNodePrefs();
    if (np && np->gps_enabled) Serial1.setRxBufferSize(4096);
#endif
  }
#endif
  sensors.begin();

#ifdef DISPLAY_CLASS
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());  // still want to pass this in as dependency, as prefs might be moved
  Serial.println("[BOOT] ui ready");
#endif

  board.onBootComplete();
}

void loop() {
  // Run UI first every iteration so splash can dismiss at 3s even if mesh/serial blocks later (was stuck on version screen when the_mesh.loop() ran before ui_task.loop()).
#ifdef DISPLAY_CLASS
  // ---- beta_31 field-freeze tracer (see UITask.cpp): time each loop section ----
  extern void stallLog(const char* tag, uint32_t dur_ms);
  extern const char* g_ui_stall_tag;
  extern uint16_t    g_ui_stall_max;
#define STALL_SCOPE(tag, call) { uint32_t _st0 = millis(); call; uint32_t _sdt = millis() - _st0; if (_sdt >= 200) stallLog(tag, _sdt); }
  { uint32_t _ui0 = millis();
    ui_task.loop();
    uint32_t _uidt = millis() - _ui0;
    if (_uidt >= 200) stallLog((g_ui_stall_max >= 150 && g_ui_stall_tag[0]) ? g_ui_stall_tag : "ui:other", _uidt);
  }
#endif
#ifdef MULTI_TRANSPORT_COMPANION
#ifdef DISPLAY_CLASS
  uint32_t _wf0 = millis();   // beta_31 tracer: time the whole Wi-Fi state machine + SNTP span
#endif
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
    // Suppressed while a scan runs on the worker: WiFi.disconnect()+begin() here would
    // abort the in-flight sweep (the scan-while-connected "0 networks" bug).
    if (wifiConfigHasRuntime() && WiFi.status() != WL_CONNECTED && !wifiScanIsActive()) {
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
#ifdef DISPLAY_CLASS
  { uint32_t _wfdt = millis() - _wf0; if (_wfdt >= 200) stallLog("wifi-sm", _wfdt); }
#endif
  // Defer TCP and WebSocket until after splash dismisses so the_mesh.loop() never blocks on accept() before ui_task.loop() runs.
  static const uint32_t TCP_DEFER_MS = 5000;   // 5 s: don't start TCP/WS until version screen has dismissed
  /* Only start TCP / WS when WiFi was actually brought up. In BLE-only mode
   * (no saved creds) the lwIP stack is never initialized — calling
   * WiFiServer::begin() crashes with a tcpip_adapter assert. */
  if (millis() > TCP_DEFER_MS && wifi_started) {
#ifdef DISPLAY_CLASS
    STALL_SCOPE("tcp-server", serial_interface.startTcpServer(WiFi.status() == WL_CONNECTED));
    STALL_SCOPE("ws-tick",    serial_interface.tickWebSocketHandshake());
#else
    serial_interface.startTcpServer(WiFi.status() == WL_CONNECTED);
    serial_interface.tickWebSocketHandshake();
#endif
  }
#endif
#if defined(HAS_TOUCH_UI)
  // The touch-UI "Spectrum" RF analyzer borrows the radio while open: it sweeps
  // the modem across the band, so the mesh must NOT touch the radio (re-tune /
  // re-arm RX on the home channel) meanwhile. spectrumOwnsRadio() is true only
  // while that app is up; the moment it closes it restores the mesh radio params
  // and clears the flag, so the next the_mesh.loop() re-arms RX correctly.
  if (!spectrumOwnsRadio())
#endif
#ifdef DISPLAY_CLASS
  STALL_SCOPE("mesh", the_mesh.loop());
#else
  the_mesh.loop();
#endif
#if defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)
#ifdef DISPLAY_CLASS
  STALL_SCOPE("mqtt", mqtt_bridge.loop());
#else
  mqtt_bridge.loop();
#endif
#endif
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
#ifdef DISPLAY_CLASS
  STALL_SCOPE("sensors", sensors.loop());
#else
  sensors.loop();
#endif
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
#ifdef DISPLAY_CLASS
  STALL_SCOPE("sleep-gate", touchSleep::loopEnd(millis()));
#else
  touchSleep::loopEnd(millis());
#endif
#endif
}
