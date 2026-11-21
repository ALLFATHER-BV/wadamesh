#include "WebSocketCompanionServer.h"
#include <Arduino.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <lwip/sockets.h>
#include <esp_heap_caps.h>
#include "WebMirror.h"
#include "WebConsoleAssets.h"
#include "../../ui-touch/device_caps.h"   // CAP_WEB_CONSOLE_SETTINGS: the Settings and Dashboard tabs
#if WADA_WEB_FILE_TRANSFER
#include "WebFileTransfer.h"
#include "WebFileTransferPage.h"
#endif

#ifndef WS_FRAME_DEBUG
#define WS_FRAME_DEBUG 0
#endif

#define TCP_WRITE_TIMEOUT_MS   120
#define WS_PAGE_SEND_MS        10000        // a console page (up to ~13 KB gzipped) must have left by then
#define WS_PAGE_CHUNK          1436         // one TCP segment: a probed-writable socket takes it without blocking
#define WS_WEDGED_DROP_MS      10000
#define WS_AUTH_TIMEOUT_MS     45000        // a browser socket must be approved within this, or it is dropped (frees the slot); longer than the device's 30 s request so its answer arrives first
#define WS_MIRROR_WRITE_TIMEOUT_MS 250   // mirror bands are bigger than companion frames — a little more headroom before we treat the frame as unrecoverable
#define WS_MAGIC               "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_MIRROR_TXBUF        33000        // max popped mirror frame (band <= 32000 + header)
#define WS_MIRROR_TX_BUDGET    (48 * 1024)  // max mirror bytes pushed per serviceMirror() call
#define WS_MIRROR_CLIENT_TXBUF 16384        // per-client outgoing frame buffer (mirror bands <=4 KB; terminal JSON data frames can be larger)

static const char WS_HTTP_UI_DISABLED[] =
  "HTTP/1.1 503 Service Unavailable\r\n"
  "Content-Type: text/plain; charset=utf-8\r\n"
  "Cache-Control: no-store\r\n"
  "Connection: close\r\n\r\n"
  "The web UI is not enabled on the device.\n";

#define COMP_STATE_IDLE        0
#define COMP_STATE_HDR_FOUND   1
#define COMP_STATE_LEN1_FOUND  2
#define COMP_STATE_LEN2_FOUND  3

#define WS_STATE_HEADER_0      0
#define WS_STATE_HEADER_1      1
#define WS_STATE_LEN_EXT       2
#define WS_STATE_MASK          3
#define WS_STATE_PAYLOAD       4

static const char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64Encode20(const uint8_t* in, char* out) {
  for (int i = 0; i < 20; i += 3) {
    uint32_t v = in[i] << 16;
    if (i + 1 < 20) v |= in[i + 1] << 8;
    if (i + 2 < 20) v |= in[i + 2];
    out[0] = BASE64[(v >> 18) & 63];
    out[1] = BASE64[(v >> 12) & 63];
    out[2] = (i + 1 < 20) ? BASE64[(v >> 6) & 63] : '=';
    out[3] = (i + 2 < 20) ? BASE64[v & 63] : '=';
    out += 4;
  }
  *out = '\0';
}

// True when lwIP can accept bytes for this socket right now — see the identical probe
// in TCPCompanionServer.cpp. Without it, WiFiClient::write() against a peer that
// stopped ACKing (half-open socket) blocks in 1 s select() waits on the loop thread.
static bool socketWritableNow(WiFiClient& client) {
#if defined(HAS_TDISPLAY_P4)
  // C6/AT transport: no lwIP fd to select() on (fd() is -1 by design). Writes go through the AT
  // worker whose CIPSEND exchange is itself bounded (the C6 buffers TX), so liveness is the only
  // cheap pre-check here.
  return client.connected();
#else
  int fd = client.fd();
  if (fd < 0) return false;
  fd_set wset;
  FD_ZERO(&wset);
  FD_SET(fd, &wset);
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 0;
  return select(fd + 1, NULL, &wset, NULL, &tv) > 0 && FD_ISSET(fd, &wset);
#endif
}

static bool writeAllBytes(WiFiClient& client, const uint8_t* buf, size_t len, uint32_t timeout_ms) {
  size_t sent = 0;
  uint32_t start = millis();
  while (sent < len) {
    if (!client.connected()) return false;
    if (socketWritableNow(client)) {
      size_t n = client.write(buf + sent, len - sent);
      if (n > 0) {
        sent += n;
        continue;
      }
    }
    if (millis() - start >= timeout_ms) return false;
    delay(1);
  }
  return true;
}

// RAII lock for the _clients array. Recursive so nested same-thread calls
// (serviceMirror -> writeBinaryFrame -> drainClientTx -> disconnectClient, or
// tickHandshake -> acceptNewClients/pruneDisconnected) never self-deadlock.
namespace {
struct WsClientsLock {
  SemaphoreHandle_t m;
  explicit WsClientsLock(SemaphoreHandle_t mtx) : m(mtx) { if (m) xSemaphoreTakeRecursive(m, portMAX_DELAY); }
  ~WsClientsLock() { if (m) xSemaphoreGiveRecursive(m); }
  WsClientsLock(const WsClientsLock&) = delete;
  WsClientsLock& operator=(const WsClientsLock&) = delete;
};
}

WebSocketCompanionServer::WebSocketCompanionServer()
  : _server(WiFiServer()), _port(0), _listening(false), _poll_start_idx(0), _mirror_txbuf(nullptr) {
  _client_mtx = xSemaphoreCreateRecursiveMutex();
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    _clients[i].in_use = false;
    _clients[i].accept_ms = 0;
    _clients[i].handshake_done = false;
    _clients[i].handshake_len = 0;
    _clients[i].ws_state = WS_STATE_HEADER_0;
    _clients[i].comp_state = COMP_STATE_IDLE;
    _clients[i].stall_ms = 0;
    _clients[i].is_mirror = false;
    _clients[i].is_term = false;
    _clients[i].is_files = false;
    _clients[i].authed = false;
    _clients[i].waiting = false;
    _clients[i].auth_gen = 0;
    _clients[i].close_after_tx = false;
    _clients[i].auth_failures = 0;
    _clients[i].meta_sent = false;
    _clients[i].lock_sent = 0xFF;
    _clients[i].page_ptr = nullptr;
    _clients[i].page_len = _clients[i].page_sent = _clients[i].page_ms = 0;
  #if WADA_WEB_FILE_TRANSFER
    _clients[i].files_request_pending = false;
    _clients[i].files_frame_started_ms = 0;
    _clients[i].files_rx_buf = nullptr;
    _clients[i].files_rx_len = 0;
  #endif
    _clients[i].tx_buf = nullptr;
    _clients[i].tx_len = 0;
    _clients[i].tx_sent = 0;
  }
}

void WebSocketCompanionServer::begin(uint16_t port) {
  _port = port;
  _listening = true;
  _server.begin(port);
  Serial.printf("[WS] listening port=%u (plain)\n", (unsigned)port);
}

void WebSocketCompanionServer::stop() {
  WsClientsLock _lk(_client_mtx);
  _listening = false;
  _server.stop();
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    if (_clients[i].in_use) {
      _clients[i].client.stop();
      _clients[i].in_use = false;
      _clients[i].page_ptr = nullptr;
    }
  }
}

void WebSocketCompanionServer::pauseListen() {
  if (!_listening) return;
  _server.stop();
  _listening = false;
}

void WebSocketCompanionServer::resumeListen() {
  if (_listening || _port == 0) return;
  _server.begin(_port);
  _listening = true;
  Serial.printf("[WS] listen resumed port=%u (plain)\n", (unsigned)_port);
}

