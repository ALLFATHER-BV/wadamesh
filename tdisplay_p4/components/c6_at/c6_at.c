// SPDX-License-Identifier: GPL-3.0-or-later
// c6_at — AT-over-SDIO client for the T-Display P4's ESP32-C6 (ESP-AT firmware). See c6_at.h.
// SDIO framing follows LilyGo's GPL-3.0 EspAt transport at pinned commit
// ff99314e37f125fe9f18b41340639f000c2f1e83. The AT worker and Wi-Fi/socket
// layers on top remain WadaMesh-owned.
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
#include "esp_heap_caps.h"
#include "c6_link_health.h"

static const char *TAG = "c6_at";

// Board-owned XL9535 hook. LilyGo's factory EspAt transport resets the C6
// immediately before SDIO initialization; the early board power sequence is
// too far ahead of this worker to provide the same startup contract.
extern bool tdisplay_p4_reset_c6(void);

// ---- C6 SDIO wiring (T-Display P4, "SDIO_2" — slot 1) ----
#define C6_SDIO_SLOT   1
#define C6_SDIO_CLK    18
#define C6_SDIO_CMD    19
#define C6_SDIO_D0     14
#define C6_SDIO_D1     15
#define C6_SDIO_D2     16
#define C6_SDIO_D3     17
// Worker scratch buffers. The ESP-AT ring itself is larger; reads drain it in
// bounded chunks through the slave's packet-length register.
#define C6_RECV_BUF    2048
#define C6_TX_CAP      2048          // AT lines AND raw socket payload chunks go out through s_tx
// SDIO clock: conservative for reliable bring-up; ESP-AT SDIO is happy at 20 MHz.
#define C6_SDIO_KHZ    SDMMC_FREQ_DEFAULT   // 20 MHz

static sdmmc_card_t *s_card  = NULL;
static bool          s_transport_up = false;
static bool          s_ready_checked = false;
static uint32_t      s_rx_total_index = 0;
static volatile bool s_up    = false;
static volatile bool s_dead  = false;
// SDIO transfers DMA straight from/into these buffers: they must be DMA-capable internal RAM,
// and on the P4 (cached internal RAM) the addresses must be cache-line aligned.
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
typedef enum { REQ_SCAN = 1,
               REQ_SOCK_CONNECT, REQ_SOCK_SEND, REQ_SOCK_PULL, REQ_SOCK_CLOSE,
               REQ_SERVER_START, REQ_SERVER_STOP } req_type_t;
// Blocking bridge: the caller stack-allocates `status` (REQ_PENDING) and polls it; the worker writes
// the outcome. Callers block in their own tasks (tile fetcher / reader / version check), never LVGL.
#define REQ_PENDING 0
#define REQ_OK      1
#define REQ_FAIL    2
typedef struct {
    req_type_t type;
    char host[96]; uint16_t port; bool ssl;    // SOCK_CONNECT
    int  id;                                   // SOCK_*
    const uint8_t *data; size_t len;           // SOCK_SEND (caller's buffer; caller blocks until done)
    volatile int *status;                      // REQ_* result (may be NULL for fire-and-forget)
} c6_req_t;
static QueueHandle_t s_reqq = NULL;
static TaskHandle_t  s_worker = NULL;

// ---- association slot (latest wins) ---------------------------------------------------------------
// Join/disconnect do NOT go through the request queue: a 6-deep FIFO filled up when main.cpp's 10 s
// retry fired disconnect+join pairs behind a slow CWJAP, the user's own Connect was then dropped on
// a full queue, and a queued CWQAP that ran right after a successful join disconnected again. One
// slot instead: a join overwrites any pending op, a disconnect clears a pending join. The worker
// takes the slot atomically and marks the join running in the same critical section, so
// c6at_join_busy() never sees a gap between "pending" and "running".
typedef enum { ASSOC_NONE = 0, ASSOC_JOIN, ASSOC_LEAVE } assoc_op_t;
static portMUX_TYPE   s_assoc_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile assoc_op_t s_assoc_op = ASSOC_NONE;
static char           s_assoc_ssid[33], s_assoc_pass[65];
static volatile bool  s_join_running = false;
static volatile bool  s_want_assoc   = false;   // last request was a join (not a disconnect)
static char           s_last_ssid[33], s_last_pass[65];   // worker-only: last join taken, for rejoin after a reset

