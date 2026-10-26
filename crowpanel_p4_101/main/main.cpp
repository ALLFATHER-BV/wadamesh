// wadamesh on the Elecrow CrowPanel Advanced 10.1" ESP32-P4 HMI (1024x600). EXPERIMENTAL.
//
// Standalone ESP-IDF app, modelled on tdisplay_p4/main/main.cpp (same P4 + C6 esp-hosted pairing,
// same storage law: hot store on the internal LittleFS 'storage' partition, the microSD for tiles and
// bulk). The board has no I/O expander, so bring-up is: on-chip LDO rails -> EK79007 panel -> radio
// -> storage -> UI, and the C6 last. Board globals + radio_init() live in
// variants/crowpanel_p4_101/target.cpp. Pin map: the table in variants/crowpanel_p4_101/target.h.
#include "../../variants/tdisplay_p4/tdisplay_p4_compat.h"   // adcAttachPin() shim, BEFORE target.h
#include <Arduino.h>
#include <Mesh.h>
#include "MyMesh.h"
#include <new>
#include "esp_heap_caps.h"
#include "UITask.h"
#include "target.h"
#include <LittleFS.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include "../../src/helpers/esp32/MultiTransportCompanionInterface.h"
#include <helpers/esp32/WifiRuntimeStore.h>
#include <helpers/esp32/WifiCrashGuard.h>   // Wi-Fi off after a crash right after starting it
#include "i18n.h"
#include <helpers/esp32/TouchPrefsStore.h>
#include "../../src/helpers/esp32/BootTimeSync.h"
#include "esp_hosted.h"
#include "esp_netif.h"
#include "esp_event.h"
#include <esp_sntp.h>                // completion status for the Wi-Fi time sync
#include "driver/gpio.h"
#include "lvgl.h"

#ifndef TCP_PORT
#define TCP_PORT 5000
#endif
#ifndef WS_PORT
#define WS_PORT 8765
#endif

#if defined(CONFIG_BT_NIMBLE_TRANSPORT_UART)
#error "CrowPanel P4 C6 requires NimBLE hosted HCI; disable CONFIG_BT_NIMBLE_TRANSPORT_UART"
#endif

extern "C" bool hostedInitBLE();   // arduino-esp32 BLE controller bring-up over esp-hosted
extern "C" bool hostedSetPins(int8_t clk, int8_t cmd, int8_t d0, int8_t d1, int8_t d2, int8_t d3, int8_t rst);

// C6 SDIO pin set. Arduino's hostedInit() does NOT take these from sdkconfig: it passes the generic
// esp32p4 variant's BOARD_SDIO_ESP_HOSTED_* (CLK18 CMD19 D0-D3 = 14,15,16,17, RST 54) to
// esp_hosted_sdio_set_config(), which overrides Kconfig. That order happens to match the T-Display
// P4 and the CrowPanel V1.0, but V1.1/V1.2 route D0..D3 to GPIO17..14, and C6_EN is GPIO32 on every
// revision. Must run before anything can start esp-hosted (Wi-Fi, BLE, boot time sync).
// The bus width (4-bit, or 1-bit on V1.0) still comes from the sdkconfig fragment.
// Say once, plainly, which esp-hosted firmware the C6 runs (known once the transport is up). A C6
// older than the host (V1.0 units have shipped with 2.3.0 against this 2.12.x host) can leave RPCs
// unanswered, each costing the 5 s RPC timeout on the calling task, so Wi-Fi reconnects are spaced
// out further on such a C6 rather than stalling the UI loop every 10 s.
static int s_c6_state = -1;   // -1 unknown, 0 matches the host, 1 older than the host
static void crowpanelC6ReportOnce(const char* when) {
  static bool s_done = false;
  if (s_done) return;
  char line[200];
  const int st = crowpanelC6Describe(line, sizeof line);
  if (st < 0) return;
  s_done = true;
  s_c6_state = st;
  printf("[C6] %s (seen at %s)\n", line, when);
  if (st == 1) printf("[C6] old co-processor: Wi-Fi reconnect attempts every 60 s instead of 10 s\n");
}

