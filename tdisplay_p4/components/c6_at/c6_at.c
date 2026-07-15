// SPDX-License-Identifier: GPL-3.0-or-later
// c6_at — AT-over-SDIO client for the T-Display P4's ESP32-C6 (ESP-AT firmware). See c6_at.h.
// Clean-room: SDIO transport via Espressif's public SDMMC + esp_serial_slave_link (ESSL); the AT
// framing / worker / Wi-Fi logic on top is our own.
#include "c6_at.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_default_configs.h"
#include "sdmmc_cmd.h"
#include "esp_serial_slave_link/essl.h"
#include "esp_serial_slave_link/essl_sdio.h"
#include "esp_heap_caps.h"

static const char *TAG = "c6_at";

// ---- C6 SDIO wiring (T-Display P4, "SDIO_2" — slot 1) ----
#define C6_SDIO_SLOT   1
#define C6_SDIO_CLK    18
#define C6_SDIO_CMD    19
#define C6_SDIO_D0     14
#define C6_SDIO_D1     15
#define C6_SDIO_D2     16
#define C6_SDIO_D3     17
// ESP-AT SDIO uses a 2048-byte transfer buffer on the slave; the host recv size must match.
#define C6_RECV_BUF    2048
#define C6_TX_CAP      2048          // AT lines AND raw socket payload chunks go out through s_tx
// SDIO clock: conservative for reliable bring-up; ESP-AT SDIO is happy at 20 MHz.
#define C6_SDIO_KHZ    SDMMC_FREQ_DEFAULT   // 20 MHz

static sdmmc_card_t *s_card  = NULL;
static essl_handle_t s_essl  = NULL;
static volatile bool s_up    = false;
static volatile bool s_dead  = false;
// ESSL SDIO transfers DMA straight from/into these buffers: they must be DMA-capable internal RAM,
// and on the P4 (cached internal RAM) both the ADDRESS and the transfer SIZE must be cache-line
// aligned — hence heap_caps_aligned_alloc(64) + padding sends up to a 64-multiple.
static uint8_t      *s_rx = NULL;
static uint8_t      *s_tx = NULL;
static size_t        s_align = 64;   // ESP32-P4 L1 cache line

// ---- lock-free status cache (single writer = worker; readers are the UI/facade) -----------------
#define SCAN_MAX 24
typedef struct { char ssid[33]; int8_t rssi; uint8_t enc; uint8_t chan; } scan_item_t;
static scan_item_t   s_scan[SCAN_MAX];
static volatile int  s_scan_n     = 0;
static volatile int  s_scan_state = -2;          // -2 idle/failed, -1 running, >=0 count
static volatile bool s_connected  = false;
static volatile uint32_t s_ip4    = 0;
static char          s_cur_ssid[33] = {0};
static volatile int  s_cur_rssi  = 0;
static volatile int  s_cur_chan  = 0;
static volatile bool s_sta_en    = false;        // facade WiFi.mode() bookkeeping only

// ---- sockets (AT+CIP*, CIPMUX=1 + CIPRECVMODE=1 passive receive) ---------------------------------
// Passive mode: TCP data buffers ON the C6; "+IPD,<id>,<len>" arrives as a bare notification line and
// the worker PULLS payload with AT+CIPRECVDATA into a per-link PSRAM ring. That keeps every byte of
// binary payload inside a bounded command exchange — no async binary demux in the RX stream.
#define SOCK_MAX       5
#define SOCK_RING      16384          // per-link rx ring (PSRAM)
#define SOCK_PULL_MAX  1436          // per-CIPRECVDATA pull (fits the 2 KB collectors with header room)
typedef struct {
    volatile bool used;              // link id allocated by a client
    volatile bool open;              // TCP established (cleared by "<id>,CLOSED" or close)
    volatile bool rx_notified;       // C6 says data is waiting (+IPD seen)
    volatile bool pull_inflight;     // a REQ_PULL for this link is queued/running
    uint8_t      *ring;              // SOCK_RING bytes, PSRAM, lazy
    volatile uint32_t r, w;          // ring indexes (w-r = fill); single producer (worker), single consumer
} sock_t;
static sock_t s_sock[SOCK_MAX];

// Inbound server (AT+CIPSERVER, single port). Want-flag design: c6at_server_start just records
// the wish; the worker's idle housekeeping brings the listener up once the AT link exists (and
// re-arms it if the caller asked before the worker was even spawned — the TCP companion latches
// its own "started" flag, so a lost first request would otherwise never retry). Accept FIFO:
// worker produces (urc_scan sees "<id>,CONNECT" on a link no outbound owner marked used), the
// app loop consumes via c6at_server_accept(). Free-running u8 indices; fill = w - r.
static volatile uint16_t s_srv_port = 0;
static volatile bool     s_srv_want = false;
static volatile bool     s_srv_up   = false;
static volatile int8_t   s_acc_q[8];
static volatile uint8_t  s_acc_r = 0, s_acc_w = 0;

// SNTP over the C6: worker configures it after a join and polls AT+CIPSNTPTIME? until synced.
static volatile uint32_t s_sntp_epoch = 0;   // UTC epoch at s_sntp_at_ms
static volatile uint32_t s_sntp_at_ms = 0;   // esp_timer ms when it was sampled
static bool s_sntp_cfged = false;

// ---- worker request queue -----------------------------------------------------------------------
typedef enum { REQ_SCAN = 1, REQ_JOIN, REQ_DISCONNECT,
               REQ_SOCK_CONNECT, REQ_SOCK_SEND, REQ_SOCK_PULL, REQ_SOCK_CLOSE,
               REQ_SERVER_START, REQ_SERVER_STOP } req_type_t;
// Blocking bridge: the caller stack-allocates `status` (REQ_PENDING) and polls it; the worker writes
// the outcome. Callers block in their own tasks (tile fetcher / reader / version check), never LVGL.
#define REQ_PENDING 0
#define REQ_OK      1
#define REQ_FAIL    2
typedef struct {
    req_type_t type;
    char ssid[33]; char pass[65];              // JOIN
    char host[96]; uint16_t port; bool ssl;    // SOCK_CONNECT
    int  id;                                   // SOCK_*
    const uint8_t *data; size_t len;           // SOCK_SEND (caller's buffer; caller blocks until done)
    volatile int *status;                      // REQ_* result (may be NULL for fire-and-forget)
} c6_req_t;
static QueueHandle_t s_reqq = NULL;
static TaskHandle_t  s_worker = NULL;

// ---- transport ---------------------------------------------------------------------------------