void WebSocketCompanionServer::adoptClient(WiFiClient& incoming) {
  // Slot-insert an already-accepted connection. Used by our own accept loop below AND by the
  // T-Display P4's first-byte router (one shared AT listener dispatches companion vs web).
  WsClientsLock _lk(_client_mtx);
  int slot = -1;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    if (!_clients[i].in_use) {
      slot = i;
      break;
    }
  }
  // No free slot: evict the oldest existing client so a fresh connect can
  // recover from stuck slots (half-closed sockets where connected() still
  // reports true). Without this, every new connect is accept()ed then
  // immediately stop()ed, producing SYN/SYN+ACK/ACK/FIN-ACK with zero data.
  if (slot < 0) {
#if WADA_WEB_FILE_TRANSFER
    // The third slot is reserved capacity for the runtime file-transfer client.
    // Never let a new browser connection evict a live companion/VNC/terminal
    // client before its HTTP path is known; a full server simply refuses it.
    incoming.stop();
    return;
#else
    uint32_t now = millis();
    uint32_t oldest_age = 0;
    for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
      uint32_t age = now - _clients[i].accept_ms;
      if (slot < 0 || age > oldest_age) {
        slot = i;
        oldest_age = age;
      }
    }
    _clients[slot].client.stop();
    _clients[slot].in_use = false;
  #endif
  }
  _clients[slot].client = incoming;
  _clients[slot].in_use = true;
  _clients[slot].accept_ms = millis();
  _clients[slot].handshake_done = false;
  _clients[slot].handshake_len = 0;
  _clients[slot].ws_state = WS_STATE_HEADER_0;
  _clients[slot].comp_state = COMP_STATE_IDLE;
  _clients[slot].stall_ms = 0;
  _clients[slot].is_mirror = false;
  _clients[slot].is_term = false;
  _clients[slot].is_files = false;
  _clients[slot].authed = false;
  _clients[slot].waiting = false;
  _clients[slot].auth_gen = 0;
  _clients[slot].close_after_tx = false;
  _clients[slot].auth_failures = 0;
  _clients[slot].meta_sent = false;
  _clients[slot].lock_sent = 0xFF;
  _clients[slot].page_ptr = nullptr;
  _clients[slot].page_len = _clients[slot].page_sent = 0;
#if WADA_WEB_FILE_TRANSFER
  _clients[slot].files_request_pending = false;
  _clients[slot].files_frame_started_ms = 0;
  _clients[slot].files_rx_len = 0;
#endif
  _clients[slot].tx_len = 0;      // drop any stale pending frame from the previous occupant (tx_buf is reused)
  _clients[slot].tx_sent = 0;
}

void WebSocketCompanionServer::acceptNewClients() {
  if (!_listening) return;
  while (_server.hasClient()) {
    WiFiClient incoming = _server.accept();
    if (!incoming) continue;
    adoptClient(incoming);
  }
}

void WebSocketCompanionServer::pruneDisconnected() {
  uint32_t now = millis();
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    if (!_clients[i].in_use) continue;
    bool stale_handshake = !_clients[i].handshake_done && !_clients[i].page_ptr &&
                           (now - _clients[i].accept_ms) > WS_HANDSHAKE_TIMEOUT_MS;
    if (!_clients[i].client.connected() || stale_handshake) {
      _clients[i].client.stop();
      _clients[i].in_use = false;
      _clients[i].page_ptr = nullptr;
    }
  }
}

// The value of request header `name` (matched case-insensitively at a line start), its
// length in *len; nullptr when the request has no such header.
static const char* httpHeader(const char* req, const char* name, size_t* len) {
  const size_t nl = strlen(name);
  for (const char* l = strstr(req, "\r\n"); l; l = strstr(l, "\r\n")) {
    l += 2;
    if (strncasecmp(l, name, nl) != 0 || l[nl] != ':') continue;
    const char* v = l + nl + 1;
    while (*v == ' ' || *v == '\t') v++;
    const char* e = v;
    while (*e && *e != '\r' && *e != '\n') e++;
    while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
    *len = (size_t)(e - v);
    return v;
  }
  return nullptr;
}

// True when the request carries an Origin that is not the address it was sent to: a
// page from another site (or another host on the LAN) the user happens to have open.
// Browsers send Origin on every WebSocket upgrade and every cross-origin fetch; the
// companion app and other non-browser clients send none, and those stay allowed.
static bool crossOrigin(const char* req) {
  size_t ol = 0, hl = 0;
  const char* o = httpHeader(req, "Origin", &ol);
  if (!o) return false;
  const char* h = httpHeader(req, "Host", &hl);
  const char* sep = nullptr;
  for (size_t i = 0; i + 3 <= ol; i++)
    if (o[i] == ':' && o[i + 1] == '/' && o[i + 2] == '/') { sep = o + i + 3; break; }
  if (!h || !sep) return true;   // "Origin: null" (a sandboxed or file: page) included
  const size_t rest = ol - (size_t)(sep - o);
  return rest != hl || strncasecmp(sep, h, hl) != 0;
}

// The peer's IPv4 address for the approval card (0 when this transport cannot tell).
static uint32_t clientIp(WiFiClient& client) {
#if defined(HAS_TDISPLAY_P4) && !TDP4_C6_HOSTED
  (void)client;
  return 0;   // AT links have no lwIP socket to ask
#else
  return static_cast<uint32_t>(client.remoteIP());
#endif
}

bool WebSocketCompanionServer::doHandshake(int idx) {
  WSClientState* c = &_clients[idx];
  WiFiClient* cl = &c->client;
  if (c->page_ptr) { drainPage(idx); return false; }
  while (cl->available() && c->handshake_len < WS_HANDSHAKE_MAX_LEN - 1) {
    char ch = (char)cl->read();
    c->handshake_buf[c->handshake_len++] = ch;
    c->handshake_buf[c->handshake_len] = '\0';
    if (c->handshake_len >= 4 &&
        c->handshake_buf[c->handshake_len - 4] == '\r' &&
        c->handshake_buf[c->handshake_len - 3] == '\n' &&
        c->handshake_buf[c->handshake_len - 2] == '\r' &&
        c->handshake_buf[c->handshake_len - 1] == '\n') {
      const char* key_hdr = "Sec-WebSocket-Key:";
      char* buf = c->handshake_buf;
      for (size_t i = 0; i + 20 < c->handshake_len; i++) {
        if (strncasecmp(buf + i, key_hdr, 18) == 0) {
          char* key_start = buf + i + 18;
          while (*key_start == ' ' || *key_start == '\t') key_start++;
          char* key_end = key_start;
          while (*key_end && *key_end != '\r' && *key_end != '\n') key_end++;
          size_t key_len = key_end - key_start;
          if (key_len == 0 || key_len > 128) break;

          // Route browser endpoints before accepting the upgrade so an unavailable
          // session (feature off, or file transfer already in use) never observes a
          // successful WebSocket handshake.
          c->is_mirror = (strncmp(c->handshake_buf, "GET /mirror ", 12) == 0);
          c->is_term   = (strncmp(c->handshake_buf, "GET /term ", 10) == 0);
          bool unavailable = (c->is_mirror && !g_web_mirror.enabled()) ||
                             (c->is_term && !g_web_mirror.terminalOn());
#if WADA_WEB_FILE_TRANSFER
          c->is_files  = (strncmp(c->handshake_buf, "GET /files ", 11) == 0);
          bool another_files = false;
          if (c->is_files) {
            for (int j = 0; j < WS_COMPANION_MAX_CLIENTS; ++j)
              if (j != idx && _clients[j].in_use && _clients[j].is_files) another_files = true;
          }
          if (c->is_files && (!g_web_file_transfer.enabled() || another_files)) unavailable = true;
#endif
          // The console's sockets only take a page served from this device. The companion
          // protocol path stays open to any client (the official app, the web client).
          if ((c->is_mirror || c->is_term || c->is_files) && crossOrigin(c->handshake_buf)) {
            static const char forbidden[] = "HTTP/1.1 403 Forbidden\r\n"
                                            "Content-Length: 0\r\nConnection: close\r\n\r\n";
            writeAllBytes(*cl, reinterpret_cast<const uint8_t*>(forbidden), sizeof(forbidden) - 1,
                          TCP_WRITE_TIMEOUT_MS);
            c->client.stop();
            c->in_use = false;
            return false;
          }
          if (unavailable) {
            const char* refused = "HTTP/1.1 503 Service Unavailable\r\n"
                                  "Content-Length: 0\r\nConnection: close\r\n\r\n";
            writeAllBytes(*cl, reinterpret_cast<const uint8_t*>(refused),
                          strlen(refused), TCP_WRITE_TIMEOUT_MS);
            c->client.stop();
            c->in_use = false;
            return false;
          }

          char concat[128 + sizeof(WS_MAGIC)];
          memcpy(concat, key_start, key_len);
          memcpy(concat + key_len, WS_MAGIC, sizeof(WS_MAGIC) - 1);
          size_t concat_len = key_len + sizeof(WS_MAGIC) - 1;
          uint8_t hash[20];
          mbedtls_sha1((const unsigned char*)concat, concat_len, hash);
          char b64[32];
          base64Encode20(hash, b64);

          const char* resp = "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: ";
          size_t resp_len = strlen(resp);
          if (!writeAllBytes(*cl, (const uint8_t*)resp, resp_len, TCP_WRITE_TIMEOUT_MS))
            return false;
          if (!writeAllBytes(*cl, (const uint8_t*)b64, 28, TCP_WRITE_TIMEOUT_MS))
            return false;
          if (!writeAllBytes(*cl, (const uint8_t*)"\r\n\r\n", 4, TCP_WRITE_TIMEOUT_MS))
            return false;

          if (c->is_mirror || c->is_term || c->is_files) c->client.setNoDelay(true);   // low latency: small frames, no Nagle coalescing
          c->meta_sent = false;
          c->lock_sent = 0xFF;
          c->handshake_done = true;
          c->ws_state = WS_STATE_HEADER_0;
          c->comp_state = COMP_STATE_IDLE;
          return true;
        }
      }
      servePage(idx);
      if (c->page_ptr) { drainPage(idx); return false; }   // the body goes out over the next loops
      c->client.stop();
      c->in_use = false;
      return false;
    }
  }
  if (c->handshake_len >= WS_HANDSHAKE_MAX_LEN - 1) {
    c->client.stop();
    c->in_use = false;
    return false;
  }
  return false;
}