static void crowpanelC6Pins() {
#if CP_REV == 10
  const bool ok = hostedSetPins(18, 19, 14, 15, 16, 17, 32);
#else
  const bool ok = hostedSetPins(18, 19, 17, 16, 15, 14, 32);
#endif
  printf("[BOOT] C6 SDIO pins set for V%d.%d: %s\n", CP_REV / 10, CP_REV % 10, ok ? "ok" : "FAILED");
}

// Boot-trace hooks the shared code defines on the S3 boards (src/main.cpp, excluded here).
volatile int g_boot_phase = 0;
extern "C" void set_boot_phase(int phase) { g_boot_phase = phase; }

// App globals: src/main.cpp declares these on the S3 boards; this build owns them.
DataStore store(LittleFS, rtc_clock);
bool g_fs_ok = false;   // internal 'storage' (LittleFS) mounted
bool g_sd_ok = false;   // microSD mounted
bool g_sd_no_card_at_boot = false;   // boot probe: no card answered (UITask's first lazy mount skips)
MultiTransportCompanionInterface serial_interface;
StdRNG fast_rng;
SimpleMeshTables tables;
UITask ui_task(&board, &serial_interface);
static MyMesh& makeTheMesh() {
  void* mem = heap_caps_malloc(sizeof(MyMesh), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) mem = malloc(sizeof(MyMesh));
  return *new (mem) MyMesh(radio_driver, fast_rng, rtc_clock, tables, store, &ui_task);
}
MyMesh& the_mesh = makeTheMesh();