static bool sdio_bring_up(void) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = C6_SDIO_SLOT;
    // ALLOC_ALIGNED_BUF: generic ESSL reads its 4-byte status registers into unaligned stack vars —
    // this flag makes the SDMMC layer auto-bounce those through an aligned buffer instead of
    // returning INVALID_ARG on the cache-aligned P4. (Our own AT buffers skip the bounce.)
    host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.max_freq_khz = C6_SDIO_KHZ;

    sdmmc_slot_config_t slot = {
        .clk = C6_SDIO_CLK, .cmd = C6_SDIO_CMD,
        .d0 = C6_SDIO_D0, .d1 = C6_SDIO_D1, .d2 = C6_SDIO_D2, .d3 = C6_SDIO_D3,
        .cd = SDMMC_SLOT_NO_CD, .wp = SDMMC_SLOT_NO_WP,
        .width = 4, .flags = 0,
    };

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {   // INVALID_STATE = already inited (SD_MMC slot 0)
        ESP_LOGE(TAG, "sdmmc_host_init: %s", esp_err_to_name(err)); return false;
    }
    err = sdmmc_host_init_slot(C6_SDIO_SLOT, &slot);
    if (err != ESP_OK) { ESP_LOGE(TAG, "sdmmc_host_init_slot: %s", esp_err_to_name(err)); return false; }

    s_card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (!s_card) return false;
    err = sdmmc_card_init(&host, s_card);      // probes the C6 as an SDIO (IO) card
    if (err != ESP_OK) { ESP_LOGE(TAG, "sdmmc_card_init (C6 not on SDIO?): %s", esp_err_to_name(err)); return false; }
    ESP_LOGI(TAG, "SDIO card up");

    essl_sdio_config_t cfg = { .card = s_card, .recv_buffer_size = C6_RECV_BUF };
    err = essl_sdio_init_dev(&s_essl, &cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "essl_sdio_init_dev: %s", esp_err_to_name(err)); return false; }
    err = essl_init(s_essl, 1000);
    if (err != ESP_OK) { ESP_LOGE(TAG, "essl_init: %s", esp_err_to_name(err)); return false; }
    (void)essl_wait_for_ready(s_essl, 2000);
    return true;
}