void WebSocketCompanionServer::tickHandshake() {
  WsClientsLock _lk(_client_mtx);
  acceptNewClients();
  pruneDisconnected();
}

size_t WebSocketCompanionServer::pollRecvFrame(uint8_t dest[], int* client_index_out) {
  WsClientsLock _lk(_client_mtx);
  acceptNewClients();
  pruneDisconnected();

  int start = _poll_start_idx;
  if (start < 0 || start >= WS_COMPANION_MAX_CLIENTS) start = 0;

  for (int off = 0; off < WS_COMPANION_MAX_CLIENTS; off++) {
    int idx = (start + off) % WS_COMPANION_MAX_CLIENTS;
    if (!_clients[idx].in_use || !_clients[idx].client.connected()) continue;
    WSClientState* c = &_clients[idx];

    if (!c->handshake_done) {
      doHandshake(idx);
      continue;
    }
    // Mirror clients carry framebuffer/pointer traffic, not companion frames —
    // serviceMirror() owns their socket. Never feed their bytes to the parser.
    if (c->is_mirror || c->is_term || c->is_files) continue;

    WiFiClient* cl = &c->client;
    while (cl->available()) {
      if (c->ws_state == WS_STATE_HEADER_0) {
        uint8_t b = (uint8_t)cl->read();
        c->ws_opcode = b & 0x0F;
        c->ws_state = WS_STATE_HEADER_1;
        continue;
      }
      if (c->ws_state == WS_STATE_HEADER_1) {
        uint8_t b = (uint8_t)cl->read();
        uint8_t len7 = b & 0x7F;
        c->ws_payload_len = len7;
        c->ws_payload_read = 0;
        if (len7 == 126) {
          c->ws_state = WS_STATE_LEN_EXT;
          continue;
        }
        if (len7 == 127) {
          c->ws_state = WS_STATE_LEN_EXT;
          c->ws_payload_len = 0;
          continue;
        }
        c->ws_state = WS_STATE_MASK;
        continue;
      }
      if (c->ws_state == WS_STATE_LEN_EXT) {
        if (c->ws_payload_len == 126) {
          if (cl->available() < 2) break;
          uint8_t lo = (uint8_t)cl->read();
          uint8_t hi = (uint8_t)cl->read();
          c->ws_payload_len = (uint16_t)lo | ((uint16_t)hi << 8);
        } else {
          if (cl->available() < 8) break;
          c->ws_payload_len = 0;
          for (int i = 0; i < 8; i++)
            c->ws_payload_len |= (uint64_t)(uint8_t)cl->read() << (i * 8);
        }
        c->ws_state = WS_STATE_MASK;
        continue;
      }
      if (c->ws_state == WS_STATE_MASK) {
        if (cl->available() < 4) break;
        for (int i = 0; i < 4; i++) c->ws_mask[i] = (uint8_t)cl->read();
        c->ws_state = WS_STATE_PAYLOAD;
        continue;
      }

      if (c->ws_payload_read >= c->ws_payload_len) {
        c->ws_state = WS_STATE_HEADER_0;
        continue;
      }
      uint8_t b = (uint8_t)cl->read() ^ c->ws_mask[c->ws_payload_read % 4];
      c->ws_payload_read++;

      if (c->ws_opcode != 0x02) continue;

      switch (c->comp_state) {
        case COMP_STATE_IDLE:
          if (b == '<') c->comp_state = COMP_STATE_HDR_FOUND;
          break;
        case COMP_STATE_HDR_FOUND:
          c->comp_frame_len = (uint16_t)b;
          c->comp_state = COMP_STATE_LEN1_FOUND;
          break;
        case COMP_STATE_LEN1_FOUND:
          c->comp_frame_len |= ((uint16_t)b) << 8;
          c->comp_rx_len = 0;
          c->comp_state = (c->comp_frame_len > 0) ? COMP_STATE_LEN2_FOUND : COMP_STATE_IDLE;
          break;
        default:
          if (c->comp_rx_len < MAX_FRAME_SIZE) c->comp_rx_buf[c->comp_rx_len] = b;
          c->comp_rx_len++;
          if (c->comp_rx_len >= c->comp_frame_len) {
            size_t copy_len = c->comp_frame_len;
            if (copy_len > MAX_FRAME_SIZE) copy_len = MAX_FRAME_SIZE;
            memcpy(dest, c->comp_rx_buf, copy_len);
            c->comp_state = COMP_STATE_IDLE;
            if (client_index_out) *client_index_out = idx;
            _poll_start_idx = (idx + 1) % WS_COMPANION_MAX_CLIENTS;
            return copy_len;
          }
          break;
      }
    }
  }
  _poll_start_idx = (start + 1) % WS_COMPANION_MAX_CLIENTS;
  return 0;
}

size_t WebSocketCompanionServer::writeToClient(int client_index, const uint8_t src[], size_t len) {
  WsClientsLock _lk(_client_mtx);
  if (client_index < 0 || client_index >= WS_COMPANION_MAX_CLIENTS || len > MAX_FRAME_SIZE) return 0;
  if (!_clients[client_index].in_use || !_clients[client_index].client.connected()) return 0;

  WiFiClient* cl = &_clients[client_index].client;
  uint8_t hdr[4];
  size_t hdr_len;
  hdr[0] = 0x82;
  if (len < 126) {
    hdr[1] = (uint8_t)len;
    hdr_len = 2;
  } else {
    hdr[1] = 126;
    hdr[2] = (len >> 8) & 0xFF;
    hdr[3] = len & 0xFF;
    hdr_len = 4;
  }
  if (!writeAllBytes(*cl, hdr, hdr_len, TCP_WRITE_TIMEOUT_MS) ||
      !writeAllBytes(*cl, src, len, TCP_WRITE_TIMEOUT_MS)) {
#if WS_FRAME_DEBUG
    Serial.printf("WS frame client=%d code=%u len=%u written=0\n", client_index, (unsigned)(len ? src[0] : 0), (unsigned)len);
#endif
    // Same wedged-peer reaper as the TCP server: a client that stays unwritable for
    // WS_WEDGED_DROP_MS straight is half-open (browser tab gone, phone asleep) and
    // would otherwise eat the write-timeout budget on every pushed frame forever.
    WSClientState* c = &_clients[client_index];
    if (c->stall_ms == 0) {
      c->stall_ms = millis() | 1;
    } else if (millis() - c->stall_ms >= WS_WEDGED_DROP_MS) {
      disconnectClient(client_index);
    }
    return 0;
  }
  _clients[client_index].stall_ms = 0;
#if WS_FRAME_DEBUG
  Serial.printf("WS frame client=%d code=%u len=%u written=%u\n", client_index, (unsigned)(len ? src[0] : 0), (unsigned)len, (unsigned)len);
#endif
  return len;
}

size_t WebSocketCompanionServer::writeToAllClients(const uint8_t src[], size_t len) {
  WsClientsLock _lk(_client_mtx);
  if (len == 0 || len > MAX_FRAME_SIZE) return 0;
  int connected = 0;
  int sent = 0;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    bool ok = _clients[i].in_use && _clients[i].client.connected() && _clients[i].handshake_done && !_clients[i].is_mirror && !_clients[i].is_term && !_clients[i].is_files;
    if (ok) {
      connected++;
      if (writeToClient(i, src, len) == len) sent++;
    }
  }
  return (sent == connected) ? len : 0;
}

