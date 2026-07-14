// wadamesh on the Tanmatsu (ESP32-P4) — the app entry point.
//
// Replaces the S3 boards' src/main.cpp setup()/loop(): it declares the app globals and runs the
// bring-up. badge-bsp owns the display/input/power; arduino-esp32 is the runtime; the LoRa is the
// TanmatsuLoraRadio bridge. The board/radio/display/sensors globals + radio_init() live in
// variants/tanmatsu/target.cpp (pulled via target.h).
#include <tanmatsu_compat.h>   // adcAttachPin() shim — BEFORE target.h pulls ESP32Board.h
#include <Arduino.h>
#include <Mesh.h>
#include "MyMesh.h"
#include <new>               // placement-new for the PSRAM-resident the_mesh
#include "esp_heap_caps.h"   // heap_caps_malloc(MALLOC_CAP_SPIRAM)
#include "UITask.h"
#include "target.h"            // board, radio_driver, rtc_clock, display, sensors, radio_init()
#include <SPIFFS.h>
#include <FFat.h>            // DataStore persists on the internal 'locfd' FAT partition
#include <SD_MMC.h>          // microSD (slot 0): reliable store for contacts/channels/chat (internal FFat loses them on this P4)
#include "esp_partition.h"   // enumerate partitions (AppFS apps boot fresh — verify locfd is visible)
#include <WiFi.h>
#include <helpers/esp32/MultiTransportCompanionInterface.h>
#include <helpers/esp32/WifiRuntimeStore.h>
#include <helpers/esp32/TouchPrefsStore.h>
#include "esp32-hal-hosted.h"   // arduino's esp-hosted bring-up (shared by LoRa + WiFi on the C6)
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"
extern "C" {
#include "bsp/device.h"
#include "bsp/input.h"
#include "bsp/display.h"
#include "bsp/tanmatsu.h"             // bsp_tanmatsu_coprocessor_get_handle (C6 radio power)
#include "tanmatsu_coprocessor.h"     // tanmatsu_coprocessor_radio_disable / _enable_application
#include "esp_hosted.h"       // C6 radio co-processor link (WiFi + LoRa ride esp-hosted)
#include "esp_netif.h"        // lwIP/tcpip bring-up (else WiFi calls assert "Invalid mbox")
#include "esp_event.h"
}

#ifndef TCP_PORT
#define TCP_PORT 5000
#endif
#ifndef WS_PORT
#define WS_PORT 8765
#endif

// Boot-trace hooks the shared code defines on the S3 boards (in src/main.cpp, which we exclude).
volatile int g_boot_phase = 0;
extern "C" void set_boot_phase(int phase) { g_boot_phase = phase; }
void halt() { while (1) { delay(1000); } }

// App globals — src/main.cpp declares these; we own them here (that file is excluded from the build).
// Persistence: an AppFS app boots FRESH (its own P4 reset) and does NOT inherit the launcher's VFS
// mounts, so we mount the internal 'locfd' FAT partition ourselves via FFat (an fs::FS, which is what
// DataStore takes — FILESYSTEM == fs::FS on ESP32). See the storage block in setup().
DataStore store(FFat, rtc_clock);
bool g_fs_ok = false;   // true once FFat(locfd) is mounted; UITask's file browser checks this (extern)
bool g_sd_ok = false;   // true once the microSD (SD_MMC) is mounted; contacts/channels/chat persist there (extern)
MultiTransportCompanionInterface serial_interface;
StdRNG fast_rng;
SimpleMeshTables tables;
UITask ui_task(&board, &serial_interface);
// the_mesh (~42 KB, dominated by the MAX_CONTACTS array) lives in PSRAM instead of the
// scarce internal DRAM — the P4 has 32 MB PSRAM. Placement-new into a heap_caps_malloc
// block + a reference, exactly like src/main.cpp on the S3 boards: the ctor still runs
// HERE at static-init (PSRAM is up first), so only the address moves. Internal-RAM
// fallback if PSRAM is somehow absent. MyMesh.h declares `extern MyMesh& the_mesh`.
static MyMesh& makeTheMesh() {
  void* mem = heap_caps_malloc(sizeof(MyMesh), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) mem = malloc(sizeof(MyMesh));
  return *new (mem) MyMesh(radio_driver, fast_rng, rtc_clock, tables, store, &ui_task);
}
MyMesh& the_mesh = makeTheMesh();