// Drain pending RX packets from the C6 into `out` (up to sz-1), NUL-terminate. Returns bytes read.
// Polls essl_get_packet (the C6 AT slave doesn't reliably raise the SDIO interrupt for us); the echo
// and the "OK" arrive as separate packets, so keep draining until 2 empty polls after data.
static size_t drain_rx(char *out, size_t sz, uint32_t deadline_ms) {
    size_t n = 0;
    int empty = 0;
    while ((uint32_t)(esp_timer_get_time() / 1000) < deadline_ms) {
        size_t got = 0;
        essl_get_packet(s_essl, s_rx, C6_RECV_BUF, &got, 50);
        if (got > 0) {
            empty = 0;
            if (out) {
                size_t cp = (n + got <= sz - 1) ? got : (sz - 1 - n);
                memcpy(out + n, s_rx, cp);
                n += cp;
            } else {
                n += got;
            }
        } else {
            if (n > 0 && ++empty >= 2) break;   // saw data, now quiet -> response complete
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    if (out) out[(n < sz) ? n : sz - 1] = '\0';
    return n;
}

// ---- AT client (worker-thread only once the worker runs) ----------------------------------------

static void urc_scan(const char *buf, size_t n);   // spot +IPD / <id>,CLOSED lines in any response

bool c6at_command(const char *cmd, char *resp, size_t resp_sz, uint32_t timeout_ms) {
    if (!s_essl || !s_tx) return false;
    int len = snprintf((char *)s_tx, C6_TX_CAP, "%s\r\n", cmd);
    if (len <= 0 || len >= C6_TX_CAP) return false;
    // The SDIO DMA transfer size must be a multiple of the cache line: pad with '\n' (ESP-AT
    // ignores empty lines).
    size_t plen = ((size_t)len + s_align - 1) / s_align * s_align;
    if (plen > C6_TX_CAP) plen = C6_TX_CAP;
    for (size_t i = (size_t)len; i < plen; i++) s_tx[i] = '\n';

    esp_err_t serr = essl_send_packet(s_essl, s_tx, plen, 1000);
    if (serr != ESP_OK) { ESP_LOGW(TAG, "send '%s' failed: %s", cmd, esp_err_to_name(serr)); return false; }

    // Collect the reply until a terminating status line appears. Static: only the worker calls this.
    static char buf[3584]; size_t n = 0; buf[0] = '\0';
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + timeout_ms;
    bool ok = false, done = false;
    while (!done && (uint32_t)(esp_timer_get_time() / 1000) < deadline) {
        static char chunk[C6_RECV_BUF];
        size_t got = drain_rx(chunk, sizeof(chunk), deadline);
        if (got) {
            size_t cp = (n + got < sizeof(buf) - 1) ? got : (sizeof(buf) - 1 - n);
            memcpy(buf + n, chunk, cp); n += cp; buf[n] = '\0';
        }
        if (strstr(buf, "\r\nOK\r\n")    || (n >= 4 && !strncmp(buf, "OK\r\n", 4)))   { ok = true;  done = true; }
        else if (strstr(buf, "\r\nERROR\r\n") || strstr(buf, "\r\nFAIL\r\n"))          { ok = false; done = true; }
    }
    urc_scan(buf, n);   // "+IPD"/"<id>,CLOSED" lines interleave with command output — never miss them
    if (resp) { strncpy(resp, buf, resp_sz - 1); resp[resp_sz - 1] = '\0'; }
    ESP_LOGI(TAG, "AT '%s' -> %s (%u bytes)", cmd, ok ? "OK" : (done ? "ERR" : "TIMEOUT"), (unsigned)n);
    return ok;
}

// ---- TEMP diagnostic: scan the C6's mfg_nvs partition for the baked-in GATT service table --------
// ESP-AT v4 keeps its factory data (and, we hope, the BLE services blob) in the 'mfg_nvs' partition,
// which AT+SYSFLASH exposes read/write over PLAIN AT — i.e. a possible cable-free path to replacing
// the demo GATT table with MeshCore's NUS UUIDs. This scanner chunk-reads the partition and reports
// GATT signatures (0x2800 primary-svc / 0x2803 char-decl / 0x2902 CCCD / 0xA002+0xC300 demo UUIDs)
// and ASCII NVS key names. Diagnostic only — compiled out once the layout is known.

// ---- TEMP diagnostic: replace the C6's GATT table with MeshCore's NUS over AT+SYSMFG -------------
// The factory table (stock esp-at demo, cfg0..cfg30 in mfg_nvs "ble_data") can't serve the MeshCore
// app's 128-bit Nordic-UART UUIDs. AT+SYSMFG edits those rows key-by-key (firmware does the NVS
// writes; full factory image backed up in tdisplay_p4/c6_mfg_nvs_factory_backup.bin). 128-bit hex
// byte order is undocumented — this writes LITTLE-ENDIAN (reversed) and reads back BLEGATTSCHAR?
// to check; if the echo is reversed we flip. Row format: index,uuid_len,uuid,perm,max,cur,value.
#define C6AT_DIAG_SYSMFG 1
#if C6AT_DIAG_SYSMFG
static bool collect_until(const char *token, char *acc, size_t acc_sz, uint32_t timeout_ms);
static bool sysmfg_write_row(int idx, const char *row) {
    static char acc[512];
    int clen = snprintf((char *)s_tx, C6_TX_CAP, "AT+SYSMFG=2,\"ble_data\",\"cfg%d\",7,%u\r\n",
                        idx, (unsigned)strlen(row));
    size_t plen = ((size_t)clen + s_align - 1) / s_align * s_align;
    for (size_t i = (size_t)clen; i < plen; i++) s_tx[i] = '\n';
    if (essl_send_packet(s_essl, s_tx, plen, 1000) != ESP_OK) return false;
    if (!collect_until(">", acc, sizeof(acc), 3000)) { printf("[nus] cfg%d no prompt: %s\n", idx, acc); return false; }
    // Data phase: EXACT length first — pad bytes after the counted data desync the SYSMFG reader
    // (unlike CIPSEND, which tolerates them). ALLOC_ALIGNED_BUF bounces small unaligned DMA sends;
    // fall back to '\n' padding only if the raw send is rejected by the host driver.
    int dl = snprintf((char *)s_tx, C6_TX_CAP, "%s", row);
    esp_err_t serr = essl_send_packet(s_essl, s_tx, (size_t)dl, 1000);
    if (serr != ESP_OK) {
        size_t dpad = ((size_t)dl + s_align - 1) / s_align * s_align;
        for (size_t i = (size_t)dl; i < dpad; i++) s_tx[i] = '\n';
        if (essl_send_packet(s_essl, s_tx, dpad, 1000) != ESP_OK) return false;
        printf("[nus] cfg%d exact-len send rejected (0x%x), padded fallback used\n", idx, serr);
    }
    if (!collect_until("OK", acc, sizeof(acc), 3000)) { printf("[nus] cfg%d write FAIL: %s\n", idx, acc); return false; }
    printf("[nus] cfg%d <- %s\n", idx, row);
    // Self-sync: ping until the parser answers OK again (absorbs any queued stray responses).
    for (int t = 0; t < 6; t++) {
        if (c6at_command("AT", NULL, 0, 400)) return true;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    printf("[nus] cfg%d: parser never re-synced\n", idx);
    return false;
}
// One-time GATT-table provisioning + BLE bring-up. Idempotent: if cfg0 already holds the MeshCore
// service row, the table writes are skipped and only init+advertising run. The factory demo table
// is preserved in tdisplay_p4/c6_mfg_nvs_factory_backup.bin (restore = write the 31 original rows
// back through the same SYSMFG mechanism).
#define NUS_SVC_LE "9ECADC240EE5A9E093F3A3B50100406E"   // 6E400001-B5A3-F393-E0A9-E50E24DCCA9E, LE
static void c6at_ble_provision(void) {
    static char r[1024];
    if (!c6at_command("AT+SYSMFG=1,\"ble_data\",\"cfg0\"", r, sizeof(r), 2000)) {
        printf("[nus] SYSMFG absent/unreadable — BLE unavailable on this AT build\n");
        return;
    }
    if (!strstr(r, NUS_SVC_LE)) {
        printf("[nus] provisioning MeshCore GATT table (one-time)\n");
        c6at_command("AT+BLEINIT=0", NULL, 0, 2000);   // ble_data may only change while BLE is down
        // MeshCore NUS (SerialBLEInterface): svc 6E400001-..., RX 6E400002 (write),
        // TX 6E400003 (read+notify) + CCCD. 128-bit hex = REVERSED (little-endian, NimBLE order).
        static const char *rows[] = {
            "0,16,0x2800,0x01,16,16," NUS_SVC_LE,
            "1,16,0x2803,0x01,1,1,08",                                    // props: WRITE
            "2,128,0x9ECADC240EE5A9E093F3A3B50200406E,0x10,512,512,",     // RX value (perm WRITE)
            "3,16,0x2803,0x01,1,1,12",                                    // props: READ|NOTIFY
            "4,128,0x9ECADC240EE5A9E093F3A3B50300406E,0x01,512,512,",     // TX value (perm READ)
            "5,16,0x2902,0x11,2,2,0000",                                  // CCCD rw
        };
        for (int i = 0; i < 6; i++) {
            c6at_command("AT", NULL, 0, 400);          // absorb strays; keep the parser in sync
            vTaskDelay(pdMS_TO_TICKS(80));
            if (!sysmfg_write_row(i, rows[i])) {
                vTaskDelay(pdMS_TO_TICKS(300));
                c6at_command("AT", NULL, 0, 400);
                if (!sysmfg_write_row(i, rows[i])) return;   // next boot retries the full table
            }
        }
        for (int i = 6; i <= 30; i++) {                // drop the demo rows beyond our table
            char cmd[48];
            snprintf(cmd, sizeof(cmd), "AT+SYSMFG=0,\"ble_data\",\"cfg%d\"", i);
            c6at_command(cmd, NULL, 0, 2000);
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        printf("[nus] GATT table provisioned\n");
    }
    // BLE stays OFF: this dev AT build can't set adv data / start advertising (BLEADVDATA /
    // BLEADVDATAEX / BLESCANRSPDATA / BLEADVSTART all ERROR; BLEINIT=2 only fires an unnamed,
    // uncontrollable auto-advert). Leaving it down keeps the airwaves clean; the provisioned
    // MeshCore table means a future LilyGo AT firmware with working advertising lights up
    // instantly. UI-side, MultiTransport::hasBleCapability() is false on this board.
    c6at_command("AT+BLEINIT=0", NULL, 0, 2000);
}
#endif

bool c6at_begin(void) {
    if (s_up) return true;
    ESP_LOGI(TAG, "bringing up C6 AT over SDIO slot %d ...", C6_SDIO_SLOT);
    if (!s_rx) s_rx = heap_caps_aligned_alloc(s_align, C6_RECV_BUF, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_tx) s_tx = heap_caps_aligned_alloc(s_align, C6_TX_CAP,   MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_rx || !s_tx) { ESP_LOGE(TAG, "DMA buffer alloc failed"); return false; }
    if (!sdio_bring_up()) return false;

    for (int i = 0; i < 10; i++) {                 // the C6 may still be booting ESP-AT
        char r[64];
        if (c6at_command("AT", r, sizeof(r), 500)) { s_up = true; break; }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!s_up) { ESP_LOGE(TAG, "C6 never answered AT"); return false; }

    c6at_command("ATE0", NULL, 0, 500);            // echo off
    c6at_command("AT+CWMODE=1", NULL, 0, 1000);    // station mode
    c6at_command("AT+CIPMUX=1", NULL, 0, 1000);    // multi-connection (link ids 0..4)
    c6at_command("AT+CIPRECVMODE=1", NULL, 0, 1000); // passive rx: data buffers on the C6, host pulls
    {   // Log the slave firmware version once per boot (cheap, invaluable in bug reports).
        // BLE survey result (2026-07-15, factory LilyGo ESP-AT v4.1.0.0-dev): BLE commands PRESENT,
        // but the GATT table is the BAKED-IN stock demo (16-bit 0xC30x/0xC40x chars) — ESP-AT can't
        // create runtime services, so the MeshCore app's 128-bit UUIDs can never exist on this build.
        // No self-OTA either (AT+CIUPDATE/AT+USEROTA both absent) — a C6 firmware change is a WIRED
        // flash via the C6 UART0/IO9 pads. BLE on this board therefore needs the C6 reflashed
        // (esp-hosted slave = the Tanmatsu path, or a custom AT build) — a product decision, not code.
        char r[512];
        if (c6at_command("AT+GMR", r, sizeof(r), 1000)) printf("[c6at] GMR:\n%s\n", r);
    }
#if C6AT_DIAG_SYSMFG
    c6at_ble_provision();
#endif
    ESP_LOGI(TAG, "C6 ESP-AT is up");
    return true;
}

bool c6at_is_up(void)   { return s_up; }
bool c6at_is_dead(void) { return s_dead; }

// ---- worker internals ----------------------------------------------------------------------------

// Spot unsolicited result codes in any text the C6 sends. Passive mode means +IPD carries NO payload
// (just "data waiting"), so plain string scanning is safe.
static void urc_scan(const char *buf, size_t n) {
    (void)n;
    const char *p = buf;
    while ((p = strstr(p, "+IPD,")) != NULL) {
        int id = atoi(p + 5);
        if (id >= 0 && id < SOCK_MAX) s_sock[id].rx_notified = true;
        p += 5;
    }
    for (int id = 0; id < SOCK_MAX; id++) {
        char pat[12];
        snprintf(pat, sizeof(pat), "%d,CLOSED", id);
        if (strstr(buf, pat)) { s_sock[id].open = false; }
    }
    // Inbound server connections: "<id>,CONNECT" on a link NO outbound owner has marked used
    // (c6at_sock_connect sets used=true before the worker ever sends CIPSTART, so an outbound
    // link's own CONNECT line can't be mistaken for an inbound one). Runs in worker/bring-up
    // context only — the PSRAM ring malloc is safe here.
    if (s_srv_up) {
        for (int id = 0; id < SOCK_MAX; id++) {
            if (s_sock[id].used) continue;
            char pat[14];
            snprintf(pat, sizeof(pat), "%d,CONNECT", id);
            if (!strstr(buf, pat)) continue;
            sock_t *sk = &s_sock[id];
            if (!sk->ring) sk->ring = heap_caps_malloc(SOCK_RING, MALLOC_CAP_SPIRAM);
            if (!sk->ring) continue;             // no RAM: ignore; the peer will time out
            sk->r = sk->w = 0; sk->rx_notified = false; sk->pull_inflight = false;
            sk->open = true; sk->used = true;    // used LAST: publishes the slot
            if ((uint8_t)(s_acc_w - s_acc_r) < 8) { s_acc_q[s_acc_w % 8] = (int8_t)id; s_acc_w++; }
            ESP_LOGI(TAG, "server: inbound connection on link %d", id);
        }
    }
}

static inline uint32_t ring_fill(sock_t *s) { return s->w - s->r; }
static inline uint32_t ring_free(sock_t *s) { return SOCK_RING - ring_fill(s); }

static void ring_push(sock_t *s, const uint8_t *d, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) s->ring[(s->w + i) % SOCK_RING] = d[i];
    s->w += len;   // publish after the bytes are in place (single producer)
}

// Bespoke collector: drain the RX stream into `acc` until `token` appears (or deadline). Used for the
// CIPSEND '>' prompt and the SEND OK trailer, where c6at_command's OK/ERROR framing doesn't apply.
static bool collect_until(const char *token, char *acc, size_t acc_sz, uint32_t timeout_ms) {
    size_t n = 0; acc[0] = '\0';
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + timeout_ms;
    while ((uint32_t)(esp_timer_get_time() / 1000) < deadline) {
        size_t got = 0;
        essl_get_packet(s_essl, s_rx, C6_RECV_BUF, &got, 40);
        if (got) {
            size_t cp = (n + got < acc_sz - 1) ? got : (acc_sz - 1 - n);
            memcpy(acc + n, s_rx, cp); n += cp; acc[n] = '\0';
            if (strstr(acc, token)) { urc_scan(acc, n); return true; }
            if (strstr(acc, "ERROR")) { urc_scan(acc, n); return false; }
        } else {
            vTaskDelay(pdMS_TO_TICKS(4));
        }
    }
    urc_scan(acc, n);
    return false;
}

static void worker_sock_connect(c6_req_t *rq) {
    sock_t *s = &s_sock[rq->id];
    if (!s->ring) s->ring = heap_caps_malloc(SOCK_RING, MALLOC_CAP_SPIRAM);
    if (!s->ring) { if (rq->status) *rq->status = REQ_FAIL; return; }
    s->r = s->w = 0; s->rx_notified = false; s->open = false;
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=%d,\"%s\",\"%s\",%u",
             rq->id, rq->ssl ? "SSL" : "TCP", rq->host, (unsigned)rq->port);
    bool ok = c6at_command(cmd, NULL, 0, 15000);
    s->open = ok;
    if (rq->status) *rq->status = ok ? REQ_OK : REQ_FAIL;
}

static void worker_sock_send(c6_req_t *rq) {
    sock_t *s = &s_sock[rq->id];
    const uint8_t *p = rq->data;
    size_t left = rq->len;
    bool ok = s->open;
    static char acc[512];
    while (ok && left > 0) {
        size_t chunk = left > SOCK_PULL_MAX ? SOCK_PULL_MAX : left;
        char cmd[40];
        snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%d,%u", rq->id, (unsigned)chunk);
        int clen = snprintf((char *)s_tx, C6_TX_CAP, "%s\r\n", cmd);
        size_t plen = ((size_t)clen + s_align - 1) / s_align * s_align;
        for (size_t i = (size_t)clen; i < plen; i++) s_tx[i] = '\n';
        if (essl_send_packet(s_essl, s_tx, plen, 1000) != ESP_OK) { ok = false; break; }
        if (!collect_until(">", acc, sizeof(acc), 3000)) { ok = false; break; }
        // Raw payload: exactly `chunk` bytes count; the '\n' padding lands in the AT parser as
        // ignorable empty lines.
        memcpy(s_tx, p, chunk);
        size_t dlen = (chunk + s_align - 1) / s_align * s_align;
        for (size_t i = chunk; i < dlen; i++) s_tx[i] = '\n';
        if (essl_send_packet(s_essl, s_tx, dlen, 2000) != ESP_OK) { ok = false; break; }
        // 4 s, not more: SEND OK normally lands in ms; it only drags when the peer stopped
        // ACKing and the C6's socket buffer is full. Every second spent here stalls the whole
        // AT worker (companion + HTTP + mirror), so fail fast and let the caller's reaper
        // drop the sick client.
        if (!collect_until("SEND OK", acc, sizeof(acc), 4000)) { ok = false; break; }
        p += chunk; left -= chunk;
    }
    // Success at debug only — the web-mirror stream sends many frames/s and a per-send
    // INFO line would both flood the console and throttle the stream.
    if (ok) ESP_LOGD(TAG, "sock%d send %u -> OK", rq->id, (unsigned)rq->len);
    else    ESP_LOGI(TAG, "sock%d send %u -> FAIL", rq->id, (unsigned)rq->len);
    if (rq->status) *rq->status = ok ? REQ_OK : REQ_FAIL;
}

// Pull buffered TCP payload off the C6 into the link's ring. Binary-safe bespoke parse: the payload
// may contain "OK\r\n", so we parse "+CIPRECVDATA:<len>," structurally and count bytes.
static void worker_sock_pull(int id) {
    sock_t *s = &s_sock[id];
    s->pull_inflight = false;          // re-kickable as soon as we've run
    if (!s->ring) return;
    uint32_t space = ring_free(s);
    if (space < 128) return;           // consumer hasn't caught up — retry on the next kick
    uint32_t want = space - 1; if (want > SOCK_PULL_MAX) want = SOCK_PULL_MAX;

    int clen = snprintf((char *)s_tx, C6_TX_CAP, "AT+CIPRECVDATA=%d,%u\r\n", id, (unsigned)want);
    size_t plen = ((size_t)clen + s_align - 1) / s_align * s_align;
    for (size_t i = (size_t)clen; i < plen; i++) s_tx[i] = '\n';
    if (essl_send_packet(s_essl, s_tx, plen, 1000) != ESP_OK) return;

    static uint8_t acc[SOCK_PULL_MAX + 512];
    size_t n = 0;
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + 3000;
    int hdr_at = -1, data_len = -1; size_t data_off = 0;
    while ((uint32_t)(esp_timer_get_time() / 1000) < deadline) {
        size_t got = 0;
        essl_get_packet(s_essl, s_rx, C6_RECV_BUF, &got, 40);
        if (got) {
            size_t cp = (n + got <= sizeof(acc)) ? got : (sizeof(acc) - n);
            memcpy(acc + n, s_rx, cp); n += cp;
        } else { vTaskDelay(pdMS_TO_TICKS(3)); }
        if (hdr_at < 0 && n > 16) {
            acc[n < sizeof(acc) ? n : sizeof(acc) - 1] = 0;
            char *h = strstr((char *)acc, "+CIPRECVDATA:");
            if (h) {
                data_len = atoi(h + 13);
                char *c = strchr(h + 13, ',');
                if (c) { hdr_at = (int)((uint8_t *)c - acc); data_off = (size_t)hdr_at + 1; }
                else if (data_len == 0) { break; }
            } else if (strstr((char *)acc, "ERROR")) { break; }
        }
        if (hdr_at >= 0 && data_len >= 0 && n >= data_off + (size_t)data_len) break;   // payload complete
    }
    if (hdr_at >= 0 && data_len > 0 && n >= data_off + (size_t)data_len) {
        ring_push(s, acc + data_off, (uint32_t)data_len);
        if ((uint32_t)data_len >= want) s->rx_notified = true;   // more likely waiting — keep pulling
        else                            s->rx_notified = false;
        ESP_LOGI(TAG, "sock%d pull %d bytes (ring %u)", id, data_len, (unsigned)ring_fill(s));
    } else {
        s->rx_notified = false;        // nothing there (or error) — wait for the next +IPD
    }
    urc_scan((char *)acc, n);
}

static void worker_sntp_service(bool just_joined) {
    if (just_joined && !s_sntp_cfged) {
        s_sntp_cfged = c6at_command("AT+CIPSNTPCFG=1,0,\"pool.ntp.org\",\"time.google.com\"", NULL, 0, 2000);
    }
    if (!s_sntp_cfged || s_sntp_epoch) return;
    static char resp[192];
    if (!c6at_command("AT+CIPSNTPTIME?", resp, sizeof(resp), 3000)) return;
    // +CIPSNTPTIME:Thu Jul 14 21:03:15 2026   (UTC because tz=0)
    char *p = strstr(resp, "+CIPSNTPTIME:");
    if (!p) return;
    p += 13;
    char mon[4] = {0}; int day, hh, mm, ss, year;
    if (sscanf(p, "%*3s %3s %d %d:%d:%d %d", mon, &day, &hh, &mm, &ss, &year) != 6) return;
    if (year < 2024) return;   // not synced yet (epoch/1970 placeholder)
    static const char *M = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *mp = strstr(M, mon);
    if (!mp) return;
    int m = (int)(mp - M) / 3 + 1;
    // days-from-civil (Howard Hinnant) -> UTC epoch
    int y = year - (m <= 2);
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    s_sntp_epoch = (uint32_t)(days * 86400L + hh * 3600 + mm * 60 + ss);
    s_sntp_at_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d UTC (epoch %lu)",
             year, m, day, hh, mm, ss, (unsigned long)s_sntp_epoch);
}

static void worker_refresh_status(void) {
    // Current AP: +CWJAP:"ssid","mac",chan,rssi,...   ERROR/No AP when disconnected.
    // A single failed/garbled poll must NOT flip s_connected: the response buffer can be
    // polluted by +IPD/CONNECT URC noise from active links, and the app reacts to one
    // false "disconnected" with WiFi.disconnect+begin — which silently kills the C6's
    // CIPSERVER. Only a "No AP" answer (genuinely not associated) clears immediately;
    // anything ambiguous needs 2 misses in a row.
    static uint8_t miss_streak = 0;
    static char resp[512];
    bool cmd_ok = c6at_command("AT+CWJAP?", resp, sizeof(resp), 3000);
    char *p = cmd_ok ? strstr(resp, "+CWJAP:\"") : NULL;
    if (!p) {
        bool no_ap = cmd_ok && strstr(resp, "No AP");
        if (no_ap || ++miss_streak >= 2) {
            s_connected = false; s_ip4 = 0;
            if (no_ap) s_cur_ssid[0] = '\0';
            miss_streak = 0;
        }
        return;
    }
    miss_streak = 0;
    p += 8;
    char *e = strchr(p, '"');
    if (e) {
        size_t l = (size_t)(e - p); if (l > 32) l = 32;
        memcpy(s_cur_ssid, p, l); s_cur_ssid[l] = '\0';
        // fields: "ssid","mac",chan,rssi — chan = after the 2nd comma, rssi = after the 3rd
        char *c = e;
        for (int skip = 0; c && skip < 2; skip++) c = strchr(c + 1, ',');
        if (c) { s_cur_chan = atoi(c + 1); c = strchr(c + 1, ','); }
        if (c) s_cur_rssi = atoi(c + 1);
    }
    // IP: +CIPSTA:ip:"a.b.c.d"
    if (c6at_command("AT+CIPSTA?", resp, sizeof(resp), 2000)) {
        char *ip = strstr(resp, "+CIPSTA:ip:\"");
        if (ip) {
            unsigned a, b, c2, d;
            if (sscanf(ip + 12, "%u.%u.%u.%u", &a, &b, &c2, &d) == 4) {
                uint32_t v = (a << 24) | (b << 16) | (c2 << 8) | d;
                s_ip4 = (v == 0) ? 0 : v;
                s_connected = (v != 0);
                return;
            }
        }
    }
    s_connected = true;   // associated (CWJAP answered) but no IP parsed yet
}

static void worker_do_scan(void) {
    s_scan_state = -1;
    static char resp[3072];
    if (!c6at_command("AT+CWLAP", resp, sizeof(resp), 10000)) { s_scan_n = 0; s_scan_state = 0; return; }
    // Lines: +CWLAP:(ecn,"ssid",rssi,"mac",channel,...)
    int count = 0;
    char *p = resp;
    while (count < SCAN_MAX && (p = strstr(p, "+CWLAP:(")) != NULL) {
        p += 8;
        int ecn = atoi(p);
        char *q = strchr(p, '"');   if (!q) break;
        q++;
        char *e = strchr(q, '"');   if (!e) break;
        size_t sl = (size_t)(e - q); if (sl > 32) sl = 32;
        memcpy(s_scan[count].ssid, q, sl); s_scan[count].ssid[sl] = '\0';
        char *r = strchr(e, ',');
        s_scan[count].rssi = r ? (int8_t)atoi(r + 1) : 0;
        s_scan[count].enc  = (uint8_t)ecn;
        // channel = 5th field: (ecn,"ssid",rssi,"mac",channel — two commas past the rssi one
        s_scan[count].chan = 0;
        if (r) { char *c = strchr(r + 1, ','); if (c) c = strchr(c + 1, ','); if (c) s_scan[count].chan = (uint8_t)atoi(c + 1); }
        if (s_scan[count].ssid[0]) count++;
        p = e + 1;
    }
    s_scan_n = count;
    s_scan_state = count;
    ESP_LOGI(TAG, "scan: %d APs", count);
}

static void worker_do_join(const char *ssid, const char *pass) {
    char cmd[192];
    // NB: no escaping of " , \ in credentials yet (rare in practice; TODO if it bites).
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"", ssid, pass ? pass : "");
    static char resp[512];
    bool ok = c6at_command(cmd, resp, sizeof(resp), 20000);
    if (ok || strstr(resp, "WIFI GOT IP")) {
        strncpy(s_cur_ssid, ssid, sizeof(s_cur_ssid) - 1);
        worker_refresh_status();
        ESP_LOGI(TAG, "join '%s': %s ip=%08lx", ssid, s_connected ? "CONNECTED" : "assoc,no-ip", (unsigned long)s_ip4);
    } else {
        s_connected = false; s_ip4 = 0;
        ESP_LOGW(TAG, "join '%s' FAILED", ssid);
    }
}