static void wadameshSetup() {
  Serial.begin(115200);
  delay(150);
  printf("[BOOT] wadamesh / CrowPanel P4 10.1 (board revision V%d.%d, EXPERIMENTAL)\n", CP_REV / 10, CP_REV % 10);

  // 1. On-chip LDO rails (DSI PHY + the 3.3 V pad domain of touch/LCD-reset/SD), backlight + amp off.
  crowpanelP4PowerOn();

  // 2. Shared I2C0 (GT911, codec, HY2.0 port) on 45/46.
  board.begin();

  // 3. EK79007 panel, before LVGL flushes to it.
  if (!display.begin()) printf("[BOOT] EK79007 display init FAILED\n");

  // 4. lwIP/tcpip + default event loop before any Wi-Fi/netif call (esp_wifi_remote needs it).
  esp_netif_init();
  esp_event_loop_create_default();
  crowpanelC6Pins();
  printf("[BOOT] C6 = esp-hosted (lazy Wi-Fi/BLE initialization)\n");

  // 5. Radio (SX1262 on J9/J11).
  if (!radio_init()) printf("[BOOT] radio_init FAILED\n");

  DisplayDriver* disp = &display;

  // 6. Storage: internal LittleFS 'storage' = the hot store; the microSD = tiles, backups, bulk.
  g_fs_ok = LittleFS.begin(true, "/lfs", 10, "storage");
  printf("[storage] LittleFS(storage) = %s\n", g_fs_ok ? "OK" : "FAILED");
  if (g_fs_ok) { LittleFS.mkdir("/identity"); LittleFS.mkdir("/bl"); }
  // microSD on SDMMC slot 0, 1-bit: the J5 slot only has D0 routed (CLK43 / CMD44 / D0 39), as in
  // Elecrow's Lesson08. Pad power is LDO4, which crowpanelP4PowerOn() already holds, so SD_MMC must
  // not try to claim it again (-1 = external power; the card itself is on VDD_3V3 with no switch).
  // The card has pull-ups on CMD/D0 (R27/R28); the build also enables the internal ones (build.sh
  // patch), as Elecrow does. DAT1-DAT3 are not connected to the P4 (schematic V1.0-V1.2), so the slot
  // can only run 1-bit. V1.0 also shows a 10k pull-down on the card's DAT3 (R33, with the pull-up
  // R29 marked "10K/NC"); a card that sees DAT3 low at its first command enters SPI mode and never
  // answers in SD mode, which would look exactly like an empty slot (unverified on hardware).
  SD_MMC.setPowerChannel(-1);
  SD_MMC.setPins(43, 44, 39);
  for (int p : {44, 39}) gpio_pullup_en((gpio_num_t)p);
  // Quiet presence check first: an empty slot (or a card that never answers the first command) is
  // reported once, instead of three IDF + Arduino error pairs from the mount ladder below. The
  // ladder only runs when a card answered, which is when a lower clock can actually help.
  const esp_err_t sd_probe = crowpanelSdProbe();
  if (sd_probe == ESP_ERR_TIMEOUT) {
    g_sd_no_card_at_boot = true;
    sdMountDiagBegin();
    sdMountDiagAttempt(400000u, false, false);
    sdMountDiagSetMounted(false, 0);
    printf("[storage] SD_MMC = no card or not responding (1-bit slot, no answer at 400 kHz)\n");
  } else {
    if (sd_probe != ESP_OK)
      printf("[storage] SD_MMC probe: card answered but init failed (%s); trying the mount ladder\n",
             esp_err_to_name(sd_probe));
    struct { int khz; const char* tag; } tries[] = {
      { SDMMC_FREQ_DEFAULT, "1-bit 20MHz" },
      { 10000,              "1-bit 10MHz (Elecrow)" },
      { 5000,               "1-bit 5MHz" },
    };
    sdMountDiagBegin();
    uint32_t mounted_hz = 0;
    for (auto& t : tries) {
      const bool begin_ok = SD_MMC.begin("/sdcard", true, false, t.khz);
      g_sd_ok = begin_ok && SD_MMC.cardType() != CARD_NONE;
      sdMountDiagAttempt((uint32_t)t.khz * 1000u, begin_ok, g_sd_ok);
      printf("[storage] SD_MMC try %s -> %s\n", t.tag, g_sd_ok ? "OK" : "fail");
      if (g_sd_ok) { mounted_hz = (uint32_t)t.khz * 1000u; break; }
      SD_MMC.end();
      delay(120);
    }
    sdMountDiagSetMounted(g_sd_ok, mounted_hz);
    printf("[storage] SD_MMC = %s\n", g_sd_ok ? "OK" : "card present but mount failed");
  }
  store.begin();   // internal LittleFS is the DataStore root (P4 storage law)

  the_mesh.begin(disp != NULL);

  wifiConfigBegin();   // NVS only, no radio
  wifi_crash_guard::boot();   // before anything can start Wi-Fi

  serial_interface.begin(Serial, TCP_PORT, WS_PORT);
  serial_interface.setBroadcastResponses(true);
  the_mesh.startInterface(serial_interface);

  sensors.begin();
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());
  board.onBootComplete();
  printf("[BOOT] setup done\n");

  // ---- C6 bring-up, AFTER the UI ----------------------------------------------------------------
  // A C6 that does not answer (wrong firmware, wrong SDIO pin set for the revision) must cost Wi-Fi
  // and BLE only: display, touch and LoRa are already up by now.
#if defined(BLE_PIN_CODE)
  char* nm = the_mesh.getNodePrefs()->node_name;
  serial_interface.prepareBle("wadamesh-", nm, the_mesh.getBLEPin());
  if (wifiConfigGetBleEnabled()) {
    if (hostedInitBLE()) serial_interface.beginBle("wadamesh-", nm, the_mesh.getBLEPin());
    else                 printf("[BOOT] hostedInitBLE FAILED (BLE unavailable; LoRa, touch and the UI keep running)\n");
    crowpanelC6ReportOnce("BLE start");
  } else {
    printf("[BOOT] BLE off in settings: C6 not started for it\n");
  }
#endif
  {
    uint32_t synced_epoch = 0;
    const BootTimeSyncResult r = bootTimeSyncRun(rtc_clock.timeIsCurrent(), synced_epoch);
    if (r == BootTimeSyncResult::Ok) {
      rtc_clock.setCurrentTime(synced_epoch);
      printf("[BOOT] cold-boot time sync ok: %lu\n", (unsigned long)synced_epoch);
    } else if (r != BootTimeSyncResult::Skipped) {
      printf("[BOOT] cold-boot time sync: %s\n", bootTimeSyncResultName(r));
    }
  }
}

