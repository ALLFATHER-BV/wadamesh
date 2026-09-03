// wadamesh on the LilyGo T-Display P4 (AMOLED) — the app entry point.
//
// Standalone ESP-IDF app (NOT an AppFS/launcher app like the Tanmatsu): it owns the USB console,
// drives the XL9535 expander to power the board, brings up the RM69A10 AMOLED + the C6 (esp-hosted)
// + the raw SX1262, then runs the shared UITask. The board/radio/display globals + radio_init() live
// in variants/tdisplay_p4/target.cpp (via target.h). Modeled on tanmatsu/main/main.cpp; the WiFi +
// companion loop is ported faithfully. Bring-up TODOs (touch, brightness, DSI/power tuning) in
// variants/tdisplay_p4/TDISPLAY_P4_PORT.md.
#include <tdisplay_p4_compat.h>    // adcAttachPin() shim — BEFORE target.h pulls ESP32Board.h
#include <Arduino.h>
#include <Mesh.h>
#include "MyMesh.h"
#include <new>                     // placement-new for the PSRAM-resident the_mesh
#include "esp_heap_caps.h"
#include "UITask.h"
#include "target.h"                // board, radio_driver, rtc_clock, display, sensors, radio_init()
#include <LittleFS.h>              // internal 'storage' partition, mounted as LittleFS (label lookup is
                                   // subtype-agnostic, so the ex-FAT partition converts in place -- no
                                   // partition-table change, OTA-safe). The P4's FAT-on-flash layer has
                                   // documented broken metadata (exists/size/f_getfree lie) and v3 of the
                                   // #167 migration proved even its WRITES cannot be trusted; LittleFS is
                                   // the same battle-proven backend every S3 board uses internally.
#include <SD_MMC.h>                // microSD on SDMMC slot 0 (primary store)
#include "esp_partition.h"
#include <WiFi.h>
#include "../../src/helpers/esp32/MultiTransportCompanionInterface.h"
#include <helpers/esp32/WifiRuntimeStore.h>   // wifiConfig* (runtime Wi-Fi state the loop drives)
#include <helpers/esp32/TouchPrefsStore.h>    // touchPrefsBuildLocalTz + WIFI_CONFIG_* sizes
#include "esp_hosted.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "lvgl.h"
#include "c6_at.h"                 // AT-over-SDIO Wi-Fi/BLE driver for the on-board ESP32-C6 (P4-only)
#include <C6Socket.h>              // C6Client — used directly by httpDateProbe() below
#include "esp_vfs_fat.h"                    // native SD probe (esp_vfs_fat_sdmmc_mount)
#include "driver/sdmmc_host.h"              // SDMMC_HOST_DEFAULT / sdmmc_slot_config_t for the probe
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"     // SD IO rail = P4 on-chip LDO channel 4
// LAST include: rebind every WiFi.* below to the c6_at facade. Arduino's real WiFi object re-inits
// esp_hosted at mode()/begin() time and panics the P4 (the C6 runs ESP-AT, not an esp-hosted slave).
// With the facade, this file's Wi-Fi state machine drives REAL AT scans/joins instead.
#include <C6WifiShim.h>

#ifndef TCP_PORT
#define TCP_PORT 5000
#endif
#ifndef WS_PORT
#define WS_PORT 8765
#endif

// --- C6 / esp-hosted bring-up gate -----------------------------------------------------------------
// The ESP32-C6 (Wi-Fi/BLE co-processor over SDIO) doesn't come up yet: the SDIO bus inits, but the
// C6's esp-hosted slave never signals ready, so esp_hosted_connect_to_slave() times out (~12 s) and
// esp-hosted resets the P4 — a boot loop that never reaches ui_task.begin(), leaving the AMOLED dark.
// Until the C6 reset/power/slave-firmware path is proven (needs a reset callback via the XL9535 — see
// TDISPLAY_P4_PORT.md), gate ALL C6-dependent bring-up OFF so the display + touch + LoRa come up.
// LoRa is a raw SX1262 on P4 GPIOs, independent of the C6, so the mesh still works over USB/LoRa.
// Flip to 1 once the C6 link is solid to restore Wi-Fi + BLE.
#ifndef TDP4_C6_READY
// C6 reset+enabled via the Meck-P4 C6_EN pulse (Xl9535::powerOnSequence) + host esp-hosted pinned to
// 2.0.17 to match the factory C6 slave protocol (see idf_component.yml). SDIO card inits AND the RPC
// channel should now sync, so connect to the C6 for Wi-Fi + BLE. (If the link ever misframes again the
// P4 reset-loops — set this back to 0 and re-flash the safe build.)
#define TDP4_C6_READY 0   // C6 connect OFF: factory C6 firmware misframes esp-hosted even at host 2.0.17 (see notes)
#endif
// BLE over the C6 uses arduino-esp32's hostedInitBLE(), which only exists in arduino-esp32 >=3.3.10.
// We pin 3.3.0 (so esp_hosted can be the C6-compatible 2.0.17), which predates hostedInitBLE — so BLE
// needs its own bring-up. Keep BLE gated OFF until that's provided; Wi-Fi (esp_wifi_remote) works now.
#ifndef TDP4_BLE_READY
#define TDP4_BLE_READY 0
#endif