bool WebSocketCompanionServer::isClientConnected(int client_index) const {
  WsClientsLock _lk(_client_mtx);
  if (client_index < 0 || client_index >= WS_COMPANION_MAX_CLIENTS) return false;
  const WSClientState* c = &_clients[client_index];
  return c->in_use && c->client.connected() && c->handshake_done && !c->is_mirror && !c->is_term && !c->is_files;
}

int WebSocketCompanionServer::connectedCount() const {
  WsClientsLock _lk(_client_mtx);
  int n = 0;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    if (_clients[i].in_use && _clients[i].client.connected() && _clients[i].handshake_done && !_clients[i].is_mirror && !_clients[i].is_term && !_clients[i].is_files)
      n++;
  }
  return n;
}

void WebSocketCompanionServer::disconnectClient(int client_index) {
  WsClientsLock _lk(_client_mtx);
  if (client_index >= 0 && client_index < WS_COMPANION_MAX_CLIENTS && _clients[client_index].in_use) {
    _clients[client_index].client.stop();
    _clients[client_index].in_use = false;
    _clients[client_index].stall_ms = 0;
    _clients[client_index].tx_len = 0;   // discard any half-sent frame; tx_buf stays for slot reuse
    _clients[client_index].tx_sent = 0;
  }
}

// ============================================================================
// Web UI mirror
// ============================================================================
int WebSocketCompanionServer::mirrorClientCount() const {
  WsClientsLock _lk(_client_mtx);
  int n = 0;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++)
    if (_clients[i].in_use && _clients[i].client.connected() &&
        _clients[i].handshake_done && _clients[i].is_mirror && _clients[i].authed) n++;
  return n;
}

// Queue one binary WS frame (FIN+binary, len up to 65535 via the 126 extended header)
// into the client's tx_buf, then push what the socket accepts right now — all
// NON-BLOCKING. The frame is drained in full over later drainClientTx() calls, so the
// loop never spins on a write and a slow link never corrupts the stream or drops the
// client. Returns 0 (rejected) if a previous frame is still draining (caller waits) or
// the frame is too big to buffer. On success the whole frame is guaranteed atomic.
size_t WebSocketCompanionServer::writeBinaryFrame(int client_index, const uint8_t src[], size_t len) {
  WsClientsLock _lk(_client_mtx);
  if (client_index < 0 || client_index >= WS_COMPANION_MAX_CLIENTS || len == 0 || len > 0xFFFF) return 0;
  WSClientState* c = &_clients[client_index];
  if (!c->in_use || !c->client.connected()) return 0;
  if (c->tx_len != 0) return 0;                          // a frame is still draining -> not ready

  if (!c->tx_buf) {
    c->tx_buf = (uint8_t*)heap_caps_malloc(WS_MIRROR_CLIENT_TXBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c->tx_buf) c->tx_buf = (uint8_t*)malloc(WS_MIRROR_CLIENT_TXBUF);
    if (!c->tx_buf) return 0;
  }
  const size_t hdr_len = (len < 126) ? 2 : 4;
  if (hdr_len + len > WS_MIRROR_CLIENT_TXBUF) return 0;  // never for our <=4 KB bands, but stay safe
  uint8_t* p = c->tx_buf;
  p[0] = 0x82;                                           // FIN + binary
  if (len < 126) { p[1] = (uint8_t)len; }
  else           { p[1] = 126; p[2] = (len >> 8) & 0xFF; p[3] = len & 0xFF; }
  memcpy(p + hdr_len, src, len);
  c->tx_len  = (uint16_t)(hdr_len + len);
  c->tx_sent = 0;
  drainClientTx(client_index);                          // push whatever fits now (non-blocking)
  return len;
}

// Push a mirror client's pending tx_buf bytes as far as the socket will take them right
// now, without ever blocking. Frame boundaries are byte-exact (tx_sent offset), so a
// partial push just resumes next call — the stream never desyncs. A client that can't
// take a single byte for WS_WEDGED_DROP_MS is genuinely dead and gets reaped.
void WebSocketCompanionServer::drainClientTx(int idx) {
  WSClientState* c = &_clients[idx];
  if (!c->in_use || c->tx_len == 0) return;
  if (!c->client.connected()) { c->tx_len = c->tx_sent = 0; return; }
#if defined(HAS_TDISPLAY_P4)
  // AT transport: write() is a BLOCKING worker round-trip (there is no lwIP buffer to
  // fill non-blockingly), and we run with _client_mtx held — draining a whole band here
  // starved the main loop (tickHandshake / pollRecvFrame / mesh) and froze the device.
  // Send ONE CIPSEND-sized chunk per call; tx_sent resumes next pass. serviceMirror's
  // all-idle check then also self-limits to one in-flight band. A failed chunk means the
  // worker already waited out its SEND-OK window (seconds) — retrying a sick browser tab
  // would stall the whole AT link every pass, so drop the client immediately (the viewer
  // page auto-reconnects).
  size_t chunk = c->tx_len - c->tx_sent;
  if (chunk > 1436) chunk = 1436;   // == c6_at SOCK_PULL_MAX: exactly one AT round-trip
  size_t n = c->client.write(c->tx_buf + c->tx_sent, chunk);
  if (n == 0) { disconnectClient(idx); return; }
  c->tx_sent += (uint16_t)n;
  if (c->tx_sent >= c->tx_len) { c->tx_len = 0; c->tx_sent = 0; c->stall_ms = 0; }
#else
  while (c->tx_sent < c->tx_len && socketWritableNow(c->client)) {
    size_t n = c->client.write(c->tx_buf + c->tx_sent, c->tx_len - c->tx_sent);
    if (n == 0) break;
    c->tx_sent += (uint16_t)n;
  }
  if (c->tx_sent >= c->tx_len) { c->tx_len = 0; c->tx_sent = 0; c->stall_ms = 0; return; }
  // Still bytes pending -> the socket is momentarily full. Just wait; only a peer that
  // stays unwritable for the full drop window is treated as wedged.
  if (c->stall_ms == 0) c->stall_ms = millis() | 1;
  else if (millis() - c->stall_ms >= WS_WEDGED_DROP_MS) disconnectClient(idx);
#endif
}