extern "C" void app_main(void) {
  crowpanelP4InstallFullRestart();   // before anything else registers a shutdown handler
  initArduino();
  wadameshSetup();

  // Wi-Fi state machine + SNTP + TCP/WS companion server, as in tdisplay_p4/main/main.cpp
  // (esp-hosted backend: Arduino's real WiFi over esp_wifi_remote).
  bool     wifi_started = false, wifi_radio_prev = true, wifi_radio_inited = false;
  bool     sntp_kicked = false, sntp_pushed = false, modem_sleep_set = false;
  uint32_t last_wifi_retry_ms = 0;
  const uint32_t WIFI_RETRY_INTERVAL_MS = 10000;
  const uint32_t WIFI_RETRY_INTERVAL_OLD_C6_MS = 60000;   // see crowpanelC6ReportOnce()

  for (;;) {
    ui_task.loop();
    if (wifi_crash_guard::tick()) ui_task.showAlert(TR("Wi-Fi was switched off because the device crashed while it was on. Turn it on in Settings > Wi-Fi to try again."), 9000);

    bool wifi_radio_en = wifiConfigWantsWifi();
    if (!wifi_radio_inited) {
      wifi_radio_inited = true;
      wifi_radio_prev   = wifi_radio_en;
    } else if (wifi_radio_en != wifi_radio_prev) {
      wifi_radio_prev = wifi_radio_en;
      if (!wifi_radio_en) { WiFi.disconnect(true); delay(50); WiFi.mode(WIFI_OFF); wifi_crash_guard::stopped(); }
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
        wifi_crash_guard::started();
        WiFi.mode(WIFI_STA);
        if (wifiConfigHasRuntime()) {
          char ssid[WIFI_CONFIG_SSID_MAX], pwd[WIFI_CONFIG_PWD_MAX];
          wifiConfigGetSsid(ssid, sizeof(ssid)); wifiConfigGetPwd(pwd, sizeof(pwd));
          if (strlen(ssid) > 0) { WiFi.begin(ssid, pwd[0] ? pwd : nullptr); last_wifi_retry_ms = millis(); }
        }
      }
      if (wifiConfigHasRuntime() && WiFi.status() != WL_CONNECTED) {
        uint32_t now = millis();
        crowpanelC6ReportOnce("Wi-Fi start");
        const uint32_t retry_ms = (s_c6_state == 1) ? WIFI_RETRY_INTERVAL_OLD_C6_MS : WIFI_RETRY_INTERVAL_MS;
        if ((uint32_t)(now - last_wifi_retry_ms) >= retry_ms) {
          last_wifi_retry_ms = now;
          char ssid[WIFI_CONFIG_SSID_MAX], pwd[WIFI_CONFIG_PWD_MAX];
          wifiConfigGetSsid(ssid, sizeof(ssid)); wifiConfigGetPwd(pwd, sizeof(pwd));
          if (strlen(ssid) > 0) { WiFi.disconnect(false, true); WiFi.begin(ssid, pwd[0] ? pwd : nullptr); }
        }
      }
      if (WiFi.status() == WL_CONNECTED) {
        if (!modem_sleep_set) { WiFi.setSleep(true); modem_sleep_set = true; crowpanelC6ReportOnce("Wi-Fi connected"); }
        serial_interface.startTcpServer(true);
        if (!sntp_pushed) {
          // The user's zone, not configTime(0, 0, ...): that set TZ to UTC, so every displayed
          // time read in UTC from the first Wi-Fi join until the zone was picked again (#699).
          if (!sntp_kicked) {
            char tz[48]; touchPrefsBuildLocalTz(tz, sizeof tz);
            esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
            configTzTime(tz, "pool.ntp.org", "time.nist.gov");
            sntp_kicked = true;
          }
          // Only a COMPLETED sync is network time. Before that time() is whatever the clock
          // was seeded with at boot, and pushing it marked a stale clock as synced.
          const uint32_t e = (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED)
                             ? (uint32_t)time(nullptr) : 0;
          if (e > 1700000000) { rtc_clock.setCurrentTime(e); sntp_pushed = true; printf("[TIME] SNTP -> rtc %lu\n", (unsigned long)e); }
        }
      }
    }
    serial_interface.tickWebSocketHandshake();
    sensors.loop();
    the_mesh.loop();
    vTaskDelay(1);
  }
}