extern "C" bool hostedInitBLE();   // arduino-esp32 BLE controller bring-up over esp-hosted

// Boot-trace hooks the shared code defines on the S3 boards (src/main.cpp, excluded here).
volatile int g_boot_phase = 0;
extern "C" void set_boot_phase(int phase) { g_boot_phase = phase; }

// App globals — src/main.cpp declares these; we own them here (that file is excluded from the build).
DataStore store(LittleFS, rtc_clock);
bool g_fs_ok = false;   // internal 'storage' (LittleFS) mounted (extern — UITask file browser)
bool g_sd_ok = false;   // microSD mounted (extern)
MultiTransportCompanionInterface serial_interface;
StdRNG fast_rng;
SimpleMeshTables tables;
UITask ui_task(&board, &serial_interface);
// the_mesh (~42 KB, MAX_CONTACTS-dominated) lives in the 32 MB PSRAM, not scarce internal DRAM.
static MyMesh& makeTheMesh() {
  void* mem = heap_caps_malloc(sizeof(MyMesh), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) mem = malloc(sizeof(MyMesh));
  return *new (mem) MyMesh(radio_driver, fast_rng, rtc_clock, tables, store, &ui_task);
}
MyMesh& the_mesh = makeTheMesh();

static void hostedConnectC6() {
  esp_hosted_init();                         // no-op if the constructor already ran
  int rc = -1;
  for (int i = 0; i < 4 && rc != 0; i++) {
    rc = esp_hosted_connect_to_slave();
    if (rc != 0) { printf("[BOOT] C6 connect try %d -> %d\n", i, rc); delay(250); }
  }
  printf("[BOOT] C6 hosted link: %s\n", rc == 0 ? "UP" : "FAILED");
}

// Recursive FS→FS copy for the one-time SD adoption migration (FFat store → SD /meshcomod).
// Skips files that already exist at the destination, so a partial earlier run just completes.
static void migrateFsCopyFile(fs::FS &src, const char *sp, fs::FS &dst, const char *dp, bool overwrite = false) {
  if (!overwrite) {
    File probe = dst.open(dp, FILE_READ);
    if (probe) { probe.close(); return; }
  }
  File in = src.open(sp, FILE_READ);
  if (!in) return;
  File out = dst.open(dp, FILE_WRITE);
  if (!out) { in.close(); return; }
  static uint8_t buf[1024];
  int n;
  while ((n = in.read(buf, sizeof buf)) > 0) out.write(buf, (size_t)n);
  out.close(); in.close();
  printf("[storage]   migrated %s\n", sp);
}
static void migrateFsDir(fs::FS &src, const char *sdir, fs::FS &dst, const char *ddir, bool overwrite = false) {
  File d = src.open(sdir);
  if (!d || !d.isDirectory()) { if (d) d.close(); return; }
  dst.mkdir(ddir);
  File f;
  while ((f = d.openNextFile())) {
    const char *name = f.name();
    const char *leaf = strrchr(name, '/');
    leaf = leaf ? leaf + 1 : name;
    char sp[128], dp[128];
    snprintf(sp, sizeof sp, "%s/%s", strcmp(sdir, "/") == 0 ? "" : sdir, leaf);
    snprintf(dp, sizeof dp, "%s/%s", ddir, leaf);
    bool is_dir = f.isDirectory();
    f.close();
    if (is_dir) migrateFsDir(src, sp, dst, dp, overwrite);
    else        migrateFsCopyFile(src, sp, dst, dp, overwrite);
  }
  d.close();
}