// A browser's console id is 32 lowercase hex characters it keeps in localStorage; the
// device only ever holds a hash of it.
static bool consoleKey(const char* id, size_t n, uint8_t key[16]) {
  if (n != 32) return false;
  for (size_t i = 0; i < n; i++) {
    const char ch = id[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  }
  uint8_t h[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char*>(id), n);
  mbedtls_sha256_finish(&ctx, h);
  mbedtls_sha256_free(&ctx);
  memcpy(key, h, 16);
  return true;
}

void WebSocketCompanionServer::grantClient(int idx) {
  WSClientState* c = &_clients[idx];
  c->authed = true;
  c->waiting = false;
  c->auth_failures = 0;
  c->auth_gen = g_web_session.generation();
#if WADA_WEB_FILE_TRANSFER
  if (c->is_files) g_web_file_transfer.touch();
#endif
  writeBinaryFrame(idx, reinterpret_cast<const uint8_t*>("AUTH OK"), 7);
}

// The first frame of every console socket (mesh, screen, files):
//   "HELLO <id> <label>"  -> "AUTH OK" (trusted / already allowed), "WAIT" (the device is
//                            asking its user; pollApproval sends the answer) or "ERR ...".
//   "CODE <id> nnnnnn"    -> the 6-digit code fallback; counts as Allow once for <id>.
//   "AUTH nnnnnn"         -> the code alone, from a page cached before the console.
// Other frames from a not-yet-authed client (a stray click, a key) are ignored and never
// count as guesses. Three wrong codes disconnect; the lockout itself is shared (g_web_auth).
void WebSocketCompanionServer::authFrame(int idx, const uint8_t* buf, size_t len) {
  WSClientState* c = &_clients[idx];
  if (c->ws_opcode != 0x01 || len < 6 || len > 95) return;
  char line[96];
  memcpy(line, buf, len);
  line[len] = '\0';
  const char* reply = nullptr;

  if (strncmp(line, "HELLO ", 6) == 0) {
    const char* id = line + 6;
    const char* sp = strchr(id, ' ');
    const size_t id_len = sp ? (size_t)(sp - id) : strlen(id);
    if (!consoleKey(id, id_len, c->key)) { reply = "ERR reload the page"; c->close_after_tx = true; }
    else {
      switch (g_web_session.hello(c->key, sp ? sp + 1 : "", clientIp(c->client))) {
        case WebSessionAuth::OK:   grantClient(idx); return;
        case WebSessionAuth::WAIT: c->waiting = true; reply = "WAIT"; break;
        case WebSessionAuth::BUSY: reply = "ERR the device is answering another browser; enter the code instead"; break;
        default:                   reply = "ERR denied on the device"; break;
      }
    }
  } else {
    const uint8_t* code = nullptr;
    bool keyed = false;
    if (strncmp(line, "CODE ", 5) == 0 && len == 5 + 32 + 1 + 6 && line[37] == ' ') {
      keyed = consoleKey(line + 5, 32, c->key);
      if (keyed) code = reinterpret_cast<const uint8_t*>(line + 38);
    } else if (len == 11 && strncmp(line, "AUTH ", 5) == 0) {
      code = reinterpret_cast<const uint8_t*>(line + 5);
    }
    if (!code) return;
    const bool allowed = g_web_auth.allowed();
    if (allowed && g_web_auth.matches(code, 6)) {
      g_web_auth.noteSuccess();
      if (keyed) g_web_session.allowSession(c->key);
      grantClient(idx);
      return;
    }
    if (allowed) g_web_auth.noteFailure();
    if (++c->auth_failures >= 3) c->close_after_tx = true;
    reply = allowed ? "ERR invalid code" : "ERR try again in 30 seconds";
  }
  if (reply && c->tx_len == 0) writeBinaryFrame(idx, reinterpret_cast<const uint8_t*>(reply), strlen(reply));
}

// A browser that got "WAIT": pass on the device user's answer once there is one. A denied
// browser stays connected (it can still use the code) until the auth timeout drops it.
void WebSocketCompanionServer::pollApproval(int idx) {
  WSClientState* c = &_clients[idx];
  if (!c->waiting || c->authed || c->tx_len != 0) return;
  const WebSessionAuth::Check r = g_web_session.status(c->key);
  if (r == WebSessionAuth::OK) {
    grantClient(idx);
  } else if (r == WebSessionAuth::DENIED) {
    c->waiting = false;
    static const char denied[] = "ERR denied on the device";
    writeBinaryFrame(idx, reinterpret_cast<const uint8_t*>(denied), sizeof(denied) - 1);
  }
}

#if WADA_WEB_FILE_TRANSFER
// Copy query parameter `name` into out (cap includes the NUL), decoding '+' and %XX.
// Leaves out empty when absent; a value too long for out is cut short.
static void queryParam(const char* q, const char* name, char* out, size_t cap) {
  out[0] = '\0';
  const size_t nl = strlen(name);
  while (q && *q) {
    if (strncmp(q, name, nl) == 0 && q[nl] == '=') {
      q += nl + 1;
      size_t n = 0;
      while (*q && *q != '&' && n + 1 < cap) {
        char ch = *q++;
        if (ch == '+') ch = ' ';
        else if (ch == '%' && isxdigit((unsigned char)q[0]) && isxdigit((unsigned char)q[1])) {
          const char hex[3] = { q[0], q[1], 0 };
          ch = (char)strtol(hex, nullptr, 16);
          q += 2;
        }
        out[n++] = ch;
      }
      out[n] = '\0';
      return;
    }
    q = strchr(q, '&');
    if (q) q++;
  }
}
#endif

namespace {
struct ConsolePage { const char* path; const uint8_t* gz; size_t len; };
}

// Plain HTTP GET (no WebSocket upgrade): the console's pages, gzipped at build time from
// web/console/, plus two small JSON endpoints. Each feature's page is only served while
// that feature is on; everything else is 404. Always closes the connection.
bool WebSocketCompanionServer::servePage(int idx) {
  WSClientState* c = &_clients[idx];
  WiFiClient* cl = &c->client;
  char path[64];
  char query[128] = "";
  {
    const char* p = c->handshake_buf;
    if (strncmp(p, "GET ", 4) != 0) p = "";
    else p += 4;
    size_t n = 0;
    while (p[n] && p[n] != ' ' && p[n] != '?' && n < sizeof(path) - 1) { path[n] = p[n]; n++; }
    path[n] = '\0';
    if (p[n] == '?') {
      const char* q = p + n + 1;
      size_t m = 0;
      while (q[m] && q[m] != ' ' && m < sizeof(query) - 1) { query[m] = q[m]; m++; }
      query[m] = '\0';
    }
  }

  const bool term = g_web_mirror.terminalOn();
  const bool screen = g_web_mirror.enabled();
  const bool settings = CAP_WEB_CONSOLE_SETTINGS && term;   // not on every board (device_caps.h)
#if WADA_WEB_FILE_TRANSFER
  const bool files = term || g_web_file_transfer.enabled();
#else
  const bool files = false;
#endif
  const ConsolePage pages[] = {
    { "/",           WC_INDEX_GZ,  WC_INDEX_GZ_LEN },
    { "/index.html", WC_INDEX_GZ,  WC_INDEX_GZ_LEN },
    { "/mesh",       WC_MESH_GZ,   WC_MESH_GZ_LEN },
    { "/screen",     WC_SCREEN_GZ, WC_SCREEN_GZ_LEN },
    { "/files",      WC_FILES_GZ,  WC_FILES_GZ_LEN },
#if CAP_WEB_CONSOLE_SETTINGS
    { "/settings",   WC_SETTINGS_GZ, WC_SETTINGS_GZ_LEN },
    { "/dashboard",  WC_DASHBOARD_GZ, WC_DASHBOARD_GZ_LEN },
#endif
  };
  const bool on[] = { term || screen || files, term || screen || files, term, screen, files, settings, settings };

  char hdr[400];
  for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
    if (strcmp(path, pages[i].path) != 0) continue;
    if (!on[i]) {
#if WADA_WEB_FILE_TRANSFER
      const char* off = (i == 4) ? WS_HTTP_FILES_DISABLED : WS_HTTP_UI_DISABLED;
#else
      const char* off = WS_HTTP_UI_DISABLED;
#endif
      writeAllBytes(*cl, reinterpret_cast<const uint8_t*>(off), strlen(off), TCP_WRITE_TIMEOUT_MS);
      return false;
    }
    const int n = snprintf(hdr, sizeof hdr,
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: text/html; charset=utf-8\r\n"
      "Content-Encoding: gzip\r\n"
      "Content-Length: %u\r\n"
      "Cache-Control: no-store\r\n"
      "Content-Security-Policy: default-src 'self'; connect-src 'self' ws: wss:; "
      "style-src 'unsafe-inline'; script-src 'unsafe-inline'; img-src 'self' data: blob:; "
      "frame-ancestors 'self'\r\n"
      "X-Content-Type-Options: nosniff\r\n"
      "Connection: close\r\n\r\n", (unsigned)pages[i].len);
    // The headers fit one probed write; the body is queued for drainPage, which pushes
    // only what the socket takes now. A blocking write here held the main loop for as
    // long as the browser took to ACK (#854 review: up to 1.5 s per page).
    if (writeAllBytes(*cl, reinterpret_cast<const uint8_t*>(hdr), (size_t)n, TCP_WRITE_TIMEOUT_MS)) {
      c->page_ptr = pages[i].gz;
      c->page_len = (uint32_t)pages[i].len;
      c->page_sent = 0;
      c->page_ms = millis();
    }
    return true;
  }

  int status = 404;
  char body[96] = "";
  if (strcmp(path, "/api/info") == 0) {
    status = 200;
    snprintf(body, sizeof body, "{\"dashboard\":%s,\"mesh\":%s,\"screen\":%s,\"files\":%s,\"settings\":%s}",
             settings ? "true" : "false", term ? "true" : "false", screen ? "true" : "false",
             files ? "true" : "false", settings ? "true" : "false");
  }
#if WADA_WEB_FILE_TRANSFER
  // Files tab: start File Transfer for an approved browser (the device would otherwise
  // need its own File Transfer page opened first). Its socket is refused until then, so
  // this is also where a Files-only page asks for approval (202 = waiting; poll again)
  // or uses the code. Query: id=<32 hex>&l=<label>[&code=nnnnnn].
  // Only the console's own page may call it: it sends X-Wada-Console, which a page on
  // another site cannot add without a CORS preflight (refused here), and a cross-origin
  // Origin is refused outright. A plain GET from elsewhere (an <img>, a no-cors fetch)
  // carries neither header and so cannot raise an approval card.
  else if (strcmp(path, "/api/files") == 0) {
    char id[33] = "", label[24] = "", code[7] = "";
    queryParam(query, "id", id, sizeof id);
    queryParam(query, "l", label, sizeof label);
    queryParam(query, "code", code, sizeof code);
    uint8_t key[16];
    size_t xl = 0;
    if (!term) status = 404;
    else if (crossOrigin(c->handshake_buf) || !httpHeader(c->handshake_buf, "X-Wada-Console", &xl))
      status = 403;
    else if (!consoleKey(id, strlen(id), key)) status = 403;
    else if (code[0]) {
      const bool allowed = g_web_auth.allowed();
      if (allowed && strlen(code) == 6 && g_web_auth.matches(reinterpret_cast<const uint8_t*>(code), 6)) {
        g_web_auth.noteSuccess();
        g_web_session.allowSession(key);
        status = 200;
      } else {
        if (allowed) g_web_auth.noteFailure();
        status = 403;
      }
    } else {
      switch (g_web_session.hello(key, label, clientIp(c->client))) {
        case WebSessionAuth::OK:   status = 200; break;
        case WebSessionAuth::WAIT: status = 202; break;
        default:                   status = 403; break;
      }
    }
    if (status == 200 && !g_web_file_transfer.enabled()) g_web_file_transfer.requestStart();
    strcpy(body, status == 200 ? "{\"ok\":true}" : status == 202 ? "{\"wait\":true}" : "{\"ok\":false}");
  }
#endif
  const char* reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 403 ? "Forbidden" : "Not Found";
  const int n = snprintf(hdr, sizeof hdr,
    "HTTP/1.1 %d %s\r\n"
    "Content-Type: %s\r\n"
    "Content-Length: %u\r\n"
    "Cache-Control: no-store\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "Connection: close\r\n\r\n%s",
    status, reason, body[0] ? "application/json" : "text/plain",
    (unsigned)strlen(body), body);
  writeAllBytes(*cl, reinterpret_cast<const uint8_t*>(hdr), (size_t)n, TCP_WRITE_TIMEOUT_MS);
  return status == 200;
}