static void c6WorkerTask(void *arg) {
    (void)arg;
    // Bring the link up ourselves (the C6 may still be booting ESP-AT after the C6_EN pulse).
    vTaskDelay(pdMS_TO_TICKS(1500));
    bool up = false;
    for (int i = 0; i < 10 && !up; i++) {
        up = c6at_begin();
        if (!up) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!up) {
        s_dead = true;
        ESP_LOGE(TAG, "worker: C6 AT link never came up — Wi-Fi unavailable");
        // Keep servicing the queue as no-ops so requesters never wedge.
    }
    uint32_t last_refresh = 0;
    for (;;) {
        c6_req_t req;
        if (xQueueReceive(s_reqq, &req, pdMS_TO_TICKS(150)) == pdTRUE) {
            if (!s_up) {                       // link dead: fail requests fast
                if (req.type == REQ_SCAN) { s_scan_n = 0; s_scan_state = 0; }
                if (req.status) *req.status = REQ_FAIL;
                continue;
            }
            switch (req.type) {
                case REQ_SCAN:         worker_do_scan(); break;
                case REQ_JOIN:         worker_do_join(req.ssid, req.pass);
                                       if (s_connected) worker_sntp_service(true);
                                       // A (re)association drops any running CIPSERVER on the
                                       // C6 — re-arm the want-flag so the idle loop restarts it.
                                       if (s_srv_want) s_srv_up = false;
                                       break;
                case REQ_DISCONNECT:   c6at_command("AT+CWQAP", NULL, 0, 3000); s_connected = false; s_ip4 = 0; break;
                case REQ_SOCK_CONNECT: worker_sock_connect(&req); break;
                case REQ_SOCK_SEND:    worker_sock_send(&req); break;
                case REQ_SOCK_PULL:    worker_sock_pull(req.id); break;
                case REQ_SOCK_CLOSE: {
                    if (s_sock[req.id].open) {   // peer-closed links ("<id>,CLOSED" seen) error on CIPCLOSE
                        char cmd[24];
                        snprintf(cmd, sizeof(cmd), "AT+CIPCLOSE=%d", req.id);
                        c6at_command(cmd, NULL, 0, 3000);
                        s_sock[req.id].open = false;
                    }
                    if (req.status) *req.status = REQ_OK;
                    break;
                }
                case REQ_SERVER_START:   // legacy kick — the want-flag path below does the work
                    if (req.status) *req.status = REQ_OK;
                    break;
                case REQ_SERVER_STOP:
                    c6at_command("AT+CIPSERVER=0", NULL, 0, 5000);
                    s_srv_up = false;
                    if (req.status) *req.status = REQ_OK;
                    break;
            }
            last_refresh = (uint32_t)(esp_timer_get_time() / 1000);
        } else if (s_up) {
            // Idle housekeeping:
            // 1. service links with data waiting on the C6 (+IPD seen) — pull into their rings.
            bool pulled = false;
            for (int id = 0; id < SOCK_MAX; id++) {
                sock_t *sk = &s_sock[id];
                if (sk->used && sk->rx_notified && sk->ring && ring_free(sk) > 128) {
                    worker_sock_pull(id);
                    pulled = true;
                    break;   // one per idle slice keeps the loop responsive to new requests
                }
            }
            // 2. drain any stray unsolicited lines so +IPD/CLOSED aren't stuck in the C6's queue.
            if (!pulled) {
                size_t got = 0;
                essl_get_packet(s_essl, s_rx, C6_RECV_BUF, &got, 20);
                if (got) { s_rx[got < C6_RECV_BUF ? got : C6_RECV_BUF - 1] = 0; urc_scan((char *)s_rx, got); }
            }
            // 3. bring the inbound listener up when asked (want-flag; also self-heals a request
            // made before this worker existed — the TCP companion latches its own started flag).
            static uint32_t srv_next_try = 0;
            if (s_srv_want && !s_srv_up && s_srv_port &&
                (uint32_t)(esp_timer_get_time() / 1000) >= srv_next_try) {
                c6at_command("AT+CIPSERVERMAXCONN=3", NULL, 0, 2000);   // phone + web viewer + one transient page GET; the rest stay outbound (tiles/HTTP)
                char cmd[32];
                snprintf(cmd, sizeof(cmd), "AT+CIPSERVER=1,%u", (unsigned)s_srv_port);
                bool ok = c6at_command(cmd, NULL, 0, 5000);
                s_srv_up = ok;
                ESP_LOGI(TAG, "CIPSERVER port %u -> %s", (unsigned)s_srv_port, ok ? "LISTENING" : "FAIL (retry in 12s)");
                if (!ok) srv_next_try = (uint32_t)(esp_timer_get_time() / 1000) + 12000;
            }
            // 4. refresh association/IP every ~12 s so the UI stays truthful, + SNTP until synced.
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (now - last_refresh > 12000) {
                worker_refresh_status();
                // Self-heal the inbound listener: the C6 drops CIPSERVER on re-association
                // (or an app-side rejoin) without any URC, leaving s_srv_up stale-true and
                // the port refusing connections until reboot. Ask, and re-arm on mismatch.
                if (s_srv_want && s_srv_up && s_connected) {
                    static char sresp[96];
                    if (c6at_command("AT+CIPSERVER?", sresp, sizeof(sresp), 2000)) {
                        char *sp = strstr(sresp, "+CIPSERVER:");
                        if (sp && sp[11] == '0') {
                            s_srv_up = false;
                            ESP_LOGW(TAG, "CIPSERVER dropped by C6 — re-arming");
                        }
                    }
                }
                if (s_connected) worker_sntp_service(!s_sntp_cfged);
                last_refresh = now;
            }
        }
    }
}

