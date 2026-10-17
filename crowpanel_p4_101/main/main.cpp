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
#include <helpers/esp32/TouchPrefsStore.h>
#include "../../src/helpers/esp32/BootTimeSync.h"
#include "esp_hosted.h"
#include "esp_netif.h"
#include "esp_event.h"
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
  // not try to claim it again (-1 = external power). The card has pull-ups on CMD/D0 (R27/R28); the
  // build also enables the internal ones (build.sh patch), as Elecrow does.
  SD_MMC.setPowerChannel(-1);
  SD_MMC.setPins(43, 44, 39);
  for (int p : {44, 39}) gpio_pullup_en((gpio_num_t)p);
  {
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
  }
  printf("[storage] SD_MMC = %s\n", g_sd_ok ? "OK" : "no card");
  store.begin();   // internal LittleFS is the DataStore root (P4 storage law)

  the_mesh.begin(disp != NULL);

  wifiConfigBegin();   // NVS only, no radio

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
    else                 printf("[BOOT] hostedInitBLE FAILED\n");
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

  for (;;) {
    ui_task.loop();

    bool wifi_radio_en = wifiConfigWantsWifi();
    if (!wifi_radio_inited) {
      wifi_radio_inited = true;
      wifi_radio_prev   = wifi_radio_en;
    } else if (wifi_radio_en != wifi_radio_prev) {
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
        if (!sntp_pushed) {
          if (!sntp_kicked) { configTime(0, 0, "pool.ntp.org", "time.nist.gov"); sntp_kicked = true; }
          const uint32_t e = (uint32_t)time(nullptr);
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