// Push the rest of a console page from flash as far as the socket takes it right now,
// one segment per probed write, from the main loop (pollRecvFrame -> doHandshake). The
// slot is freed once the body has left, the peer has gone, or WS_PAGE_SEND_MS is up.
void WebSocketCompanionServer::drainPage(int idx) {
  WSClientState* c = &_clients[idx];
  while (c->page_sent < c->page_len && c->client.connected() && socketWritableNow(c->client)) {
    size_t chunk = c->page_len - c->page_sent;
    if (chunk > WS_PAGE_CHUNK) chunk = WS_PAGE_CHUNK;
    const size_t n = c->client.write(c->page_ptr + c->page_sent, chunk);
    if (n == 0) break;
    c->page_sent += (uint32_t)n;
#if defined(HAS_TDISPLAY_P4)
    break;   // AT transport: each write is a worker round-trip; one per pass (as drainClientTx)
#endif
  }
  if (c->page_sent >= c->page_len || !c->client.connected() ||
      (uint32_t)(millis() - c->page_ms) > WS_PAGE_SEND_MS) {
    c->page_ptr = nullptr;
    c->client.stop();
    c->in_use = false;
  }
}

// Read masked WebSocket binary frames from a mirror client and dispatch pointer
// events. Reuses this client's ws_* parse state (pollRecvFrame skips mirror
// clients, so it is exclusively ours). Payload: [0x01, x_lo,x_hi, y_lo,y_hi, pressed].
void WebSocketCompanionServer::drainMirrorInput(int idx, WebMirror& m) {
  WSClientState* c = &_clients[idx];
  WiFiClient* cl = &c->client;
  int guard = 0;
  // Before auth, take the next frame only once the previous AUTH reply has been sent, so a
  // correct code is never acknowledged into a full tx buffer (the reply would be dropped).
  while (cl->available() && guard++ < 1024 && (c->authed || c->tx_len == 0)) {
    switch (c->ws_state) {
      case WS_STATE_HEADER_0: {
        uint8_t b = (uint8_t)cl->read();
        c->ws_opcode = b & 0x0F;
        c->ws_state = WS_STATE_HEADER_1;
        break;
      }
      case WS_STATE_HEADER_1: {
        uint8_t b = (uint8_t)cl->read();
        uint8_t l7 = b & 0x7F;
        c->ws_payload_read = 0;
        c->comp_rx_len = 0;
        if (l7 == 126 || l7 == 127) { c->ws_payload_len = l7; c->ws_state = WS_STATE_LEN_EXT; }
        else { c->ws_payload_len = l7; c->ws_state = WS_STATE_MASK; }
        break;
      }
      case WS_STATE_LEN_EXT: {
        if (c->ws_payload_len == 126) {
          if (cl->available() < 2) return;
          uint8_t hi = (uint8_t)cl->read(), lo = (uint8_t)cl->read();
          c->ws_payload_len = ((uint16_t)hi << 8) | lo;
        } else {
          if (cl->available() < 8) return;
          c->ws_payload_len = 0;
          for (int i = 0; i < 8; i++) c->ws_payload_len = (c->ws_payload_len << 8) | (uint8_t)cl->read();
        }
        c->ws_state = (c->ws_payload_len == 0) ? WS_STATE_HEADER_0 : WS_STATE_MASK;
        break;
      }
      case WS_STATE_MASK: {
        if (cl->available() < 4) return;
        for (int i = 0; i < 4; i++) c->ws_mask[i] = (uint8_t)cl->read();
        c->ws_state = (c->ws_payload_len == 0) ? WS_STATE_HEADER_0 : WS_STATE_PAYLOAD;
        break;
      }
      default: {  // WS_STATE_PAYLOAD
        uint8_t b = (uint8_t)cl->read() ^ c->ws_mask[c->ws_payload_read % 4];
        if (c->comp_rx_len < MAX_FRAME_SIZE) c->comp_rx_buf[c->comp_rx_len++] = b;
        c->ws_payload_read++;
        if (c->ws_payload_read >= c->ws_payload_len) {
          if (!c->authed && c->ws_opcode != 0x08) {
            authFrame(idx, c->comp_rx_buf, c->comp_rx_len);
          } else if (c->ws_opcode == 0x02 && c->comp_rx_len >= 1) {
            const uint8_t ty = c->comp_rx_buf[0];
            if (ty == 0x01 && c->comp_rx_len >= 6) {          // pointer: [0x01, x,y (LE16), pressed]
              int16_t x = (int16_t)(c->comp_rx_buf[1] | (c->comp_rx_buf[2] << 8));
              int16_t y = (int16_t)(c->comp_rx_buf[3] | (c->comp_rx_buf[4] << 8));
              m.pushPointer(x, y, c->comp_rx_buf[5]);
            } else if (ty == 0x02 && c->comp_rx_len >= 3) {   // key: [0x02, codepoint LE16]
              m.pushKey((uint16_t)(c->comp_rx_buf[1] | (c->comp_rx_buf[2] << 8)));
            } else if (ty == 0x04 && c->comp_rx_len >= 2) {   // orientation: [0x04, want_landscape]
              m.requestOrient(c->comp_rx_buf[1] ? 1 : 2);     // 1=landscape, 2=portrait; UI thread reboots into it
            } else if (ty == 0x05) {                          // exit remote mode
              m.requestExit();
            } else if (ty == 0x06) {                          // unlock the screen (#506)
              m.requestUnlock();
            }
          } else if (c->ws_opcode == 0x08) {   // client close
            disconnectClient(idx);
            return;
          }
          c->ws_state = WS_STATE_HEADER_0;
        }
        break;
      }
    }
  }
}