// ---- public async API -----------------------------------------------------------------------------

void c6at_worker_start(void) {
    if (s_worker) return;
    if (!s_reqq) s_reqq = xQueueCreate(6, sizeof(c6_req_t));
    xTaskCreatePinnedToCore(c6WorkerTask, "c6at", 16384, NULL, 3, &s_worker, 1);
}

void c6at_req_scan(void) {
    if (s_dead) { s_scan_n = 0; s_scan_state = 0; return; }
    s_scan_state = -1;                       // running (set now so a poll right after sees RUNNING)
    c6_req_t r = { .type = REQ_SCAN };
    if (s_reqq) xQueueSend(s_reqq, &r, 0);
}

void c6at_req_join(const char *ssid, const char *pass) {
    if (s_dead || !ssid) return;
    c6_req_t r = { .type = REQ_JOIN };
    strncpy(r.ssid, ssid, sizeof(r.ssid) - 1);
    if (pass) strncpy(r.pass, pass, sizeof(r.pass) - 1);
    if (s_reqq) xQueueSend(s_reqq, &r, 0);
}

void c6at_req_disconnect(void) {
    if (s_dead) return;
    c6_req_t r = { .type = REQ_DISCONNECT };
    if (s_reqq) xQueueSend(s_reqq, &r, 0);
}