// ---- link health (worker-only; see c6_link_health.h) --------------------------------------------
// A C6 whose ESP-AT stack wedges mid-session (#685: scans empty, Connect dead after about an hour,
// only a reboot helped) used to stay dead: s_up was set once and every command just timed out.
// The worker now counts command timeouts, takes the link down on a stall, resets the C6 through
// the same path the boot uses (with backoff), then rejoins and re-arms the listener.
static c6h_t          s_health;
static bool           s_health_armed = false;   // count outcomes only while the worker runs an up link
static c6h_act_t      s_health_act   = C6H_ACT_NONE;
static volatile bool  s_recovering   = false;   // link down after a runtime stall, reset pending/running

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void health_note(c6h_res_t r, c6h_kind_t k) {
    if (!s_health_armed) return;
    c6h_act_t a = c6h_note(&s_health, r, k, now_ms());
    if (a > s_health_act) s_health_act = a;
}

// ---- transport ---------------------------------------------------------------------------------

#define C6_SDIO_FN_ENABLE       0x002
#define C6_SDIO_FN_READY        0x003
#define C6_SDIO_INT_ENABLE      0x004
#define C6_SDIO_BLOCK_SIZE_LO   0x010
#define C6_SDIO_BLOCK_SIZE_HI   0x011
#define C6_SDIO_FN1_BLOCK_LO    0x110
#define C6_SDIO_FN1_BLOCK_HI    0x111
#define C6_SDIO_FN2_BLOCK_LO    0x210
#define C6_AT_TOKEN_RDATA       0x044
#define C6_AT_PACKET_LENGTH     0x060
#define C6_AT_INTERRUPT_RAW     0x050
#define C6_AT_INTERRUPT_CLEAR   0x0D4
#define C6_AT_RX_NEW_PACKET     (1UL << 23)
#define C6_AT_CMD53_END_ADDR    0x1F800
#define C6_AT_BLOCK_SIZE        512
#define C6_AT_TX_BLOCK_OFFSET   16
#define C6_AT_TX_BLOCK_MASK     0x0FFF
#define C6_AT_RX_LENGTH_MASK    0x0FFFFF
#define C6_AT_RX_LENGTH_WRAP    0x100000

static bool sdio_write_byte(uint32_t function, uint32_t address, uint8_t value) {
    return s_card && sdmmc_io_write_byte(s_card, function, address, value, NULL) == ESP_OK;
}

static bool sdio_read_u32(uint32_t address, uint32_t *value) {
    return s_card && value &&
           sdmmc_io_read_bytes(s_card, 1, address, value, sizeof(*value)) == ESP_OK;
}

static bool sdio_write_u32(uint32_t address, uint32_t value) {
    return s_card &&
           sdmmc_io_write_bytes(s_card, 1, address, &value, sizeof(value)) == ESP_OK;
}

static bool transport_configure(void) {
    // Match LilyGo's EspAt CCCR/function setup: functions 1+2, master IRQ, and
    // 512-byte block sizes. WadaMesh currently exchanges data on function 1.
    if (!sdio_write_byte(0, C6_SDIO_FN_ENABLE, 6) ||
        !sdio_write_byte(0, C6_SDIO_FN_READY, 6) ||
        !sdio_write_byte(0, C6_SDIO_INT_ENABLE, 7) ||
        !sdio_write_byte(0, C6_SDIO_BLOCK_SIZE_LO, 0) ||
        !sdio_write_byte(0, C6_SDIO_BLOCK_SIZE_HI, 2) ||
        !sdio_write_byte(0, C6_SDIO_FN1_BLOCK_LO, 0) ||
        !sdio_write_byte(0, C6_SDIO_FN1_BLOCK_HI, 2) ||
        !sdio_write_byte(0, C6_SDIO_FN2_BLOCK_LO, 0) ||
        // The 2025 factory driver deliberately writes 0x210 twice; retain that
        // quirk for the C6 image shipped on early P4 units.
        !sdio_write_byte(0, C6_SDIO_FN2_BLOCK_LO, 2)) {
        ESP_LOGE(TAG, "ESP-AT SDIO function configuration failed");
        return false;
    }
    s_rx_total_index = 0;
    s_ready_checked = false;
    s_transport_up = true;
    return true;
}