static void wadameshSetup() {
  Serial.begin(115200);
  delay(150);
  printf("[BOOT] wadamesh / T-Display P4\n");

  // 1. XL9535 expander FIRST — it powers the rails and enables the C6, SD, screen + touch.
  if (!xl9535.begin(7, 8)) printf("[BOOT] XL9535 init FAILED (board will not power up)\n");
  xl9535.powerOnSequence();

  // 2. AMOLED (RM69A10 MIPI-DSI) — up before LVGL flushes to it.
  if (!display.begin()) printf("[BOOT] RM69A10 display init FAILED\n");

  // 3. lwIP/tcpip + default event loop before any WiFi/netif call (esp_wifi_remote needs it).
  esp_netif_init();
  esp_event_loop_create_default();

  // 4. C6: runs the FACTORY ESP-AT firmware (never esp-hosted, never reflashed — Kaj's product
  //    decision 2026-07-15). Wi-Fi comes up via the c6_at AT-over-SDIO worker spawned at the end of
  //    setup; BLE companion is unavailable on this AT build (advertising commands stubbed) so the
  //    phone pairs over Wi-Fi (TCP:5000) or USB.
#if TDP4_C6_READY
  hostedConnectC6();   // never taken (TDP4_C6_READY=0): kept only as a reference to the hosted path
#else
  printf("[BOOT] C6 = factory ESP-AT (Wi-Fi via c6_at worker; BLE companion unavailable on this build)\n");
#endif

  // 5. Radio (raw SX1262; reset via the XL9535 inside radio_init).
  if (!radio_init()) printf("[BOOT] radio_init FAILED\n");

  DisplayDriver* disp = &display;

  // 6. Storage — microSD (SDMMC slot 0) primary, internal FFat 'storage' as the no-card fallback.
  // formatOnFail=true: the first boot of this build finds FAT data in the partition, fails the
  // littlefs mount, and formats it — the deliberate one-way conversion (store contents come back
  // from the SD card via the v4 migration below).
  g_fs_ok = LittleFS.begin(true, "/lfs", 10, "storage");
  printf("[storage] LittleFS(storage) = %s\n", g_fs_ok ? "OK" : "FAILED");
  if (g_fs_ok) { LittleFS.mkdir("/identity"); LittleFS.mkdir("/bl"); }
  // The slot's VDD is gated by the XL9535's SD_EN (IO15), ACTIVE-LOW — powerOnSequence() drives it
  // low. (Root-caused 2026-07-15 with a GPIO pad sweep: SD_EN high = all six SD pads clamped LOW
  // through the unpowered card's ESD diodes -> 0x107/0x109 on every mount; SD_EN low = pads high,
  // card powered. Our Xl9535::begin() had parked it output-HIGH.) Card supply also wants the P4's
  // on-chip LDO4 attached like Meck does — Arduino's SD_MMC defaults to exactly that on this chip.
  // Pins are the board's SDMMC slot 0 IOMUX set (CLK43/CMD44/D0-3=39..42), 4-bit like Meck.
  SD_MMC.setPins(43, 44, 39, 40, 41, 42);
  // Meck's working init also sets SDMMC_SLOT_FLAG_INTERNAL_PULLUP (build.sh patches Arduino's slot-0
  // struct literal to add it). The powered board turns out to have external pull-ups on all six
  // lines too — belt and braces.
  for (int p : {44, 39, 40, 41, 42}) gpio_pullup_en((gpio_num_t)p);
  // Mount ladder: 4-bit@20 MHz normally succeeds first try now that the slot is powered; the
  // fallbacks stay for marginal cards. (1-bit skips D1..D3; 5 MHz derates signal margin.)
  //
  // #167: SD activity IS the whole-screen flash. Proven by elimination on-device: identical
  // firmware with the card removed never flashes, while every reported trigger (message arrives,
  // opening an unread chat, contact add/delete, boot pre-splash) ends in an SD write burst or the
  // mount probe. The display stack itself measured clean throughout (framebuffer content, bridge
  // underrun, host protocol errors, DCS traffic, expander writes -- all silent during visible
  // flashes; DSI config + panel init identical to the flash-free Meck-P4). Remaining coupling is
  // electrical: the 4-bit 20 MHz SDMMC bus and/or the card's write-current spikes on the shared
  // rail. TDP4_SD_KHZ / TDP4_SD_1BIT (build-time) tune the first mount rung to test/derate.
  {
#ifndef TDP4_SD_KHZ
#define TDP4_SD_KHZ SDMMC_FREQ_DEFAULT   // 20 MHz
#endif
#ifndef TDP4_SD_1BIT
#define TDP4_SD_1BIT false
#endif
    struct { bool onebit; int khz; const char *tag; } tries[] = {
      { TDP4_SD_1BIT, TDP4_SD_KHZ, "primary (TDP4_SD_KHZ)" },
      { false, 5000,               "4-bit 5MHz"  },
      { true,  SDMMC_FREQ_DEFAULT, "1-bit 20MHz" },
      { true,  5000,               "1-bit 5MHz"  },
    };
    for (auto &t : tries) {
      g_sd_ok = SD_MMC.begin("/sdcard", t.onebit, false, t.khz) && SD_MMC.cardType() != CARD_NONE;
      printf("[storage] SD_MMC try %s -> %s\n", t.tag, g_sd_ok ? "OK" : "fail");
      if (g_sd_ok) break;
      SD_MMC.end();
      delay(120);
    }
  }
  printf("[storage] SD_MMC = %s\n", g_sd_ok ? "OK" : "no card");
  if (g_sd_ok) {
    // #167 RESOLUTION -- the hot store lives on INTERNAL FFat; the SD carries only bulk, gentle
    // writers (map tiles, backups). Root cause, established by on-device elimination: SD-card
    // WRITE bursts electrically disturb the AMOLED (whole-screen blue flash). The display stack
    // measured clean throughout -- framebuffer content, DSI bridge underrun, DSI host protocol
    // errors, DCS traffic, expander writes: all silent during visible flashes -- and the flash
    // survived every digital derate (4->1-bit bus, 20->10 MHz SD clock, 1000->750 Mbps DSI lane
    // rate) on EVERY card tried, at any brightness, yet vanished the moment the card was removed
    // or left unwritten. The card's supply rail is shared with the panel (XL9535 SD_EN switches
    // it off the same 3V3 domain), so its program-current spikes reach the panel and firmware
    // cannot filter them -- it can only stop hammering the card. Streaming tile writes never
    // flashed, so the SD keeps those. This is also why the flash-free Meck build proves nothing
    // about the board: it keeps its store internal and never writes the SD at all.
    //
    // One-time REVERSE migration: earlier P4 builds kept the live store on SD /meshcomod, so if
    // the card carries an identity and this build hasn't migrated yet, copy the store back to
    // FFat, OVERWRITING FFat's stale pre-adoption copies (the SD is the current truth -- booting
    // cardless on the old FFat set showed long-deleted contacts). SD data is left in place,
    // untouched, as a rollback.
    // v4 migration -- one lesson per predecessor: v1 trusted whichever card was in and copied a
    // blank store; v2 copied the whole SD tree and filled the partition before contacts3 landed;
    // v3 did format+whitelist correctly and STILL read back empty -- because the P4's FAT-on-flash
    // driver itself is broken (its stat layer lying was already documented; v3 proved writes are
    // no better). v4 = the same clean-slate whitelist copy, onto LittleFS. Card is never written.
    File mk = LittleFS.open("/.hot_store_on_lfs_v4", FILE_READ);
    bool migrated = (bool)mk; if (mk) mk.close();
    if (!migrated) {
      File sp = SD_MMC.open("/meshcomod/identity/_main.id", FILE_READ);
      bool sd_has_id = (bool)sp; if (sp) sp.close();
      auto fsize = [](fs::FS &f, const char *path) -> size_t {
        File h = f.open(path, FILE_READ); size_t n = h ? h.size() : 0; if (h) h.close(); return n;
      };
      size_t sd_meshcomod = fsize(SD_MMC, "/meshcomod/contacts3");
      size_t sd_root      = fsize(SD_MMC, "/contacts3");
      size_t lf_contacts  = fsize(LittleFS, "/contacts3");
      size_t sd_contacts  = sd_meshcomod > sd_root ? sd_meshcomod : sd_root;
      if (sd_has_id && sd_contacts > lf_contacts) {
        printf("[storage] #167 v4: store SD -> LittleFS (contacts3 /meshcomod=%uB root=%uB, lfs had %uB)\n",
               (unsigned)sd_meshcomod, (unsigned)sd_root, (unsigned)lf_contacts);
        LittleFS.mkdir("/identity"); LittleFS.mkdir("/bl");
        const char *sub = sd_meshcomod >= sd_root ? "/meshcomod" : "";   // live layout root on the card
        char sp2[64];
        migrateFsDir(SD_MMC, "/meshcomod/identity", LittleFS, "/identity", true);
        migrateFsDir(SD_MMC, "/meshcomod/bl",       LittleFS, "/bl",       true);
        static const char *k_files[] = { "/contacts3", "/channels2", "/adv_blobs", "/new_prefs",
                                         "/new_prefs.tmp", "/regions2", "/ui_chat_history_v1.bin" };
        for (const char *nm : k_files) {
          snprintf(sp2, sizeof sp2, "%s%s", sub, nm);
          File probe = SD_MMC.open(sp2, FILE_READ); bool have = (bool)probe; if (probe) probe.close();
          if (!have && sub[0]) { snprintf(sp2, sizeof sp2, "%s", nm); }
          else if (!have)      { snprintf(sp2, sizeof sp2, "/meshcomod%s", nm); }
          migrateFsCopyFile(SD_MMC, sp2, LittleFS, nm, true);
        }
        printf("[storage] #167 v4: done -- lfs used %u / %u KB, contacts3 readback %uB\n",
               (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024),
               (unsigned)fsize(LittleFS, "/contacts3"));
        File w = LittleFS.open("/.hot_store_on_lfs_v4", FILE_WRITE);
        if (w) { w.write((const uint8_t*)"1", 1); w.close(); }
      } else {
        printf("[storage] #167 v4: no migration (sd_id=%d sd=%uB lfs=%uB) -- LittleFS is the store\n",
               (int)sd_has_id, (unsigned)sd_contacts, (unsigned)lf_contacts);
        File w = LittleFS.open("/.hot_store_on_lfs_v4", FILE_WRITE);
        if (w) { w.write((const uint8_t*)"1", 1); w.close(); }
      }
    }
#if defined(TDP4_SD_STORE_TEST)
    // #167 EXPERIMENT: store back on the SD card with all hot writes hopped to core 0 (the tile
    // task's core, which has never flashed). One variable vs the known-flashing config: the
    // executing core. Uses the card's existing (slightly stale) store; migration skipped.
    printf("[storage] TDP4_SD_STORE_TEST: store on SD, writes on core 0\n");
    store.useSdMmcStorage();
#else
    // Deliberately NOT calling store.useSdMmcStorage(): the internal store stays the DataStore root.
#endif
  }
  store.begin();

  the_mesh.begin(disp != NULL);

  serial_interface.begin(Serial, TCP_PORT, WS_PORT);
  serial_interface.setBroadcastResponses(true);
  the_mesh.startInterface(serial_interface);

#if defined(BLE_PIN_CODE) && TDP4_C6_READY && TDP4_BLE_READY
  if (hostedInitBLE()) {
    char* nm = the_mesh.getNodePrefs()->node_name;
    serial_interface.prepareBle("wadamesh-", nm, the_mesh.getBLEPin());
    if (wifiConfigGetBleEnabled())
      serial_interface.beginBle("wadamesh-", nm, the_mesh.getBLEPin());
  } else {
    printf("[BOOT] hostedInitBLE FAILED\n");
  }
#endif

  // GPS UART resilience (same fix as the S3 boards): the core opens Serial1 with Arduino's
  // 256-byte RX ring; one long LVGL frame overflows it and corrupts NMEA, stretching TTFF from
  // ~1 min to many. Must precede sensors.begin() (a no-op once the UART runs). Unconditional
  // here — the P4 has RAM to spare, and GPS detection itself needs an intact first second.
  Serial1.setRxBufferSize(4096);
  sensors.begin();
  {   // ---- GPS reality check -------------------------------------------------------------
    // The line that used to be here reported whether the "gps" SETTING got registered, which on
    // this board is FORCED true by ENV_SKIP_GPS_DETECT (we set that because the P4's heavy boot
    // misses the core's 1 s detect window, and a false negative hides the GPS toggle entirely).
    // So it printed DETECTED unconditionally and proved nothing, including across several rounds
    // of "GPS still isn't working". Read the UART directly instead and report what arrives.
    bool has_setting = false;
    for (int i = 0; i < sensors.getNumSettings(); i++)
      if (strcmp(sensors.getSettingName(i), "gps") == 0) has_setting = true;

    // Drain a SHORT window of NMEA. Bytes at all => pins, rails, wake and baud are ALL correct and
    // a stuck "acquiring" is a FIX problem (antenna / sky view). Zero bytes => the link is dead.
    // 1.2 s, not longer: this runs on every boot and it CONSUMES bytes the NMEA parser would
    // otherwise see, so it is deliberately just long enough to catch one of each sentence type
    // (the L76K repeats the full set about once a second).
    uint32_t bytes = 0, lines = 0;
    char first[96]; first[0] = '\0';
    char cur[96];   size_t cl = 0;
    // Keep one example of each distinct GSV/GGA/RMC talker so every constellation is represented
    // without dumping the whole stream (the same 5 sentence types repeat every second).
    char kept[10][96]; int kept_n = 0;
    const uint32_t until = millis() + 1200;
    while ((int32_t)(millis() - until) < 0) {
      while (Serial1.available() > 0) {
        const int c = Serial1.read();
        if (c < 0) break;
        ++bytes;
        if (c == '\n' || c == '\r') {
          if (cl) {
            cur[cl] = '\0';
            ++lines;
            if (!first[0] && cur[0] == '$') { strncpy(first, cur, sizeof first - 1); first[sizeof first - 1] = '\0'; }
            // Keep the first of each sentence TYPE (talker + type, e.g. "$GPGSV", "$GNGGA").
            if (cur[0] == '$' && cl >= 6 && kept_n < 10) {
              bool seen = false;
              for (int k = 0; k < kept_n; ++k) if (strncmp(kept[k], cur, 6) == 0) { seen = true; break; }
              if (!seen) { strncpy(kept[kept_n], cur, sizeof kept[0] - 1); kept[kept_n][sizeof kept[0] - 1] = '\0'; ++kept_n; }
            }
            cl = 0;
          }
        } else if (cl < sizeof(cur) - 1) {
          cur[cl++] = (char)c;
        }
      }
      delay(5);
    }
    printf("[GPS] setting_registered=%d (forced by ENV_SKIP_GPS_DETECT, proves nothing)\n", (int)has_setting);
    printf("[GPS] UART probe: %lu bytes, %lu lines in 1.2s @ %d baud, module TX -> GPIO%d\n",
           (unsigned long)bytes, (unsigned long)lines, (int)GPS_BAUD_RATE, (int)PIN_GPS_TX);
    if (first[0])        printf("[GPS] first sentence: %s\n", first);
    else if (bytes)      printf("[GPS] bytes but no complete '$' sentence -> wrong baud or line noise\n");
    else                 printf("[GPS] NOTHING on the UART -> check wake (XL9535 IO11 HIGH), rails, RX pin\n");
    // Satellite visibility across ALL constellations, plus the fix line. One GxGSV is not enough:
    // GLGSV is GLONASS only, so "00" there says nothing about GPS/Galileo/BeiDou. GSV field 3 is
    // sats-in-view for that constellation; GGA field 6 is fix quality (0 = no fix) and field 7 the
    // sats used. Non-zero in-view with quality 0 means it IS hearing satellites and just needs
    // time or better sky. All-zero in-view points at the antenna or shielding.
    if (kept_n) {
      printf("[GPS] --- satellite view (%d sentences) ---\n", kept_n);
      for (int i = 0; i < kept_n; ++i) printf("[GPS]   %s\n", kept[i]);
    }
  }
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());
  board.onBootComplete();
  printf("[BOOT] setup done\n");

