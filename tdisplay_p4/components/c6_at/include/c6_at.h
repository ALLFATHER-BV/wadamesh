// SPDX-License-Identifier: GPL-3.0-or-later
// c6_at — AT-over-SDIO client for the LilyGo T-Display P4's on-board ESP32-C6.
//
// The C6 ships running Espressif ESP-AT firmware, reachable over SDIO slot 1 (CLK18/CMD19/D0-3=
// 14..17). This is a clean-room driver: the SDIO transport is Espressif's public esp_serial_slave_link
// (ESSL) + SDMMC host API, and the AT command / Wi-Fi / socket layers on top are our own. It exists
// ONLY in the tdisplay_p4 build — no other board can reference it.
//
// Layering:
//   1. transport  : ESSL send/recv of raw byte packets over SDIO
//   2. AT client  : send an AT line, collect the response            (c6at_command)
//   3. worker     : the ONLY task doing AT I/O; async requests + a lock-free status cache that the
//                   UI (via the C6WifiShim facade) reads without ever touching the bus
//   4. Wi-Fi      : scan / join / IP over the worker                 (c6at_req_*, c6at_scan_*)
//   5. sockets    : AT+CIP* TCP/UDP (later phase)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- worker lifecycle ---------------------------------------------------------------------------
// Spawn the AT worker task (idempotent). It brings the SDIO link up itself (with retries, so the C6
// can still be booting), then services requests. NOTHING here blocks the caller.
void c6at_worker_start(void);
bool c6at_is_up(void);        // transport up + ESP-AT answered (set by the worker)
bool c6at_is_dead(void);      // worker gave up bringing the link up (AT probe exhausted)

// ---- async requests (enqueue only; the worker executes) -----------------------------------------
void c6at_req_scan(void);
void c6at_req_join(const char *ssid, const char *pass);   // copies the strings
void c6at_req_disconnect(void);

// ---- lock-free status cache (safe from any task) -------------------------------------------------
bool     c6at_connected(void);
uint32_t c6at_ip4(void);                         // IPv4 as host-order a.b.c.d packed (a<<24)... 0 = none
bool     c6at_current_ssid(char *out, size_t sz);
int      c6at_current_rssi(void);
int      c6at_current_channel(void);             // current AP channel (0 = unknown)
// Scan cache. State mirrors Arduino semantics: -2 idle/failed, -1 running, >=0 result count.
int      c6at_scan_state(void);
bool     c6at_scan_get(int idx, char *ssid, size_t sz, int8_t *rssi, uint8_t *enc);
int      c6at_scan_channel(int idx);             // scan result channel (0 = unknown)
void     c6at_scan_clear(void);
// Facade "radio mode" bookkeeping (no AT traffic — just remembers STA-enabled for WiFi.getMode()).
void     c6at_set_sta_enabled(bool en);
bool     c6at_sta_enabled(void);

// ---- TCP/SSL sockets (AT+CIP*, passive receive) ---------------------------------------------------
// Blocking calls (poll a per-request status the worker completes) — call them from WORKER-FRIENDLY
// tasks (the tile fetcher, reader, version check), never the LVGL thread. Up to 5 links (CIPMUX=1).
// The C6 does TLS on-chip for ssl=true links (fixes on-device HTTPS: the S3 boards never could).
int  c6at_sock_connect(const char *host, uint16_t port, bool ssl, uint32_t timeout_ms); // link id or -1
bool c6at_sock_send(int id, const uint8_t *data, size_t len, uint32_t timeout_ms);
int  c6at_sock_read(int id, uint8_t *dst, size_t maxlen);   // non-blocking pop from the rx ring
int  c6at_sock_peek(int id);                                 // next byte or -1
int  c6at_sock_available(int id);                            // ring bytes (kicks a pull when empty)
bool c6at_sock_connected(int id);                            // open, or closed with data still buffered
void c6at_sock_close(int id);

// ---- inbound TCP server (AT+CIPSERVER) ------------------------------------------------------------
// ESP-AT allows exactly ONE listening port — on this board it's the phone-app companion TCP server.
// The C6 assigns inbound connections the LOWEST free link id (outbound allocation is top-down to
// stay out of the way); accepted links reuse the same per-link rings / pull / send machinery.
bool c6at_server_start(uint16_t port);   // async (worker executes); idempotent per port
void c6at_server_stop(void);
int  c6at_server_pending(void);          // inbound connections waiting to be accept()ed
int  c6at_server_accept(void);           // pop one inbound link id, or -1

// SNTP over the C6 (AT+CIPSNTP*): 0 until synced, then the current UTC epoch (self-advancing).
uint32_t c6at_sntp_epoch(void);

// ---- low-level (worker-internal + bring-up probes; do NOT call from other tasks once the worker
// runs — all AT I/O must stay single-threaded) ----------------------------------------------------
bool c6at_begin(void);
bool c6at_command(const char *cmd, char *resp, size_t resp_sz, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