static uint32_t transport_rx_available(void) {
    uint32_t total = 0;
    if (!s_transport_up || !sdio_read_u32(C6_AT_PACKET_LENGTH, &total)) return 0;
    total &= C6_AT_RX_LENGTH_MASK;
    return (total + C6_AT_RX_LENGTH_WRAP - s_rx_total_index) % C6_AT_RX_LENGTH_WRAP;
}

static bool transport_receive_packet(uint8_t *data, size_t capacity, size_t *received) {
    if (received) *received = 0;
    if (!data || !received || capacity == 0 || !s_transport_up) return false;

    uint32_t flags = 0;
    if (sdio_read_u32(C6_AT_INTERRUPT_RAW, &flags) && (flags & C6_AT_RX_NEW_PACKET))
        (void)sdio_write_u32(C6_AT_INTERRUPT_CLEAR, flags);

    uint32_t available = transport_rx_available();
    if (available == 0) return true;
    size_t length = available < capacity ? (size_t)available : capacity;
    const size_t block_length = (length / C6_AT_BLOCK_SIZE) * C6_AT_BLOCK_SIZE;
    uint32_t remaining = available;
    if (block_length) {
        esp_err_t err = sdmmc_io_read_blocks(
            s_card, 1, C6_AT_CMD53_END_ADDR - remaining, data, block_length);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ESP-AT block read: %s", esp_err_to_name(err));
            return false;
        }
        remaining -= block_length;
        s_rx_total_index = (s_rx_total_index + block_length) & C6_AT_RX_LENGTH_MASK;
    }
    const size_t tail_length = length - block_length;
    if (tail_length) {
        uint8_t tail[C6_AT_BLOCK_SIZE] __attribute__((aligned(4))) = {0};
        const size_t aligned = (tail_length + 3) & ~(size_t)3;
        esp_err_t err = sdmmc_io_read_bytes(
            s_card, 1, C6_AT_CMD53_END_ADDR - remaining, tail, aligned);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ESP-AT tail read: %s", esp_err_to_name(err));
            return false;
        }
        memcpy(data + block_length, tail, tail_length);
        s_rx_total_index = (s_rx_total_index + tail_length) & C6_AT_RX_LENGTH_MASK;
    }
    *received = length;
    return true;
}

static bool transport_send_packet(const uint8_t *data, size_t length, uint32_t timeout_ms) {
    if (!data || length == 0 || length > C6_AT_CMD53_END_ADDR || !s_transport_up)
        return false;
    const uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + timeout_ms;
    for (;;) {
        uint32_t token = 0;
        if (!sdio_read_u32(C6_AT_TOKEN_RDATA, &token)) return false;
        if (((token >> C6_AT_TX_BLOCK_OFFSET) & C6_AT_TX_BLOCK_MASK) * C6_AT_BLOCK_SIZE >= length)
            break;
        if ((int32_t)((uint32_t)(esp_timer_get_time() / 1000) - deadline) >= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    const size_t block_length = (length / C6_AT_BLOCK_SIZE) * C6_AT_BLOCK_SIZE;
    if (block_length) {
        esp_err_t err = sdmmc_io_write_blocks(
            s_card, 1, C6_AT_CMD53_END_ADDR - length, data, block_length);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ESP-AT block write: %s", esp_err_to_name(err));
            return false;
        }
    }
    const size_t tail_length = length - block_length;
    if (tail_length) {
        uint8_t tail[C6_AT_BLOCK_SIZE] __attribute__((aligned(4))) = {0};
        memcpy(tail, data + block_length, tail_length);
        const size_t aligned = (tail_length + 3) & ~(size_t)3;
        esp_err_t err = sdmmc_io_write_bytes(
            s_card, 1, C6_AT_CMD53_END_ADDR - tail_length, tail, aligned);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ESP-AT tail write: %s", esp_err_to_name(err));
            return false;
        }
    }
    return true;
}

static void sdio_bring_down(void) {
    s_transport_up = false;
    s_ready_checked = false;
    s_rx_total_index = 0;
    if (s_card) {
        free(s_card);
        s_card = NULL;
    }
    // Slot 0 belongs to the SD card; release only the C6's dedicated slot 1.
    (void)sdmmc_host_deinit_slot(C6_SDIO_SLOT);
}