bool     c6at_connected(void)        { return s_connected; }
uint32_t c6at_ip4(void)              { return s_ip4; }
int      c6at_current_rssi(void)     { return s_cur_rssi; }
int      c6at_current_channel(void)  { return s_cur_chan; }
int      c6at_scan_state(void)       { return s_scan_state; }
int      c6at_scan_channel(int idx)  { return (idx < 0 || idx >= s_scan_n) ? 0 : s_scan[idx].chan; }

bool c6at_current_ssid(char *out, size_t sz) {
    if (!out || sz == 0) return false;
    strncpy(out, s_cur_ssid, sz - 1); out[sz - 1] = '\0';
    return out[0] != '\0';
}

bool c6at_scan_get(int idx, char *ssid, size_t sz, int8_t *rssi, uint8_t *enc) {
    if (idx < 0 || idx >= s_scan_n) return false;
    if (ssid) { strncpy(ssid, s_scan[idx].ssid, sz - 1); ssid[sz - 1] = '\0'; }
    if (rssi) *rssi = s_scan[idx].rssi;
    if (enc)  *enc  = s_scan[idx].enc;
    return true;
}

void c6at_scan_clear(void)          { s_scan_n = 0; s_scan_state = -2; }
void c6at_set_sta_enabled(bool en)  { s_sta_en = en; }
bool c6at_sta_enabled(void)         { return s_sta_en; }