// Parse a terminal client's masked WS frames; each complete text/binary frame is one
// command line -> m.pushTermCmd. Reuses this client's ws_* parse state (pollRecvFrame
// skips is_term clients, so it is exclusively ours).
void WebSocketCompanionServer::drainTermInput(int idx, WebMirror& m) {
  WSClientState* c = &_clients[idx];
  WiFiClient* cl = &c->client;
  int guard = 0;
  while (cl->available() && guard++ < 2048 && (c->authed || c->tx_len == 0)) {   // as drainMirrorInput
    switch (c->ws_state) {
      case WS_STATE_HEADER_0: { uint8_t b = (uint8_t)cl->read(); c->ws_opcode = b & 0x0F; c->ws_state = WS_STATE_HEADER_1; break; }
      case WS_STATE_HEADER_1: {
        uint8_t b = (uint8_t)cl->read(); uint8_t l7 = b & 0x7F; c->ws_payload_read = 0; c->comp_rx_len = 0;
        if (l7 == 126 || l7 == 127) { c->ws_payload_len = l7; c->ws_state = WS_STATE_LEN_EXT; }
        else                        { c->ws_payload_len = l7; c->ws_state = WS_STATE_MASK; }
        break;
      }
      case WS_STATE_LEN_EXT: {
        if (c->ws_payload_len == 126) { if (cl->available() < 2) return; uint8_t hi = (uint8_t)cl->read(), lo = (uint8_t)cl->read(); c->ws_payload_len = ((uint16_t)hi << 8) | lo; }
        else                          { if (cl->available() < 8) return; c->ws_payload_len = 0; for (int i = 0; i < 8; i++) c->ws_payload_len = (c->ws_payload_len << 8) | (uint8_t)cl->read(); }
        c->ws_state = (c->ws_payload_len == 0) ? WS_STATE_HEADER_0 : WS_STATE_MASK; break;
      }
      case WS_STATE_MASK: {
        if (cl->available() < 4) return;
        for (int i = 0; i < 4; i++) c->ws_mask[i] = (uint8_t)cl->read();
        c->ws_state = (c->ws_payload_len == 0) ? WS_STATE_HEADER_0 : WS_STATE_PAYLOAD; break;
      }
      default: {  // WS_STATE_PAYLOAD
        uint8_t b = (uint8_t)cl->read() ^ c->ws_mask[c->ws_payload_read % 4];
        if (c->comp_rx_len < MAX_FRAME_SIZE) c->comp_rx_buf[c->comp_rx_len++] = b;
        c->ws_payload_read++;
        if (c->ws_payload_read >= c->ws_payload_len) {
          if (!c->authed && c->ws_opcode != 0x08) {
            authFrame(idx, c->comp_rx_buf, c->comp_rx_len);
          } else if ((c->ws_opcode == 0x01 || c->ws_opcode == 0x02) && c->comp_rx_len > 0) {   // text/binary = a command line
            uint16_t n = c->comp_rx_len;
            while (n > 0 && (c->comp_rx_buf[n - 1] == '\r' || c->comp_rx_buf[n - 1] == '\n')) n--;
            if (n > 0) m.pushTermCmd((const char*)c->comp_rx_buf, n);
          } else if (c->ws_opcode == 0x08) { disconnectClient(idx); return; }
          c->ws_state = WS_STATE_HEADER_0;
        }
        break;
      }
    }
  }
}

#if WADA_WEB_FILE_TRANSFER
bool WebSocketCompanionServer::sendFileReply(int idx, const char* text) {
  if (!text || !text[0]) return false;
  const size_t len = strlen(text);
  return writeBinaryFrame(idx, reinterpret_cast<const uint8_t*>(text), len) == len;
}

// Parse one file-transfer socket. Browser frames are intentionally stop-and-wait:
// BEGIN -> READY, each binary chunk -> ACK, END -> DONE. That makes the bounded
// bridge backpressure explicit and prevents the network task from outrunning SD.
void WebSocketCompanionServer::drainFileInput(int idx) {
  static constexpr uint32_t kFrameTimeoutMs = 5000;
  WSClientState* c = &_clients[idx];
  WiFiClient* cl = &c->client;
  if (c->ws_state != WS_STATE_HEADER_0 &&
      millis() - c->files_frame_started_ms > kFrameTimeoutMs) {
    disconnectClient(idx);
    return;
  }
  if (!c->files_rx_buf) {
    c->files_rx_buf = static_cast<uint8_t*>(heap_caps_malloc(
        WebFileTransfer::MAX_INBOUND_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!c->files_rx_buf)
      c->files_rx_buf = static_cast<uint8_t*>(malloc(WebFileTransfer::MAX_INBOUND_BYTES));
    if (!c->files_rx_buf) {
      c->client.stop();
      c->in_use = false;
      return;
    }
  }

  int guard = 0;
  while (cl->available() && guard++ < 4096) {
    switch (c->ws_state) {
      case WS_STATE_HEADER_0: {
        const uint8_t b = static_cast<uint8_t>(cl->read());
        c->files_frame_started_ms = millis();
        const bool final_frame = (b & 0x80) != 0;
        const bool reserved_bits = (b & 0x70) != 0;
        c->ws_opcode = b & 0x0F;
        if (!final_frame || reserved_bits ||
            (c->ws_opcode != 0x01 && c->ws_opcode != 0x02 && c->ws_opcode != 0x08)) {
          disconnectClient(idx);
          return;
        }
        c->ws_state = WS_STATE_HEADER_1;
        break;
      }
      case WS_STATE_HEADER_1: {
        const uint8_t b = static_cast<uint8_t>(cl->read());
        if ((b & 0x80) == 0) {   // RFC 6455: every client-to-server frame is masked
          disconnectClient(idx);
          return;
        }
        const uint8_t len7 = b & 0x7F;
        if (c->ws_opcode == 0x08 && len7 >= 126) {
          disconnectClient(idx);
          return;
        }
        c->ws_payload_read = 0;
        c->files_rx_len = 0;
        if (len7 == 127) {
          disconnectClient(idx);
          return;
        }
        if (len7 == 126) {
          c->ws_payload_len = len7;
          c->ws_state = WS_STATE_LEN_EXT;
        } else {
          c->ws_payload_len = len7;
          c->ws_state = WS_STATE_MASK;
        }
        break;
      }
      case WS_STATE_LEN_EXT: {
        if (c->ws_payload_len == 126) {
          if (cl->available() < 2) return;
          const uint8_t hi = static_cast<uint8_t>(cl->read());
          const uint8_t lo = static_cast<uint8_t>(cl->read());
          c->ws_payload_len = (static_cast<uint16_t>(hi) << 8) | lo;
        }
        if (c->ws_payload_len < 126 ||
            c->ws_payload_len > WebFileTransfer::MAX_INBOUND_BYTES) {
          disconnectClient(idx);
          return;
        }
        c->ws_state = WS_STATE_MASK;
        break;
      }
      case WS_STATE_MASK: {
        if (cl->available() < 4) return;
        for (int i = 0; i < 4; ++i) c->ws_mask[i] = static_cast<uint8_t>(cl->read());
        if (c->ws_payload_len == 0) {
          c->files_frame_started_ms = 0;
          if (c->ws_opcode == 0x08) {
            disconnectClient(idx);
            return;
          }
          c->ws_state = WS_STATE_HEADER_0;
        } else {
          c->ws_state = WS_STATE_PAYLOAD;
        }
        break;
      }
      default: {
        const uint8_t value = static_cast<uint8_t>(cl->read()) ^
                              c->ws_mask[c->ws_payload_read % 4];
        if (c->files_rx_len < WebFileTransfer::MAX_INBOUND_BYTES)
          c->files_rx_buf[c->files_rx_len++] = value;
        c->ws_payload_read++;
        if (c->ws_payload_read < c->ws_payload_len) break;

        if (c->ws_opcode == 0x08) {
          disconnectClient(idx);
          return;
        }
        if (c->ws_payload_len > WebFileTransfer::MAX_INBOUND_BYTES) {
          if (c->tx_len == 0) sendFileReply(idx, "ERR frame too large");
        } else if (!c->authed) {
          authFrame(idx, c->files_rx_buf, c->files_rx_len);
        } else if ((c->ws_opcode == 0x01 || c->ws_opcode == 0x02) &&
                   c->files_rx_len > 0) {
          if (g_web_file_transfer.pushInbound(c->ws_opcode, c->files_rx_buf,
                                               c->files_rx_len)) {
            c->files_request_pending = true;
          } else if (c->tx_len == 0) {
            sendFileReply(idx, "ERR device busy");
          }
        }
        c->ws_state = WS_STATE_HEADER_0;
        c->files_frame_started_ms = 0;
        return;   // one complete application frame per service pass
      }
    }
  }
}

void WebSocketCompanionServer::serviceFileClients() {
  int clients = 0;
  int active = -1;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; ++i) {
    WSClientState* c = &_clients[i];
    if (!c->in_use || !c->client.connected() || !c->handshake_done || !c->is_files) continue;
    clients++;
    active = i;
    drainClientTx(i);
    if (!c->in_use) continue;
    if (!g_web_file_transfer.enabled()) {
      if (c->tx_len == 0) sendFileReply(i, "ERR session ended");
      c->close_after_tx = true;
    } else if (!c->files_request_pending && c->tx_len == 0) {
      drainFileInput(i);
    }
    if (c->in_use && c->close_after_tx && c->tx_len == 0) disconnectClient(i);
  }
  g_web_file_transfer.noteClients(clients);

  if (active < 0 || !_clients[active].in_use || !_clients[active].authed ||
      _clients[active].tx_len != 0) return;
  uint8_t reply[WebFileTransfer::MAX_REPLY_BYTES];
  const size_t len = g_web_file_transfer.popReply(reply, sizeof reply);
  if (len > 0) {
    if (writeBinaryFrame(active, reply, len) == len) {
      _clients[active].files_request_pending = false;
    } else {
      char retry[WebFileTransfer::MAX_REPLY_BYTES];
      memcpy(retry, reply, len);
      retry[len] = '\0';
      g_web_file_transfer.pushReply(retry);
    }
    return;
  }
  if (!_mirror_txbuf) return;
  const size_t data_len = g_web_file_transfer.popData(
      _mirror_txbuf, WebFileTransfer::MAX_OUTBOUND_BYTES);
  if (data_len > 0) {
    if (writeBinaryFrame(active, _mirror_txbuf, data_len) == data_len) {
      _clients[active].files_request_pending = false;
    } else {
      g_web_file_transfer.pushData(_mirror_txbuf, data_len);
    }
  }
}
#endif

