// SPDX-License-Identifier: GPL-3.0-or-later
// C6Socket — P4-only Arduino-client facades over the c6_at AT+CIP socket layer.
//
// C6Client subclasses NetworkClient and overrides the Client virtuals (connect/read/write/available/
// peek/stop/connected...), so HTTPClient works UNCHANGED through its NetworkClient& — virtual dispatch
// lands here instead of lwIP (which has no netif on this board: the C6 owns TCP/IP). C6ClientSecure
// maps to AT+CIPSTART "SSL": the C6 does the TLS handshake ON-CHIP, which makes on-device HTTPS work
// on this board (the S3 boards never had the heap for it). C6Server fronts AT+CIPSERVER for the
// inbound companion TCP port (ESP-AT: ONE listening port).
//
// The link is SHARED between copies (WiFiClient semantics): TCPCompanionServer does
// `WiFiClient incoming = _server.accept(); _clients[i].client = incoming;` — with plain
// close-on-destroy the dying temporary would kill the freshly accepted link. A shared ref closes
// the link only when the LAST copy goes away; stop() hard-closes for all copies at once.
//
// Include AFTER <C6WifiShim.h> in a P4-gated block, then rebind the types:
//   #define WiFiClient        C6Client
//   #define WiFiClientSecure  C6ClientSecure
//   #define WiFiServer        C6Server
// (Done by the includer, so shared code paths pick the facade only on the P4.)
#pragma once
#include <WiFi.h>              // NetworkClient / IPAddress / String
#include <memory>
#include "c6_at.h"

struct C6LinkRef {             // owns one AT link id; last holder closes it
  int id;
  explicit C6LinkRef(int i) : id(i) {}
  ~C6LinkRef() { if (id >= 0) c6at_sock_close(id); }
};

class C6Client : public NetworkClient {
protected:
  std::shared_ptr<C6LinkRef> _ref;
  bool _ssl = false;
  int link() const { return _ref ? _ref->id : -1; }

public:
  C6Client() {}
  explicit C6Client(int accepted_link) {              // wrap a server-accepted link id
    if (accepted_link >= 0) _ref = std::make_shared<C6LinkRef>(accepted_link);
  }

  int connect(IPAddress ip, uint16_t port) { return connect(ip, port, 15000); }
  int connect(IPAddress ip, uint16_t port, int32_t timeout_ms) {
    char h[20];
    snprintf(h, sizeof(h), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return connect((const char *)h, port, timeout_ms);
  }
  int connect(const char *host, uint16_t port) { return connect(host, port, 15000); }
  int connect(const char *host, uint16_t port, int32_t timeout_ms) {
    stop();
    int id = c6at_sock_connect(host, port, _ssl, (uint32_t)timeout_ms);
    if (id >= 0) _ref = std::make_shared<C6LinkRef>(id);
    return id >= 0 ? 1 : 0;
  }

  size_t write(uint8_t b) { return write(&b, 1); }
  size_t write(const uint8_t *buf, size_t size) {
    int id = link();
    if (id < 0 || !buf || !size) return 0;
    return c6at_sock_send(id, buf, size, 30000) ? size : 0;
  }

  int available() { int id = link(); return id < 0 ? 0 : c6at_sock_available(id); }
  int read() {
    uint8_t b;
    return (read(&b, 1) == 1) ? (int)b : -1;
  }
  int read(uint8_t *buf, size_t size) {
    int id = link();
    if (id < 0) return -1;
    return c6at_sock_read(id, buf, size);   // 0 = nothing yet (caller's timeout loop retries)
  }
  int peek() { int id = link(); return id < 0 ? -1 : c6at_sock_peek(id); }
  void flush() {}   // writes are synchronous through the worker — nothing buffered host-side

  void stop() {     // hard close for ALL copies sharing this link (WiFiClient::stop semantics)
    if (_ref) {
      int id = _ref->id;
      _ref->id = -1;                 // other copies see a dead link; C6LinkRef won't double-close
      if (id >= 0) c6at_sock_close(id);
    }
    _ref.reset();
  }
  uint8_t connected() { int id = link(); return (id >= 0 && c6at_sock_connected(id)) ? 1 : 0; }
  operator bool() { return connected(); }

  void setNoDelay(bool) {}                // AT transport: no Nagle knob; sends are already framed
  virtual int fd() const { return -1; }   // no lwIP socket behind this client
};

class C6ClientSecure : public C6Client {
public:
  C6ClientSecure() { _ssl = true; }
  // TLS runs on the C6 with its own bundle; the app always used setInsecure() anyway.
  void setInsecure() {}
  void setCACert(const char *) {}
  void setCertificate(const char *) {}
  void setPrivateKey(const char *) {}
};

// Inbound listener facade (WiFiServer surface used by TCP/WS companion servers).
// c6_at has ONE global listener + accept FIFO, but multiple C6Server instances exist
// (TCP + WS companion servers each embed one; on the P4 only the router's instance ever
// begins). Gate everything on _begun so a never-begun instance is fully inert — matching
// lwIP WiFiServer semantics — instead of stealing accepts from / stopping the live one.
class C6Server {
  uint16_t _port = 0;
  bool _begun = false;

public:
  C6Server() {}
  explicit C6Server(uint16_t port) : _port(port) {}
  void begin(uint16_t port = 0) {
    if (port) _port = port;
    if (_port) {
      c6at_server_start(_port);   // async; the AT worker logs LISTENING/FAIL
      _begun = true;
    }
  }
  bool hasClient() { return _begun && c6at_server_pending() > 0; }
  C6Client accept() { return C6Client(_begun ? c6at_server_accept() : -1); }
  void stop() {
    if (_begun) {
      c6at_server_stop();
      _begun = false;
    }
  }
  void end() { stop(); }
  void close() { stop(); }
  operator bool() { return _begun; }
};