// ---- public socket API -----------------------------------------------------------------------------
// Blocking bridge: the caller polls a stack-resident status until the worker writes it. NO caller-side
// timeout bail — the worker's own AT timeouts bound every op, and bailing early would leave the worker
// holding a dangling pointer onto our stack. (Dead-link requests are failed fast by the worker.)

static uint32_t s_last_kick[SOCK_MAX];

static void kick_pull(int id) {
    sock_t *s = &s_sock[id];
    if (s->pull_inflight || !s_reqq) return;
    s->pull_inflight = true;
    c6_req_t r = { .type = REQ_SOCK_PULL, .id = id };
    if (xQueueSend(s_reqq, &r, 0) != pdTRUE) s->pull_inflight = false;
    else s_last_kick[id] = (uint32_t)(esp_timer_get_time() / 1000);
}

int c6at_sock_connect(const char *host, uint16_t port, bool ssl, uint32_t timeout_ms) {
    (void)timeout_ms;   // worker-side CIPSTART timeout (15 s) governs
    if (s_dead || !s_up || !host || !s_reqq) return -1;
    int id = -1;
    // Top-down: ESP-AT hands INBOUND server connections the lowest free id, so outbound stays
    // high to avoid racing an arriving phone-app connection for the same link number.
    for (int i = SOCK_MAX - 1; i >= 0; i--) if (!s_sock[i].used) { s_sock[i].used = true; id = i; break; }
    if (id < 0) return -1;
    c6_req_t r = { .type = REQ_SOCK_CONNECT, .port = port, .ssl = ssl, .id = id };
    strncpy(r.host, host, sizeof(r.host) - 1);
    volatile int st = REQ_PENDING; r.status = &st;
    if (xQueueSend(s_reqq, &r, pdMS_TO_TICKS(250)) != pdTRUE) { s_sock[id].used = false; return -1; }
    while (st == REQ_PENDING) vTaskDelay(pdMS_TO_TICKS(5));
    if (st != REQ_OK) { s_sock[id].used = false; return -1; }
    return id;
}