static bool sdio_bring_up(void) {
    if (s_transport_up) return true;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = C6_SDIO_SLOT;
    // Let SDMMC bounce small control-register reads through aligned storage when needed.
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
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_host_init_slot: %s", esp_err_to_name(err));
        sdio_bring_down();
        return false;
    }

    s_card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (!s_card) { sdio_bring_down(); return false; }
    // Match LilyGo's HardwareSdio: C6 can still be leaving reset while the
    // host slot is ready, so retry card identification before failing.
    for (int attempt = 0; attempt < 30; ++attempt) {
        err = sdmmc_card_init(&host, s_card);
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_card_init (C6 not on SDIO?): %s", esp_err_to_name(err));
        sdio_bring_down();
        return false;
    }
    err = sdmmc_io_enable_int(s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdmmc_io_enable_int: %s", esp_err_to_name(err));
        sdio_bring_down();
        return false;
    }
    if (!transport_configure()) { sdio_bring_down(); return false; }
    ESP_LOGI(TAG, "SDIO card up (ESP-AT register transport)");
    return true;
}

// Drain pending RX packets from the C6 into `out` (up to sz-1), NUL-terminate. Returns bytes read.
// Poll the ESP-AT receive-length register; echo and OK may arrive separately, so
// keep draining until two empty polls after data.
static size_t drain_rx(char *out, size_t sz, uint32_t deadline_ms) {
    size_t n = 0;
    int empty = 0;
    while ((uint32_t)(esp_timer_get_time() / 1000) < deadline_ms) {
        size_t got = 0;
        if (!transport_receive_packet(s_rx, C6_RECV_BUF, &got)) break;
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

// One AT exchange. Result is tri-state (+busy) so the link-health counter can tell "the C6
// answered ERROR" (alive) from "nothing came back" (possibly wedged).
static c6h_res_t at_exec(const char *cmd, char *resp, size_t resp_sz, uint32_t timeout_ms,
                         c6h_kind_t kind) {
    if (resp && resp_sz) resp[0] = '\0';
    if (!s_transport_up || !s_tx) return C6H_RES_TIMEOUT;
    // A stall is already decided: fail the rest of the current request at once instead of
    // spending another full timeout per command before the worker gets to the reset.
    if (s_health_armed && s_health_act == C6H_ACT_STALL) return C6H_RES_TIMEOUT;
    int len = snprintf((char *)s_tx, C6_TX_CAP, "%s\r\n", cmd);
    if (len <= 0 || len >= C6_TX_CAP) return C6H_RES_ERR;
    if (!transport_send_packet(s_tx, (size_t)len, 1000)) {
        ESP_LOGW(TAG, "send '%s' failed", cmd);
        health_note(C6H_RES_TIMEOUT, kind);   // the C6 is not taking data: same as no answer
        return C6H_RES_TIMEOUT;
    }

    // Collect the reply until a terminating status line appears. Static: only the worker calls this.
    static char buf[3584]; size_t n = 0; buf[0] = '\0';
    uint32_t deadline = now_ms() + timeout_ms;
    c6h_res_t res = C6H_RES_TIMEOUT;
    bool done = false, truncated = false;
    while (!done && now_ms() < deadline) {
        static char chunk[C6_RECV_BUF];
        size_t got = drain_rx(chunk, sizeof(chunk), deadline);
        bool chunk_ok = false, chunk_err = false, chunk_busy = false;
        if (got) {
            // A dense CWLAP response can fill `buf` before ESP-AT sends its
            // terminal status packet. Always inspect the fresh chunk too, or
            // the discarded trailing OK turns a successful scan into TIMEOUT.
            chunk_ok = strstr(chunk, "\r\nOK\r\n") || (got >= 4 && !strncmp(chunk, "OK\r\n", 4));
            chunk_err = strstr(chunk, "\r\nERROR\r\n") || strstr(chunk, "\r\nFAIL\r\n");
            // "busy p..." / "busy s...": ESP-AT rejected the line because it is still stuck on
            // something else. No OK/ERROR will follow, so stop waiting for one.
            chunk_busy = strstr(chunk, "busy p") || strstr(chunk, "busy s");
            size_t cp = (n + got < sizeof(buf) - 1) ? got : (sizeof(buf) - 1 - n);
            memcpy(buf + n, chunk, cp); n += cp; buf[n] = '\0';
            if (cp < got) truncated = true;
        } else {
            vTaskDelay(pdMS_TO_TICKS(2));   // SDIO read error: don't spin the core until the deadline
        }
        if (chunk_ok || strstr(buf, "\r\nOK\r\n") || (n >= 4 && !strncmp(buf, "OK\r\n", 4))) {
            res = C6H_RES_OK; done = true;
        } else if (chunk_err || strstr(buf, "\r\nERROR\r\n") || strstr(buf, "\r\nFAIL\r\n")) {
            res = C6H_RES_ERR; done = true;
        } else if (chunk_busy) {
            res = C6H_RES_BUSY; done = true;
        }
    }
    urc_scan(buf, n);   // "+IPD"/"<id>,CLOSED" lines interleave with command output — never miss them
    if (resp && resp_sz) { strncpy(resp, buf, resp_sz - 1); resp[resp_sz - 1] = '\0'; }
    static const char *const RES_NAME[] = { "OK", "ERR", "TIMEOUT", "BUSY" };
    ESP_LOGI(TAG, "AT '%s' -> %s (%u bytes%s)", cmd, RES_NAME[res], (unsigned)n,
             truncated ? ", truncated" : "");
    health_note(res, kind);
    return res;
}

bool c6at_command(const char *cmd, char *resp, size_t resp_sz, uint32_t timeout_ms) {
    return at_exec(cmd, resp, resp_sz, timeout_ms, C6H_KIND_CMD) == C6H_RES_OK;
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
    if (!transport_send_packet(s_tx, (size_t)clen, 1000)) return false;
    if (!collect_until(">", acc, sizeof(acc), 3000)) { printf("[nus] cfg%d no prompt: %s\n", idx, acc); return false; }
    // Data phase: EXACT length first — pad bytes after the counted data desync the SYSMFG reader
    // (unlike CIPSEND, which tolerates them). ALLOC_ALIGNED_BUF bounces small unaligned DMA sends;
    // fall back to '\n' padding only if the raw send is rejected by the host driver.
    int dl = snprintf((char *)s_tx, C6_TX_CAP, "%s", row);
    if (!transport_send_packet(s_tx, (size_t)dl, 1000)) return false;
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
    if (!s_transport_up && !sdio_bring_up()) return false;

    if (!s_ready_checked) {
        static char ready[256];
        const uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + 2000;
        const size_t ready_n = drain_rx(ready, sizeof(ready), deadline);
        uint32_t token = 0, rx_total = 0, irq = 0;
        const bool token_ok = sdio_read_u32(C6_AT_TOKEN_RDATA, &token);
        const bool length_ok = sdio_read_u32(C6_AT_PACKET_LENGTH, &rx_total);
        const bool irq_ok = sdio_read_u32(C6_AT_INTERRUPT_RAW, &irq);
        ESP_LOGI(TAG,
                 "ESP-AT startup: ready=%s bytes=%u token=%s%lu rxlen=%s%lu irq=%s0x%08lx",
                 strstr(ready, "ready") ? "yes" : "no", (unsigned)ready_n,
                 token_ok ? "" : "ERR/", (unsigned long)token,
                 length_ok ? "" : "ERR/", (unsigned long)(rx_total & C6_AT_RX_LENGTH_MASK),
                 irq_ok ? "" : "ERR/", (unsigned long)irq);
        s_ready_checked = true;
    }

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
    // Modem sleep off (#685). The C6 runs off the P4's supply, so the saving is small, and a
    // sleeping station that misses beacons is the likeliest way for an old AT build to drift off
    // its AP. Re-sent after every runtime reset, since c6at_begin is the shared bring-up path.
    if (!c6at_command("AT+SLEEP=0", NULL, 0, 1000))
        printf("[C6AT] AT+SLEEP=0 not accepted by this AT build (ignored)\n");
    {   // Let the C6 rejoin a lost AP by itself (every 5 s, unlimited). Query first: the setting is
        // stored in the C6's flash, so only write it when it differs. ERROR on the query = an AT
        // build without the command; the P4-side retry in main.cpp still covers reconnects.
        char r[96];
        if (!c6at_command("AT+CWRECONNCFG?", r, sizeof(r), 1000)) {
            printf("[C6AT] AT+CWRECONNCFG unsupported on this AT build (ignored)\n");
        } else {
            const char *q = strstr(r, "+CWRECONNCFG:");
            int iv = -1, cnt = -1;
            if (q) sscanf(q + 13, "%d,%d", &iv, &cnt);
            if (iv == 5 && cnt == 0) {
                printf("[C6AT] C6 auto-reconnect already on (5 s, unlimited)\n");
            } else {
                bool ok = c6at_command("AT+CWRECONNCFG=5,0", NULL, 0, 1000);
                printf("[C6AT] C6 auto-reconnect 5 s, unlimited -> %s (was %d,%d)\n",
                       ok ? "OK" : "ERROR (ignored)", iv, cnt);
            }
        }
    }
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
        if (!transport_receive_packet(s_rx, C6_RECV_BUF, &got)) return false;
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
    // SOFT: a connect can time out because the far host is dead, not the C6. It only asks for a
    // liveness ping and never counts toward the stall streak (tile fetches hit dead hosts).
    bool ok = at_exec(cmd, NULL, 0, 15000, C6H_KIND_SOFT) == C6H_RES_OK;
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
        if (!transport_send_packet(s_tx, (size_t)clen, 1000)) { ok = false; break; }
        if (!collect_until(">", acc, sizeof(acc), 3000)) {
            // No prompt: either an ERROR (link closed) or a C6 that stopped answering. Ask for a
            // ping so a dead parser is caught even while only sends are flowing.
            if (!strstr(acc, "ERROR")) health_note(C6H_RES_TIMEOUT, C6H_KIND_SOFT);
            ok = false; break;
        }
        // Raw payload: exactly `chunk` bytes, as promised by CIPSEND.
        memcpy(s_tx, p, chunk);
        if (!transport_send_packet(s_tx, chunk, 2000)) { ok = false; break; }
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
    if (!transport_send_packet(s_tx, (size_t)clen, 1000)) return;

    static uint8_t acc[SOCK_PULL_MAX + 512];
    size_t n = 0;
    uint32_t deadline = (uint32_t)(esp_timer_get_time() / 1000) + 3000;
    int hdr_at = -1, data_len = -1; size_t data_off = 0;
    while ((uint32_t)(esp_timer_get_time() / 1000) < deadline) {
        size_t got = 0;
        if (!transport_receive_packet(s_rx, C6_RECV_BUF, &got)) break;
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

// Bring the AT link up, rebuilding the SDIO slot + resetting the C6 every third failed attempt.
// Shared by the boot path (10 attempts) and the runtime recovery path (3 attempts).
static bool link_bringup(int attempts) {
    for (int i = 0; i < attempts; i++) {
        if (c6at_begin()) return true;
        // Reuse a successfully initialized slot while the C6 is merely
        // booting, but periodically rebuild a poisoned/half-open SDIO link.
        if (i % 3 == 2 && i + 1 < attempts) {
            ESP_LOGW(TAG, "worker: rebuilding C6 SDIO link after failed AT probes");
            sdio_bring_down();
            if (!tdisplay_p4_reset_c6()) ESP_LOGE(TAG, "worker: C6 reset hook failed");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

// Fail one request without touching the bus (link down).
static void req_fail_fast(const c6_req_t *req) {
    if (req->type == REQ_SCAN) { s_scan_n = 0; s_scan_state = 0; }
    if (req->type == REQ_SOCK_PULL && req->id >= 0 && req->id < SOCK_MAX)
        s_sock[req->id].pull_inflight = false;   // else the link could never be kicked again
    if (req->status) *req->status = REQ_FAIL;
}

// A stall was decided: take the link down NOW (every caller fails fast from here on), forget all
// state that lived on the C6, and leave the reset to worker_link_recover() once the backoff allows.
static void worker_link_stalled(void) {
    const uint32_t now = now_ms();
    s_health_armed = false;
    s_health_act   = C6H_ACT_NONE;
    s_up           = false;
    s_recovering   = true;
    if (c6h_reset_due(&s_health, now))
        printf("[C6AT] link stalled: resetting the C6\n");
    else
        printf("[C6AT] link stalled: resetting the C6 in %lu s (backoff)\n",
               (unsigned long)((s_health.next_reset_ms - now) / 1000));
    // Association, IP, listener and SNTP config all lived on the C6 and die with the reset. The
    // listener WANT flag (s_srv_want/s_srv_port) is kept: the idle loop re-arms CIPSERVER once the
    // link is back, and the router's C6Server stays begun, so nothing app-side has to restart it.
    s_connected = false; s_ip4 = 0; s_cur_rssi = 0;
    s_srv_up = false;
    s_sntp_cfged = false;
    if (s_scan_state == -1) { s_scan_n = 0; s_scan_state = 0; }   // a scan in flight ends empty
    // Links: mark closed so every C6Client reads connected()==false (once its ring drains) and the
    // TCP/WS companion servers prune it; the owner's stop() frees the slot (`used` stays theirs, so
    // a recycled id can never be closed by a stale holder). Rings are left to their consumers.
    for (int id = 0; id < SOCK_MAX; id++) {
        s_sock[id].open = false;
        s_sock[id].rx_notified = false;
        s_sock[id].pull_inflight = false;
    }
    // Requests already queued were aimed at the dead link: fail them so their callers return.
    c6_req_t req;
    while (s_reqq && xQueueReceive(s_reqq, &req, 0) == pdTRUE) req_fail_fast(&req);
}

// Reset the C6 through the boot path and, on success, rejoin the last network. Worker-only: the
// worker owns all SDIO access, so nothing else can touch slot 1 while it is torn down.
static void worker_link_recover(void) {
    const uint32_t now = now_ms();
    const unsigned attempt = (unsigned)s_health.resets + 1;
    c6h_on_reset(&s_health, now);      // arms the backoff for the NEXT attempt, success or not
    printf("[C6AT] reset attempt %u: SDIO down, C6 reset, AT bring-up\n", attempt);
    sdio_bring_down();
    if (!tdisplay_p4_reset_c6()) ESP_LOGE(TAG, "recover: C6 reset hook failed");
    if (!link_bringup(3)) {
        sdio_bring_down();             // leave the slot clean for the next attempt
        printf("[C6AT] reset attempt %u failed: C6 still silent, next try in %lu s\n", attempt,
               (unsigned long)((s_health.next_reset_ms - now_ms()) / 1000));
        return;                        // stay in recovery; callers keep failing fast
    }
    s_recovering   = false;
    s_health_armed = true;
    s_health_act   = C6H_ACT_NONE;
    printf("[C6AT] link back up after reset attempt %u\n", attempt);
    // Rejoin: only if the last request was a join, the station is still wanted, and nothing
    // newer is already waiting in the slot (a Connect the user tapped meanwhile wins).
    taskENTER_CRITICAL(&s_assoc_mux);
    if (s_want_assoc && s_sta_en && s_last_ssid[0] && s_assoc_op == ASSOC_NONE) {
        memcpy(s_assoc_ssid, s_last_ssid, sizeof(s_assoc_ssid));
        memcpy(s_assoc_pass, s_last_pass, sizeof(s_assoc_pass));
        s_assoc_op = ASSOC_JOIN;
    }
    taskEXIT_CRITICAL(&s_assoc_mux);
}

// After a timeout the health counter asks for a plain "AT" ping; a stall takes the link down.
static void worker_health_service(void) {
    if (!s_health_armed) return;
    if (s_health_act == C6H_ACT_PING) {
        s_health_act = C6H_ACT_NONE;
        (void)at_exec("AT", NULL, 0, 1000, C6H_KIND_PING);   // may escalate to STALL
    }
    if (s_health_act == C6H_ACT_STALL) worker_link_stalled();
}

// Execute the latest join/disconnect from the association slot.
static void worker_assoc_service(void) {
    if (s_assoc_op == ASSOC_NONE) return;     // racy peek; re-checked under the lock
    char ssid[33], pass[65];
    assoc_op_t op;
    taskENTER_CRITICAL(&s_assoc_mux);
    op = s_assoc_op;
    if (op == ASSOC_LEAVE && !s_up) {
        s_assoc_op = ASSOC_NONE;              // link down: the association is gone anyway
        op = ASSOC_NONE;
    } else if (op == ASSOC_JOIN && !s_up) {
        op = ASSOC_NONE;                      // keep it pending until the link is back
    } else {
        s_assoc_op = ASSOC_NONE;
        if (op == ASSOC_JOIN) {
            memcpy(ssid, s_assoc_ssid, sizeof(ssid));
            memcpy(pass, s_assoc_pass, sizeof(pass));
            s_join_running = true;            // same critical section: no busy-gap for main.cpp
        }
    }
    taskEXIT_CRITICAL(&s_assoc_mux);
    if (op == ASSOC_JOIN) {
        memcpy(s_last_ssid, ssid, sizeof(s_last_ssid));
        memcpy(s_last_pass, pass, sizeof(s_last_pass));
        worker_do_join(ssid, pass);
        if (s_connected) worker_sntp_service(true);
        // A (re)association drops any running CIPSERVER on the C6:
        // re-arm the want-flag so the idle loop restarts it.
        if (s_srv_want) s_srv_up = false;
        s_join_running = false;
    } else if (op == ASSOC_LEAVE) {
        c6at_command("AT+CWQAP", NULL, 0, 3000);
        s_connected = false; s_ip4 = 0;
    }
}

static void c6WorkerTask(void *arg) {
    (void)arg;
    c6h_init(&s_health);
    // Match LilyGo's factory transport lifecycle: reset immediately before
    // configuring SDIO, then let ESP-AT reach its ready state.
    if (!tdisplay_p4_reset_c6()) ESP_LOGE(TAG, "worker: C6 reset hook failed");
    bool up = link_bringup(10);
    if (!up) {
        s_dead = true;
        ESP_LOGE(TAG, "worker: C6 AT link never came up — Wi-Fi unavailable");
        // Keep servicing the queue as no-ops so requesters never wedge.
    } else {
        s_health_armed = true;
    }
    uint32_t last_refresh = 0;
    for (;;) {
        // Runtime recovery: the link stalled earlier; reset once the backoff window allows.
        if (s_recovering && c6h_reset_due(&s_health, now_ms())) worker_link_recover();
        worker_assoc_service();
        worker_health_service();
        c6_req_t req;
        if (xQueueReceive(s_reqq, &req, pdMS_TO_TICKS(150)) == pdTRUE) {
            if (!s_up) {                       // link dead: fail requests fast
                req_fail_fast(&req);
                continue;
            }
            switch (req.type) {
                case REQ_SCAN:         worker_do_scan(); break;
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
            last_refresh = now_ms();
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
                (void)transport_receive_packet(s_rx, C6_RECV_BUF, &got);
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
    // A dropped request must not leave the UI on "scanning" forever: report an empty result.
    if (!s_reqq || xQueueSend(s_reqq, &r, 0) != pdTRUE) { s_scan_n = 0; s_scan_state = 0; }
}

void c6at_req_join(const char *ssid, const char *pass) {
    if (s_dead || !ssid) return;
    taskENTER_CRITICAL(&s_assoc_mux);
    memset(s_assoc_ssid, 0, sizeof(s_assoc_ssid));
    memset(s_assoc_pass, 0, sizeof(s_assoc_pass));
    strncpy(s_assoc_ssid, ssid, sizeof(s_assoc_ssid) - 1);
    if (pass) strncpy(s_assoc_pass, pass, sizeof(s_assoc_pass) - 1);
    s_assoc_op   = ASSOC_JOIN;          // latest wins: replaces any pending join or disconnect
    s_want_assoc = true;
    taskEXIT_CRITICAL(&s_assoc_mux);
}

void c6at_req_disconnect(void) {
    if (s_dead) return;
    taskENTER_CRITICAL(&s_assoc_mux);
    s_assoc_op   = ASSOC_LEAVE;         // also cancels a join that has not started yet
    s_want_assoc = false;
    memset(s_assoc_ssid, 0, sizeof(s_assoc_ssid));
    memset(s_assoc_pass, 0, sizeof(s_assoc_pass));
    taskEXIT_CRITICAL(&s_assoc_mux);
}

bool c6at_join_busy(void)     { return s_join_running || s_assoc_op == ASSOC_JOIN; }
bool c6at_is_recovering(void) { return s_recovering; }

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
    // Link down (recovering): the C6 was or will be reset, so there is nothing to close on it, and
    // waiting for the worker would block the caller (often the main loop) through the reset.
    if (s_reqq && !s_dead && s_up) {
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
