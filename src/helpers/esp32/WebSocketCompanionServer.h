#pragma once

#include <helpers/BaseSerialInterface.h>
#include <WiFi.h>

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

  size_t writeToClient(int client_index, const uint8_t src[], size_t len);
  size_t writeToAllClients(const uint8_t src[], size_t len);

  bool isClientConnected(int client_index) const;
  int connectedCount() const;
  void disconnectClient(int client_index);

private:
  WiFiServer _server;
  mutable WSClientState _clients[WS_COMPANION_MAX_CLIENTS];
  uint16_t _port;
  bool _listening;
  int _poll_start_idx;

  void acceptNewClients();
  void pruneDisconnected();
  bool doHandshake(int idx);
};