bool c6at_sock_send(int id, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (id < 0 || id >= SOCK_MAX || !data || !len || !s_reqq) return false;
    if (!s_sock[id].used || !s_sock[id].open) return false;
    c6_req_t r = { .type = REQ_SOCK_SEND, .id = id, .data = data, .len = len };
    volatile int st = REQ_PENDING; r.status = &st;
    if (xQueueSend(s_reqq, &r, pdMS_TO_TICKS(250)) != pdTRUE) return false;
    while (st == REQ_PENDING) vTaskDelay(pdMS_TO_TICKS(5));
    return st == REQ_OK;
}

int c6at_sock_available(int id) {
    if (id < 0 || id >= SOCK_MAX) return 0;
    sock_t *s = &s_sock[id];
    uint32_t f = ring_fill(s);
    if (f == 0) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        // Pull on notification immediately; while open, also probe every 200 ms (an +IPD line can be
        // missed if it split across drained chunks — the probe is a cheap 0-or-data CIPRECVDATA).
        if (s->rx_notified || (s->open && now - s_last_kick[id] > 200)) kick_pull(id);
    }
    return (int)f;
}

int c6at_sock_read(int id, uint8_t *dst, size_t maxlen) {
    if (id < 0 || id >= SOCK_MAX || !dst || !maxlen) return -1;
    sock_t *s = &s_sock[id];
    uint32_t f = ring_fill(s);
    if (f == 0) { (void)c6at_sock_available(id); return 0; }
    uint32_t n = f < maxlen ? f : (uint32_t)maxlen;
    for (uint32_t i = 0; i < n; i++) dst[i] = s->ring[(s->r + i) % SOCK_RING];
    s->r += n;
    if (s->rx_notified) kick_pull(id);   // room freed — keep the pipeline moving
    return (int)n;
}

int c6at_sock_peek(int id) {
    if (id < 0 || id >= SOCK_MAX) return -1;
    sock_t *s = &s_sock[id];
    if (ring_fill(s) == 0) { (void)c6at_sock_available(id); return -1; }
    return (int)s->ring[s->r % SOCK_RING];
}

bool c6at_sock_connected(int id) {
    if (id < 0 || id >= SOCK_MAX || !s_sock[id].used) return false;
    sock_t *s = &s_sock[id];
    return s->open || s->rx_notified || ring_fill(s) > 0;   // closed-with-buffered-data still reads
}

void c6at_sock_close(int id) {
    if (id < 0 || id >= SOCK_MAX || !s_sock[id].used) return;
    if (s_reqq && !s_dead) {
        c6_req_t r = { .type = REQ_SOCK_CLOSE, .id = id };
        volatile int st = REQ_PENDING; r.status = &st;
        if (xQueueSend(s_reqq, &r, pdMS_TO_TICKS(250)) == pdTRUE)
            while (st == REQ_PENDING) vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_sock[id].open = false; s_sock[id].rx_notified = false;
    s_sock[id].r = s_sock[id].w = 0;
    s_sock[id].used = false;
}

bool c6at_server_start(uint16_t port) {
    if (!port) return false;
    s_srv_port = port;
    s_srv_want = true;    // the worker's idle housekeeping starts the listener (works even when
                          // called before c6at_worker_start — the wish survives until it can run)
    return !s_dead;
}

void c6at_server_stop(void) {
    s_srv_want = false;
    if (!s_reqq || s_dead || !s_srv_up) return;
    c6_req_t r = { .type = REQ_SERVER_STOP };
    xQueueSend(s_reqq, &r, 0);
}

int c6at_server_pending(void) { return (int)(uint8_t)(s_acc_w - s_acc_r); }

int c6at_server_accept(void) {
    if (s_acc_r == s_acc_w) return -1;
    int id = s_acc_q[s_acc_r % 8];
    s_acc_r++;
    return id;
}

uint32_t c6at_sntp_epoch(void) {
    if (!s_sntp_epoch) return 0;
    return s_sntp_epoch + ((uint32_t)(esp_timer_get_time() / 1000) - s_sntp_at_ms) / 1000;
}