// ---------------------------------------------------------------------------
// Boot splash — a tiny on-screen console so launching the app shows progress
// instead of a black screen (the other badge.team apps print boot lines before
// their UI comes up; ours stayed black during the long pre-LVGL bring-up:
// radio + mesh + the 28k-line UI build). We draw straight to the panel with an
// 8x8 font. The panel is native 480x800 PORTRAIT and LVGL software-rotates the
// UI to 800x480 landscape (ROT_270); we apply the SAME mapping so the boot text
// is upright in landscape, then the first LVGL frame paints over it.
// ---------------------------------------------------------------------------
#define BOOT_LW 800   // logical (landscape) width
#define BOOT_LH 480   // logical (landscape) height
#define BOOT_PW 480   // physical panel width  (stride)
#define BOOT_PH 800   // physical panel height
#define BOOT_FG 0x07E0 // terminal green — pure G channel, immune to R/B order; only a full byte-swap limes it

// font8x8_basic (public domain, Daniel Hepper) — printable ASCII 0x20..0x7E.
// One glyph = 8 rows; bit 0 (LSB) is the leftmost pixel, row 0 the top.
static const uint8_t kBootFont[95][8] = {
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},{0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
  {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},{0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
  {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},{0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
  {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},{0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
  {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},{0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
  {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},{0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},{0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},{0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
  {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},{0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
  {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},{0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
  {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},{0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
  {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},{0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
  {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},{0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
  {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},{0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
  {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},{0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
  {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},{0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
  {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},{0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
  {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},{0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
  {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},{0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
  {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},{0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
  {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},{0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
  {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},{0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
  {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},{0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
  {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},{0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
  {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},{0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
  {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},{0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
  {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},{0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
  {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
  {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},{0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
  {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},{0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
  {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},{0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
  {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
  {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
  {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},{0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
  {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00},{0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},
  {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00},{0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
  {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},{0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
  {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},{0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
  {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},{0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
  {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},{0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
  {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},{0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
  {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},{0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
  {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},{0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
  {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},{0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
  {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},{0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
  {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},{0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
  {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},{0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
  {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
};

static uint16_t* s_boot_fb = nullptr;   // 480x800 RGB565 portrait framebuffer (PSRAM)
static int       s_boot_y  = 0;         // next text row, in landscape coords
static char      s_boot_cur[48] = {0};  // the in-progress line, so the animator can append dots to it
static int       s_boot_cur_y   = 0;
static volatile bool     s_boot_anim_run = false;
static SemaphoreHandle_t s_boot_lock     = nullptr;   // guards s_boot_fb + the panel between threads

// Map one logical (landscape) pixel into the portrait panel buffer. The on-device
// result showed the ROT_270 formula was 180° off, so use its opposite (ROT_90):
// phys_x = (PW-1) - ly, phys_y = lx.
static inline void bootPx(int lx, int ly, uint16_t c) {
  if ((unsigned)lx >= BOOT_LW || (unsigned)ly >= BOOT_LH) return;
  s_boot_fb[lx * BOOT_PW + (BOOT_PW - 1 - ly)] = c;
}

static void bootDrawText(int x, int y, const char* msg, uint16_t col, int scale) {
  for (const char* p = msg; *p; p++) {
    char ch = *p;
    if (ch < 0x20 || ch > 0x7E) ch = ' ';
    const uint8_t* g = kBootFont[ch - 0x20];
    for (int gy = 0; gy < 8; gy++)
      for (int gx = 0; gx < 8; gx++)
        if (g[gy] & (1 << gx))
          for (int sy = 0; sy < scale; sy++)
            for (int sx = 0; sx < scale; sx++)
              bootPx(x + gx * scale + sx, y + gy * scale + sy, col);
    x += 8 * scale;
    if (x + 8 * scale > BOOT_LW) break;
  }
}

// Push the whole framebuffer to the panel in horizontal bands. A single full-frame
// (480x800) blit only showed partially on-device — the proven LVGL flush path always
// writes sub-full bands, so do the same here.
static void bootBlit() {
  if (!s_boot_fb) return;
  const int BAND = 40;   // match the proven LVGL flush height (draw buf is 800x24 px -> <=480x40 portrait)
  for (int y = 0; y < BOOT_PH; y += BAND) {
    int h = (y + BAND > BOOT_PH) ? (BOOT_PH - y) : BAND;
    bsp_display_blit(0, y, BOOT_PW, y + h, s_boot_fb + (size_t)y * BOOT_PW);
  }
}

// Redraw the trailing "..." on the in-progress line with `ndots` dots, then blit only the few
// rows those dots occupy. Called from the animator (and to finalize a line at 3 dots).
static void bootDrawDots(int ndots) {
  if (!s_boot_fb || !s_boot_cur[0]) return;
  const int S = 2;
  const int dotstart = 16 + (int)strlen(s_boot_cur) * 8 * S;   // lx just past the text
  const int zonew    = 4 * 8 * S;                              // room for up to 4 dots
  for (int lx = dotstart; lx < dotstart + zonew && lx < BOOT_LW; lx++)   // clear the dot zone
    for (int ly = s_boot_cur_y; ly < s_boot_cur_y + 8 * S; ly++)
      bootPx(lx, ly, 0x0000);
  if (ndots > 4) ndots = 4;
  char d[8]; int j = 0; for (int k = 0; k < ndots; k++) d[j++] = '.'; d[j] = 0;
  if (ndots > 0) bootDrawText(dotstart, s_boot_cur_y, d, BOOT_FG, S);
  int r0 = dotstart - (dotstart % 40), r1 = dotstart + zonew;   // lx maps to fb rows (py)
  if (r1 > BOOT_PH) r1 = BOOT_PH;
  for (int y = r0; y < r1; y += 40) {
    int h = (y + 40 > BOOT_PH) ? (BOOT_PH - y) : 40;
    bsp_display_blit(0, y, BOOT_PW, y + h, s_boot_fb + (size_t)y * BOOT_PW);
  }
}

// Cycles ".", "..", "..." on the current line so a long phase (e.g. the slow C6 radio bring-up)
// shows activity instead of looking frozen. Runs on core 1 while the boot thread blocks on core 0.
static void bootAnimTask(void*) {
  int phase = 0;
  while (s_boot_anim_run) {
    if (s_boot_lock && xSemaphoreTake(s_boot_lock, pdMS_TO_TICKS(60)) == pdTRUE) {
      if (s_boot_anim_run && s_boot_fb) bootDrawDots((phase % 3) + 1);
      xSemaphoreGive(s_boot_lock);
    }
    phase++;
    vTaskDelay(pdMS_TO_TICKS(280));
  }
  vTaskDelete(NULL);
}

static void bootSplashBegin() {
  s_boot_fb = (uint16_t*)heap_caps_malloc(BOOT_PW * BOOT_PH * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_boot_fb) return;
  size_t hh = 0, vv = 0; bsp_display_color_format_t cf = (bsp_display_color_format_t)0;
  bsp_display_endianness_t en = (bsp_display_endianness_t)0;
  bsp_display_get_parameters(&hh, &vv, &cf, &en);
  printf("[BOOT] panel %ux%u fmt=%d endian=%d\n", (unsigned)hh, (unsigned)vv, (int)cf, (int)en);
  for (int i = 0; i < BOOT_PW * BOOT_PH; i++) s_boot_fb[i] = 0x0000;   // black
  bootDrawText(16, 28, "WADAMESH",     BOOT_FG, 3);                     // caps wordmark
  bootDrawText(16, 64, "============", BOOT_FG, 2);                     // terminal rule under it
  s_boot_y = 100;
  bootBlit();
  s_boot_lock = xSemaphoreCreateMutex();
  s_boot_anim_run = true;
  xTaskCreatePinnedToCore(bootAnimTask, "bootanim", 3072, NULL, 1, NULL, 1);
}

// Start a new boot-progress line ("> MSG"), finalizing the previous one at a steady "...". The
// animator then cycles dots on this line until the next bootLog().
static void bootLog(const char* msg) {
  Serial.printf("[BOOT] %s\n", msg);
  printf("[BOOT] %s\n", msg);            // Serial is swallowed on the IDF console
  if (!s_boot_fb) return;
  if (s_boot_lock) xSemaphoreTake(s_boot_lock, portMAX_DELAY);
  bootDrawDots(3);                       // freeze the previous line at "..."
  char line[48];
  int j = 0; line[j++] = '>'; line[j++] = ' ';
  for (const char* p = msg; *p && j < (int)sizeof(line) - 1; p++) {
    char c = *p; if (c >= 'a' && c <= 'z') c -= 32;   // uppercase for the boot-ROM look
    line[j++] = c;
  }
  line[j] = 0;
  const int S = 2, LH = 8 * S + 6;
  strncpy(s_boot_cur, line, sizeof(s_boot_cur) - 1); s_boot_cur[sizeof(s_boot_cur) - 1] = 0;
  s_boot_cur_y = s_boot_y;
  bootDrawText(16, s_boot_cur_y, line, BOOT_FG, S);
  s_boot_y += LH;
  if (s_boot_y + LH > BOOT_LH) s_boot_y = 100;
  bootBlit();
  if (s_boot_lock) xSemaphoreGive(s_boot_lock);
}

static void bootSplashEnd() {
  s_boot_anim_run = false;
  vTaskDelay(pdMS_TO_TICKS(320));        // let the animator observe the flag + self-delete
  if (s_boot_lock) xSemaphoreTake(s_boot_lock, portMAX_DELAY);
  if (s_boot_fb) {
    // Clean black handoff so no green text lingers; LVGL is then forced to repaint (see app setup).
    for (int i = 0; i < BOOT_PW * BOOT_PH; i++) s_boot_fb[i] = 0x0000;
    bootBlit();
    heap_caps_free(s_boot_fb);
    s_boot_fb = nullptr;
  }
  if (s_boot_lock) xSemaphoreGive(s_boot_lock);
}

// Bring up the esp-hosted SDIO link to the ESP32-C6 radio co-processor that BOTH LoRa and WiFi ride.
//
// esp_hosted is force-initialized before app_main by an unconditional __attribute__((constructor))
// in esp_hosted (port_esp_hosted_host_init.c) — there's no Kconfig to disable it. That pre-init is
// what made Arduino WiFi impossible: arduino's WiFi.mode -> hostedInit() calls esp_hosted_sdio_set_config(),
// which returns ESP_ERR_NOT_ALLOWED once esp_hosted is already configured, and this arduino build treats
// that as fatal -> esp_wifi_init() never runs (WiFi stuck at WIFI_NOT_INIT). And nobody ever called
// connect_to_slave(), so LoRa timed out too.
//
// LoRa fix: call connect_to_slave() (esp_hosted is already inited by the constructor) — that
// identifies the C6 slave so the LoRa custom-RPC channel works. WiFi needs more: arduino's WiFi.mode
// init bails because esp_hosted is pre-inited (esp_hosted_sdio_set_config -> ESP_ERR_NOT_ALLOWED).
// The code-only deinit workaround crashes (esp_hosted_deinit heap bug), so WiFi stays blocked pending
// a one-line arduino-core patch (esp32-hal-hosted.c:326 tolerate NOT_ALLOWED).
static void hostedConnectC6() {
  esp_hosted_init();                         // no-op (constructor already did it)
  int rc = -1;
  for (int i = 0; i < 4 && rc != 0; i++) {
    rc = esp_hosted_connect_to_slave();
    if (rc != 0) { Serial.printf("[BOOT] C6 connect try %d -> %d\n", i, rc); delay(250); }
  }
  Serial.printf("[BOOT] C6 hosted link: %s\n", rc == 0 ? "UP" : "FAILED");
}

static void wadameshSetup() {
  // Do NOT call Serial.begin() on the Tanmatsu (P4). The launcher already owns the USB-Serial-JTAG
  // console; re-initialising it here crashed the app during boot (before the splash could even draw
  // — the beta_21+ symptom of "blue screen, no boot log"). Serial output is swallowed on this board's
  // IDF console anyway, so printf (not Serial) carries the [BOOT] logs. See tanmatsu-serial-begin memory.
  delay(150);
  printf("[BOOT] wadamesh / tanmatsu\n");

  // badge-bsp: panel + input + power. The panel must be up before LVGL flushes to it; ask for
  // RGB565 to match LVGL's 16-bit color depth (lvglFlush -> writePixelsRGB565 -> bsp_display_blit).
  const bsp_configuration_t bspcfg = {
    .display = { .requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB, .num_fbs = 1 },
  };
  if (bsp_device_initialize(&bspcfg) != ESP_OK) Serial.println("[BOOT] bsp_device_initialize FAILED");

  bootSplashBegin();          // panel is up — start the on-screen boot log

  board.begin();
  // Bring up lwIP/tcpip + the default event loop before any WiFi/netif call. On the S3 boards
  // Arduino's WiFi path does this implicitly; on the P4/esp_wifi_remote it isn't, so WiFi.mode()
  // / WiFiServer would hit lwIP with no tcpip mbox and assert. Both are idempotent.
  esp_netif_init();
  esp_event_loop_create_default();
  bootLog("C6 radio coproc");
  // Power-cycle the C6 through the CH32 so the app ALWAYS gets a fresh C6. Without this,
  // re-launching the app (especially right after a flash) onto a C6 still in its previous
  // state wedges esp-hosted ("Not able to connect with ESP-Hosted slave") into a P4 reboot
  // loop that previously only a manual power cycle cleared — this does the same in software.
  {
    tanmatsu_coprocessor_handle_t cph = nullptr;
    if (bsp_tanmatsu_coprocessor_get_handle(&cph) == ESP_OK && cph) {
      tanmatsu_coprocessor_radio_disable(cph);
      delay(150);
      tanmatsu_coprocessor_radio_enable_application(cph);   // boot the C6 into its esp-hosted slave fw
      delay(600);                                           // let the C6 come up before we attach
    }
  }
  hostedConnectC6();          // identify the C6 slave so WiFi + LoRa work (must precede radio_init)
  bootLog("Radio link");
  if (!radio_init()) { bootLog("radio init FAILED"); Serial.println("[BOOT] radio_init FAILED"); }

  DisplayDriver* disp = &display;

  // Persistence: point DataStore at a writable directory the launcher has already mounted (see
  // discoverPersistMount + the PersistFS globals). DataStore (profile name, NodePrefs, contacts,
  // channels, chat history) is otherwise lost on reboot; NVS-backed stores (Wi-Fi creds, TouchPrefs)
  // persist independently. FAT is hierarchical (unlike the flat SPIFFS the other boards use), so the
  // subdirectories DataStore writes into must exist first — DataStore::begin() makes /bl, but on
  // ESP32 it never calls identity_store.begin() (RP2040-only), so /identity must be created here or
  // the node keypair can't be saved and a fresh identity is generated every boot.
  bootLog("Storage");
  // An AppFS app boots fresh, so enumerate the partitions we actually see, then mount the internal
  // 'locfd' FAT partition with FFat. printf (NOT Serial — Arduino Serial is swallowed on this board's
  // IDF console) so the result lands in the serial monitor.
  printf("[storage] partitions visible to the app:\n");
  esp_partition_iterator_t pit = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
  for (; pit != NULL; pit = esp_partition_next(pit)) {
    const esp_partition_t* pp = esp_partition_get(pit);
    printf("[storage]   %-10s type=%d sub=%d off=0x%06lx size=0x%06lx\n",
           pp->label, (int)pp->type, (int)pp->subtype, (unsigned long)pp->address, (unsigned long)pp->size);
  }
  if (pit) esp_partition_iterator_release(pit);
  g_fs_ok = FFat.begin(true, "/ffat", 10, "locfd");
  printf("[storage] FFat.begin(locfd) = %s\n", g_fs_ok ? "OK" : "FAILED");
  if (g_fs_ok) {
    printf("[storage] FFat %u KB used / %u KB total\n",
           (unsigned)(FFat.usedBytes() / 1024), (unsigned)(FFat.totalBytes() / 1024));
    // FAT is hierarchical: DataStore::begin() makes /bl, but on ESP32 it never calls
    // identity_store.begin() (RP2040-only), so /identity is missing — without it the node keypair
    // can't be saved and a fresh identity is generated every boot.
    FFat.mkdir("/identity");
    FFat.mkdir("/bl");
  } else {
    printf("[storage] persistence disabled this boot (DataStore opens fail gracefully)\n");
  }
  // The internal FFat 'locfd' on this P4 has a broken FAT metadata layer (see the tile-cache notes):
  // open/read/write work but f_stat/exists()/size() return garbage — and WHICH garbage shifts with
  // the build (-Og "worked by luck"; -Os made the exists()-gated identity + prefs loads come up
  // empty: fresh node identity, default name, profile changes lost every reboot). The card's FAT
  // metadata is truthful, so the WHOLE store lives there now; FFat remains only the no-card fallback.
  g_sd_ok = SD_MMC.begin("/sdcard", false /*1-bit*/) && SD_MMC.cardType() != CARD_NONE;
  printf("[storage] SD_MMC.begin = %s\n", g_sd_ok ? "OK" : "no card");
  if (g_sd_ok) {
    SD_MMC.mkdir("/meshcomod");
    SD_MMC.mkdir("/meshcomod/identity");
    SD_MMC.mkdir("/meshcomod/bl");
    if (g_fs_ok) {
      // One-time rescue of identity + prefs off FFat: probe by OPEN + READ (never
      // exists()/size() on this FS). Files the card already holds win — SD metadata
      // is honest, so exists() is safe THERE.
      static const char* k_mig[] = { "/new_prefs", "/new_prefs.tmp", "/node_prefs",
                                     "/identity/_main.id" };
      for (const char* nm : k_mig) {
        char dst[48];
        snprintf(dst, sizeof dst, "/meshcomod%s", nm);
        if (SD_MMC.exists(dst)) continue;
        File s = FFat.open(nm, FILE_READ);
        if (!s) continue;
        uint8_t buf[512];
        size_t n = s.read(buf, sizeof buf);
        if (n == 0) { s.close(); continue; }             // ghost/empty entry — nothing to keep
        File d = SD_MMC.open(dst, FILE_WRITE);
        if (!d) { s.close(); continue; }
        size_t total = 0;
        do { d.write(buf, n); total += n; n = s.read(buf, sizeof buf); } while (n > 0);
        d.close();
        s.close();
        printf("[storage] migrated %s -> SD:/meshcomod (%u B)\n", nm, (unsigned)total);
      }
    }
    store.useSdMmcStorage();   // identity/prefs/contacts/channels all on the card
  }
  // No card: FFat stays the store, as before — identity/prefs loads there depend
  // on the broken metadata layer and may be unreliable, but it is the only
  // persistent option on a card-less unit.
  store.begin();

  bootLog("Mesh stack");
  the_mesh.begin(disp != NULL);

  bootLog("Companion interface");
  serial_interface.begin(Serial, TCP_PORT, WS_PORT);
  serial_interface.setBroadcastResponses(true);
  the_mesh.startInterface(serial_interface);

#if defined(BLE_PIN_CODE)
  // BLE companion over the C6: bring up the remote BLE controller (esp-hosted), then the NimBLE host
  // (esp-nimble-cpp). hostedInitBLE() is arduino's controller bring-up; our build.sh patch lets its
  // hostedInit() tolerate the already-configured esp_hosted (from hostedConnectC6()).
  bootLog("Bluetooth");
  if (hostedInitBLE()) {
    char* nm = the_mesh.getNodePrefs()->node_name;
    serial_interface.prepareBle("wadamesh-", nm, the_mesh.getBLEPin());
    if (wifiConfigGetBleEnabled())
      serial_interface.beginBle("wadamesh-", nm, the_mesh.getBLEPin());
  } else {
    Serial.println("[BOOT] hostedInitBLE FAILED");
  }
#endif

  bootLog("Sensors");
  sensors.begin();
  bootLog("Building UI");
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());
  board.onBootComplete();
  bootLog("Ready");
  bootSplashEnd();            // stop the animator + clean black handoff

  // The splash wrote straight to the panel framebuffer behind LVGL's back, so LVGL still believes
  // the screen is unchanged and would leave our black fill on top of the UI. Invalidate every layer
  // so the first lv_timer_handler() repaints the whole UI over the handoff.
  if (lv_disp_t* d = lv_disp_get_default()) {
    lv_obj_invalidate(lv_disp_get_scr_act(d));
    lv_obj_invalidate(lv_disp_get_layer_top(d));
    lv_obj_invalidate(lv_disp_get_layer_sys(d));
  }
  Serial.println("[BOOT] setup done");
}

extern "C" void app_main(void) {
  initArduino();
  wadameshSetup();

  // WiFi state machine + SNTP + TCP companion server. On the S3 boards this lives in
  // src/main.cpp's loop() — which the Tanmatsu build excludes — so without it the UI's
  // wifiConfigRequestApply() is never consumed and WiFi.begin() never runs (radio sits in
  // "connecting" forever). Ported faithfully here.
  bool     wifi_started = false, wifi_radio_prev = true, wifi_radio_inited = false;
  bool     sntp_kicked = false, sntp_pushed = false, modem_sleep_set = false;
  uint32_t last_wifi_retry_ms = 0, sntp_kick_ms = 0;
  const uint32_t WIFI_RETRY_INTERVAL_MS = 10000;

  for (;;) {
    ui_task.loop();      // UI first (splash/flush)

    bool wifi_radio_en = wifiConfigWantsWifi();
    if (!wifi_radio_inited) { wifi_radio_inited = true; wifi_radio_prev = wifi_radio_en; }
    else if (wifi_radio_en != wifi_radio_prev) {
      wifi_radio_prev = wifi_radio_en;
      if (!wifi_radio_en) { WiFi.disconnect(true); delay(50); WiFi.mode(WIFI_OFF); }
      wifi_started = false;
    }
    if (wifiConfigConsumeApplyRequest()) {          // UI changed SSID/PWD or toggled the radio
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
      if (wifiConfigHasRuntime() && WiFi.status() != WL_CONNECTED) {   // periodic reconnect
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
        // Start the TCP (+ WebSocket) companion server now the STA is associated. Deferred to here on
        // purpose: starting WiFiServer before association asserts in lwIP (the old reason this was
        // stubbed out). startTcpServer() is idempotent — it no-ops once the server is up — so calling
        // it on every connected tick is safe.
        serial_interface.startTcpServer(true);
        if (!sntp_kicked) {
          char tz[48]; touchPrefsBuildLocalTz(tz, sizeof tz);
          configTzTime(tz, "pool.ntp.org", "time.google.com");
          sntp_kicked = true; sntp_kick_ms = millis();
        } else if (!sntp_pushed && (uint32_t)(millis() - sntp_kick_ms) >= 1500) {
          time_t t = time(nullptr);
          if (t > 1700000000) { rtc_clock.setCurrentTime((uint32_t)t); sntp_pushed = true; }
        }
      } else if (sntp_kicked && !sntp_pushed) { sntp_kicked = false; }
    }
    serial_interface.tickWebSocketHandshake();   // accept WS clients/handshakes (no-op until the WS server is up)

    the_mesh.loop();     // mesh + companion servers
    vTaskDelay(1);
  }
}