#ifdef TDP4_C6_FLASH_HELPER
  // C6-FLASH HELPER BUILD: the P4 must NOT touch the C6 so it can be held in ROM download mode and
  // reflashed over its own USB. powerOnSequence() already skips the C6_EN reset pulse under this flag
  // (leaving C6_EN HIGH = powered), and here we skip the AT worker entirely. Boots the normal UI so
  // the board is alive, but the C6 is left completely free. Temporary — remove after the C6 reflash.
  printf("[BOOT] C6-FLASH HELPER: AT worker NOT started, C6 left free for download-mode reflash\n");
  board.onBootComplete();
  printf("[BOOT] setup done (helper)\n");
  return;
#endif
  // C6 AT/SDIO Wi-Fi: spawn the c6_at worker (non-blocking — it brings the AT link up itself on
  // core 1 with retries). The Wi-Fi state machine in app_main's loop then drives scans/joins through
  // the C6WifiShim facade exactly like the other boards drive Arduino WiFi.
  c6at_worker_start();
}

// Time-sync fallback: pull UTC from the Date header of a plain-HTTP HEAD to the firmware
// host. C6-SNTP needs UDP/123 egress, which some networks (guest/IoT VLANs) block — while
// port-80 HTTP is proven open on this device (tiles + version checks ride it). Returns a
// UTC epoch, or 0. Runs on the loop thread only while the clock is still unsynced.
static uint32_t httpDateProbe(void) {
  C6Client c;
  if (!c.connect("firmware.wadamesh.com", 80, 8000)) return 0;
  static const char req[] =
      "HEAD / HTTP/1.1\r\nHost: firmware.wadamesh.com\r\nConnection: close\r\n\r\n";
  if (c.write((const uint8_t*)req, sizeof(req) - 1) != sizeof(req) - 1) { c.stop(); return 0; }
  char buf[600];
  size_t got = 0;
  uint32_t t0 = millis();
  while (got < sizeof(buf) - 1 && millis() - t0 < 6000) {
    int n = c.read((uint8_t*)buf + got, sizeof(buf) - 1 - got);
    if (n > 0) { got += (size_t)n; buf[got] = '\0'; if (strstr(buf, "\r\n\r\n")) break; }
    else if (!c.connected()) break;
    else vTaskDelay(pdMS_TO_TICKS(20));
  }
  c.stop();
  buf[got] = '\0';
  const char* p = strstr(buf, "\r\nDate: ");           // "Date: Tue, 15 Jul 2026 16:33:47 GMT"
  if (!p) return 0;
  char mon[4] = {0};
  int day, year, hh, mm, ss;
  if (sscanf(p + 8, "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) return 0;
  if (year < 2024) return 0;
  static const char* M = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* mp = strstr(M, mon);
  if (!mp) return 0;
  int m = (int)(mp - M) / 3 + 1;
  int y = year - (m <= 2);                              // days-from-civil (Howard Hinnant)
  int era = y / 400, yoe = y - era * 400;
  int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + doe - 719468;
  return (uint32_t)(days * 86400L + hh * 3600 + mm * 60 + ss);
}

extern "C" void app_main(void) {
  initArduino();
  wadameshSetup();

  // WiFi state machine + SNTP + TCP/WS companion server — ported from src/main.cpp's loop()
  // (excluded from this build), identical to the Tanmatsu.
  bool     wifi_started = false, wifi_radio_prev = true, wifi_radio_inited = false;
  bool     sntp_kicked = false, sntp_pushed = false, modem_sleep_set = false;
  uint32_t last_wifi_retry_ms = 0, sntp_kick_ms = 0;
  const uint32_t WIFI_RETRY_INTERVAL_MS = 10000;

  for (;;) {
    ui_task.loop();

    // WiFi.* here is the C6WifiShim facade (AT-over-SDIO), so this state machine drives REAL joins:
    // start once the c6_at worker has the AT link up + the user wants Wi-Fi. The 10 s retry branch
    // doubles as auto-reconnect. (TDP4_C6_READY still gates the unused esp-hosted path elsewhere.)
    const bool c6_up = c6at_is_up();
    bool wifi_radio_en = c6_up && wifiConfigWantsWifi();
    // The C6 runs its own AT firmware with auto-connect enabled, so after a power
    // cycle it rejoins the last AP by itself, before this loop ever asks it to.
    // So latch only once the AT link is actually up, and APPLY the user's choice
    // at that moment rather than merely recording it: with the radio pref off
    // there is no on->off transition for the branch below to catch, the join the
    // C6 made on its own is never torn down, and the device comes up connected
    // while every switch in the UI reads off (#373).
    if (!wifi_radio_inited) {
      if (c6_up) {
        wifi_radio_inited = true;
        wifi_radio_prev   = wifi_radio_en;
        if (!wifi_radio_en) { WiFi.disconnect(true); delay(50); WiFi.mode(WIFI_OFF); }
      }
    }
    else if (wifi_radio_en != wifi_radio_prev) {
      wifi_radio_prev = wifi_radio_en;
      if (!wifi_radio_en) { WiFi.disconnect(true); delay(50); WiFi.mode(WIFI_OFF); }
      wifi_started = false;
    }
    if (wifiConfigConsumeApplyRequest()) {
      if (wifi_started) {
        if (!wifi_radio_en) { WiFi.disconnect(true); delay(50); WiFi.mode(WIFI_OFF); }
        else                { WiFi.disconnect(false, false); delay(50); }
      }
      wifi_started = false; last_wifi_retry_ms = 0;
    }
    if (wifi_radio_en) {
      if (!wifi_started) {
        wifi_started = true;
        WiFi.mode(WIFI_STA);
        if (wifiConfigHasRuntime()) {
          char ssid[WIFI_CONFIG_SSID_MAX], pwd[WIFI_CONFIG_PWD_MAX];
          wifiConfigGetSsid(ssid, sizeof(ssid)); wifiConfigGetPwd(pwd, sizeof(pwd));
          if (strlen(ssid) > 0) { WiFi.begin(ssid, pwd[0] ? pwd : nullptr); last_wifi_retry_ms = millis(); }
        }
      }
      if (wifiConfigHasRuntime() && WiFi.status() != WL_CONNECTED) {
        uint32_t now = millis();
        if ((uint32_t)(now - last_wifi_retry_ms) >= WIFI_RETRY_INTERVAL_MS) {
          last_wifi_retry_ms = now;
          char ssid[WIFI_CONFIG_SSID_MAX], pwd[WIFI_CONFIG_PWD_MAX];
          wifiConfigGetSsid(ssid, sizeof(ssid)); wifiConfigGetPwd(pwd, sizeof(pwd));
          if (strlen(ssid) > 0) { WiFi.disconnect(false, true); WiFi.begin(ssid, pwd[0] ? pwd : nullptr); }
        }
      }
      if (WiFi.status() == WL_CONNECTED) {
        if (!modem_sleep_set) { WiFi.setSleep(true); modem_sleep_set = true; }
        serial_interface.startTcpServer(true);
        // NTP runs ON the C6 (AT+CIPSNTP*, configured by the c6_at worker after a join) — lwIP has no
        // netif on this board, so esp_sntp/configTzTime can't sync. Pull the UTC epoch when ready.
        if (!sntp_pushed) {
          if (sntp_kick_ms == 0) sntp_kick_ms = millis();   // connected-since stamp for the fallback below
          uint32_t e = c6at_sntp_epoch();
          if (e > 1700000000) { rtc_clock.setCurrentTime(e); sntp_pushed = true; printf("[C6-AT] SNTP -> rtc %lu\n", (unsigned long)e); }
          // Bench-verified 2026-07-15: on some networks SNTP never completes (UDP/123
          // blocked) — the clock then stays wrong forever, since BLE is parked and the
          // phone app only sets time forward. After 45 s connected without a sync, fall
          // back to HTTP Date (sets the RTC through the same guarded ClockFloorRTC path,
          // so it also *repairs* a garbage-future hardware clock via the backward hatch).
          else if (millis() - sntp_kick_ms > 45000) {
            static uint32_t http_date_next = 0;
            if ((int32_t)(millis() - http_date_next) >= 0) {
              http_date_next = millis() + 120000;
              uint32_t he = httpDateProbe();
              if (he > 1700000000) {
                rtc_clock.setCurrentTime(he);
                sntp_pushed = true;
                printf("[TIME] HTTP-Date -> rtc %lu\n", (unsigned long)he);
              }
            }
          }
        }
        (void)sntp_kicked;
      }
    }
    serial_interface.tickWebSocketHandshake();

    // Service the sensor manager. This was MISSING on this board: sensors.begin() ran at boot and
    // then nothing ever pumped it again, while every S3 board calls this each iteration (see
    // src/main.cpp, which is excluded from this build — that is how the call got lost in the port).
    //
    // sensors.loop() is what drains the GPS UART into MicroNMEA. Without it the parser never sees a
    // byte, so isValid() stays false forever and the UI sits on "searching" no matter how good the
    // sky is — while the module itself is perfectly happy and does acquire a fix (proven with a raw
    // UART probe: a valid 3D fix at HDOP 2.2 that the firmware never knew about). It also starves
    // everything else the manager provides: telemetry sensors and the GPS->RTC time sync.
    sensors.loop();

    the_mesh.loop();
    vTaskDelay(1);
  }
}
