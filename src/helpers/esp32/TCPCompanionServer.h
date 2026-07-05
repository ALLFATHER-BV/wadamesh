#pragma once

#include <helpers/BaseSerialInterface.h>
#include <WiFi.h>

#ifndef TCP_COMPANION_MAX_CLIENTS
#define TCP_COMPANION_MAX_CLIENTS  3
#endif

// Per-client receive state (same frame protocol as serial: '<' + len_lo + len_hi + payload)
struct TCPClientState {
  WiFiClient client;
  uint8_t state;       // 0=idle, 1=got '<', 2=got len_lo, 3=reading payload
  uint16_t frame_len;
  uint16_t rx_len;
  uint8_t rx_buf[MAX_FRAME_SIZE];
  bool in_use;
  uint32_t stall_ms;   // start of a continuous write-failure run; 0 = healthy
};

// Multi-client TCP server for companion protocol. Same frame format as serial.
// Does not implement BaseSerialInterface; used inside MultiTransportCompanionInterface.
class TCPCompanionServer {
public:
  TCPCompanionServer();

  void begin(uint16_t port = 5000);
  void stop();

  // Poll all clients; if any has a complete frame, copy to dest and return (len, client_index).
  // client_index is 0..TCP_COMPANION_MAX_CLIENTS-1. Returns 0 if no frame ready.
  size_t pollRecvFrame(uint8_t dest[], int* client_index_out);

  // Send frame to one client (same wire format: '>' + len_lo + len_hi + payload).
  size_t writeToClient(int client_index, const uint8_t src[], size_t len);

  // Send frame to all connected TCP clients.
  // Returns len when all currently connected clients were written successfully,
  // otherwise returns 0.
  size_t writeToAllClients(const uint8_t src[], size_t len);

  bool isClientConnected(int client_index) const;
  int connectedCount() const;
  void disconnectClient(int client_index);

private:
  WiFiServer _server;
  mutable TCPClientState _clients[TCP_COMPANION_MAX_CLIENTS];
  uint16_t _port;
  int _poll_start_idx;

  void acceptNewClients();
  void pruneDisconnected();
};
