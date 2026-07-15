#pragma once

#include <helpers/BaseSerialInterface.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#if defined(HAS_TDISPLAY_P4)
  // Same type rebind as TCPCompanionServer.h (every TU must see one consistent class layout).
  // NB: begin() is never called on the P4 — ESP-AT has a single listening port, so a first-byte
  // router in MultiTransportCompanionInterface accepts everything on the companion port and
  // hands HTTP/WS connections here via adoptClient() (_port stays 0; pause/resumeListen no-op).
  #include <C6WifiShim.h>
  #include <C6Socket.h>
  #define WiFiClient C6Client
  #define WiFiServer C6Server
#endif

class WebMirror;   // web-UI mirror bridge (WebMirror.h); included in the .cpp

#ifndef WS_COMPANION_MAX_CLIENTS
#define WS_COMPANION_MAX_CLIENTS  2
#endif

#ifndef WS_HANDSHAKE_MAX_LEN
#define WS_HANDSHAKE_MAX_LEN  1536
#endif

#ifndef WS_HANDSHAKE_TIMEOUT_MS
#define WS_HANDSHAKE_TIMEOUT_MS  5000
#endif

// Per-client: HTTP upgrade handshake or WebSocket mode with companion state machine (plain ws:// only).
struct WSClientState {
  mutable WiFiClient client;
  bool in_use;
  uint32_t accept_ms;

  bool handshake_done;
  uint16_t handshake_len;
  char handshake_buf[WS_HANDSHAKE_MAX_LEN];

  uint8_t ws_state;
  uint8_t ws_opcode;
  uint64_t ws_payload_len;
  uint32_t ws_payload_read;
  uint8_t ws_mask[4];

  uint8_t comp_state;
  uint16_t comp_frame_len;
  uint16_t comp_rx_len;
  uint8_t comp_rx_buf[MAX_FRAME_SIZE];
  uint32_t stall_ms;   // start of a continuous write-failure run; 0 = healthy

  bool is_mirror;      // this client is a web-UI mirror (GET /mirror), not a companion peer
  bool is_term;        // this client is a web mesh terminal (GET /term)
  bool meta_sent;      // mirror: the one-time screen-size meta frame has been sent

  // Mirror TX buffer: one WS frame (header + payload) queued for this client, drained
  // NON-BLOCKING across loop iterations. The loop never spins on a socket write and a
  // transient stall never drops the client — it just drains at the link's pace.
  uint8_t* tx_buf;     // lazily-allocated PSRAM frame buffer (null until first mirror use)
  uint16_t tx_len;     // bytes queued in tx_buf (0 = idle, ready for the next frame)
  uint16_t tx_sent;    // bytes already pushed to the socket
};

// WebSocket server for companion protocol. Same logical protocol as TCP; transport is RFC 6455 (plain WS).
class WebSocketCompanionServer {
public:
  WebSocketCompanionServer();

  void begin(uint16_t port);
  void stop();
  /** Close only the listen socket; existing WebSocket clients stay connected. */
  void pauseListen();
  /** Re-open listen socket after pauseListen(); no-op if already listening. */
  void resumeListen();

  size_t pollRecvFrame(uint8_t dest[], int* client_index_out);
  /** Accept new clients and prune disconnects; call from main loop for timely handshakes. */
  void tickHandshake();
  /** Slot-insert an already-accepted connection (evicts the oldest handshaking client if
   *  full). Lets an external router (T-Display P4: one shared AT listener) hand us
   *  HTTP/WS clients that were accepted on the companion port. */
  void adoptClient(WiFiClient& incoming);

  size_t writeToClient(int client_index, const uint8_t src[], size_t len);
  size_t writeToAllClients(const uint8_t src[], size_t len);

  bool isClientConnected(int client_index) const;
  int connectedCount() const;                 // companion clients only (mirror excluded)
  void disconnectClient(int client_index);

  // ---- Web UI mirror (see WebMirror) ----
  int mirrorClientCount() const;              // handshaken GET /mirror clients
  /** Drain remote pointer input + push queued framebuffer frames to mirror
   *  clients. Non-blocking (socketWritableNow-gated); call each loop. */
  void serviceMirror(WebMirror& m);
  /** WebSocket binary frame with len up to 65535 (no MAX_FRAME_SIZE cap) — used
   *  for mirror display bands. Same freeze-safe write + wedged reaper as the
   *  companion path. */
  size_t writeBinaryFrame(int client_index, const uint8_t src[], size_t len);

private:
  WiFiServer _server;
  mutable WSClientState _clients[WS_COMPANION_MAX_CLIENTS];
  uint16_t _port;
  bool _listening;
  int _poll_start_idx;

  void acceptNewClients();
  void pruneDisconnected();
  bool doHandshake(int idx);

  uint8_t* _mirror_txbuf;              // lazily-allocated PSRAM buffer for popped mirror frames
  void drainMirrorInput(int idx, WebMirror& m);
  void drainClientTx(int idx);        // push a mirror client's pending tx_buf bytes, non-blocking
  void serviceTerminalClients(WebMirror& m);   // web mesh terminal: reply text out + command text in
  void drainTermInput(int idx, WebMirror& m);  // parse a term client's WS frames -> m.pushTermCmd

  // The _clients array is now touched by TWO cores: the main loop (accept/handshake +
  // companion RX/TX, core 1) and the dedicated mirror stream task (serviceMirror, core 0).
  // A recursive mutex serialises every _clients access; held only for the quick,
  // non-blocking work, so cross-core contention is negligible.
  SemaphoreHandle_t _client_mtx;
};