// Web mesh terminal: shuttle text both ways for /term clients (independent of the
// framebuffer mirror; a client is never both). Called from serviceMirror on the stream
// task with _client_mtx already held. writeBinaryFrame's per-client tx_buf keeps it
// non-blocking; the browser TextDecodes the reply bytes.
void WebSocketCompanionServer::serviceTerminalClients(WebMirror& m) {
  int tc = 0;
  bool all_idle = true;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    WSClientState* c = &_clients[i];
    if (!c->in_use || !c->client.connected() || !c->handshake_done || !c->is_term) continue;
    drainClientTx(i);
    if (!c->in_use) continue;          // reaped mid-drain
    drainTermInput(i, m);              // before auth this only looks for "AUTH nnnnnn"
    if (!c->in_use || !c->authed) continue;
    tc++;
    if (c->tx_len != 0) all_idle = false;
  }
  m.noteTermClients(tc);
  if (tc == 0 || !all_idle || !_mirror_txbuf) return;   // wait until every client drained its previous chunk
  // Each frame is [type][payload]: type 0x01 = a complete JSON data message (Contacts/Chats),
  // 0x00 = a terminal text chunk. Prefer data (discrete + responsive) over the text stream.
  _mirror_txbuf[0] = 0x01;
  size_t n = m.popTermData(_mirror_txbuf + 1, WS_MIRROR_CLIENT_TXBUF - 8);
  if (n == 0) { _mirror_txbuf[0] = 0x00; n = m.popTermReply(_mirror_txbuf + 1, 2048); }
  if (n == 0) return;
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    WSClientState* c = &_clients[i];
    if (!c->in_use || !c->client.connected() || !c->handshake_done || !c->is_term || !c->authed) continue;
    writeBinaryFrame(i, _mirror_txbuf, n + 1);
  }
}

// Called every loop: refresh the client count for the producer gate, send each
// client its one-time screen-size meta, drain remote pointer input, and broadcast
// queued framebuffer bands. Strictly non-blocking (socketWritableNow-gated) so it
// never stalls the mesh loop (the beta_32 Wi-Fi-freeze discipline).
void WebSocketCompanionServer::serviceMirror(WebMirror& m) {
  WsClientsLock _lk(_client_mtx);          // held for the whole (quick, non-blocking) pass
  if (!_mirror_txbuf) {                    // shared scratch — also used by serviceTerminalClients for data frames
    _mirror_txbuf = (uint8_t*)heap_caps_malloc(WS_MIRROR_TXBUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_mirror_txbuf) _mirror_txbuf = (uint8_t*)malloc(WS_MIRROR_TXBUF);
  }
#if WADA_WEB_FILE_TRANSFER
  serviceFileClients();                    // authenticated browser upload channel
#endif
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    WSClientState* c = &_clients[i];
    if (!c->in_use || !c->handshake_done || !(c->is_mirror || c->is_term || c->is_files)) continue;
    // Drop a browser whose feature was switched off, that is done failing to authenticate,
    // or that never authenticated (it would hold one of the few client slots forever).
    // Ending the console sessions on the device (console off, End all sessions, a revoked
    // browser) bumps the generation: every browser let in before that is dropped.
    if ((c->is_mirror && !m.enabled()) || (c->is_term && !m.terminalOn()) ||
        (c->close_after_tx && c->tx_len == 0) ||
        (c->authed && c->auth_gen != g_web_session.generation()) ||
        (!c->authed && (uint32_t)(millis() - c->accept_ms) > WS_AUTH_TIMEOUT_MS)) {
      disconnectClient(i);
    } else if (!c->authed && c->client.connected()) {
      if (c->is_mirror) drainClientTx(i);    // flush the previous reply; terminal and files clients do this in their own pass
      if (c->in_use) pollApproval(i);
      if (c->in_use && c->is_mirror) drainMirrorInput(i, m);
    }
  }
  serviceTerminalClients(m);               // web mesh terminal + Contacts/Chats data (independent of the framebuffer)
  const int mc = mirrorClientCount();
  m.noteClients(mc);
  if (mc == 0 || !_mirror_txbuf) return;

  // Per-client: first push any bytes still pending from the previous frame (non-blocking),
  // then queue the one-time size meta + keyboard-focus changes (only while idle, so on-wire
  // frame order is preserved), then drain remote input.
  bool kf = false;
  const bool kb_changed = m.kbTake(&kf);
  for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
    WSClientState* c = &_clients[i];
    if (!c->in_use || !c->client.connected() || !c->handshake_done || !c->is_mirror || !c->authed) continue;
    drainClientTx(i);
    if (!c->in_use) continue;                 // a wedged client may have just been reaped
    if (c->tx_len == 0 && !c->meta_sent) {
      uint8_t meta[6] = { 0x02,
        (uint8_t)(m.screenW() & 0xFF), (uint8_t)(m.screenW() >> 8),
        (uint8_t)(m.screenH() & 0xFF), (uint8_t)(m.screenH() >> 8),
        (uint8_t)(m.remote() ? 1 : 0) };   // byte5 bit0 = remote mode -> browser shows the Rotate button
      if (writeBinaryFrame(i, meta, 6) == 6) { c->meta_sent = true; m.requestFullRepaint(); }
    }
    if (kb_changed && c->tx_len == 0) { uint8_t km[2] = { 0x03, (uint8_t)(kf ? 1 : 0) }; writeBinaryFrame(i, km, 2); }
    // Lock state, tracked per browser so a late joiner or a busy link still gets it.
    const uint8_t lk = m.locked() ? 1 : 0;
    if (c->meta_sent && c->tx_len == 0 && c->lock_sent != lk) {
      uint8_t lm[2] = { 0x06, lk };
      if (writeBinaryFrame(i, lm, 2) == 2) c->lock_sent = lk;
    }
    drainMirrorInput(i, m);
  }

  // Broadcast queued display bands: pop the next band only once EVERY mirror client has
  // fully drained its previous frame (keeps all clients byte-synced + backpressures to the
  // slowest peer). writeBinaryFrame buffers + pushes non-blocking, so a slow link just
  // drains across later calls instead of blocking the loop or dropping the socket.
  size_t budget = WS_MIRROR_TX_BUDGET;
  while (budget > 0) {
    bool any = false, all_idle = true;
    for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
      WSClientState* c = &_clients[i];
      if (!c->in_use || !c->client.connected() || !c->handshake_done || !c->is_mirror || !c->authed) continue;
      any = true;
      if (c->tx_len != 0) { all_idle = false; break; }
    }
    if (!any || !all_idle) break;
    size_t n = m.popFrame(_mirror_txbuf, WS_MIRROR_TXBUF);
    if (n == 0) break;
    for (int i = 0; i < WS_COMPANION_MAX_CLIENTS; i++) {
      WSClientState* c = &_clients[i];
      if (!c->in_use || !c->client.connected() || !c->handshake_done || !c->is_mirror || !c->authed) continue;
      writeBinaryFrame(i, _mirror_txbuf, n);
    }
    budget = (n < budget) ? (budget - n) : 0;
  }
}
