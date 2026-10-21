// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// CompanionTransport: the byte pipe between CompanionBackend and a MeshCore companion
// node. The backend works in whole companion FRAMES (the payloads handleCmdFrame() reads
// and writeFrame() writes); each transport owns its own framing on the wire:
//
//   transport            host -> node                    node -> host
//   ------------------   -----------------------------   -----------------------------
//   USB serial, TCP      '<' len_lo len_hi payload        '>' len_lo len_hi payload
//                        (a byte stream: StreamDeframer finds the frames, skipping any
//                         log text the firmware prints on the same port)
//   WebSocket (8765)     '<' len_lo len_hi payload        payload (one WS binary message
//                        inside ONE binary WS message     per frame, no header)
//   BLE (Nordic UART)    payload, written to the RX       payload, one notification of
//                        characteristic                   the TX characteristic per frame
//
// Implementations:
//   SerialTransport.cpp   native: POSIX termios (`--serial /dev/cu.usbmodemXXX`)
//   TcpTransport.cpp      native: a TCP socket (`--tcp host:5000`)
//   BrowserTransport.cpp  Emscripten: Web Serial / Web Bluetooth / WebSocket, with the
//                         browser APIs in web/mesh/js/companion-transports.js
//
// Threading: everything happens on the UI thread. poll() is called from the backend's
// loop(); a transport that receives on another thread (the browser's event loop, an OS
// callback) buffers there and delivers from poll().
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vector>

class CompanionTransportListener {
public:
  virtual ~CompanionTransportListener() {}
  virtual void onTransportConnected() = 0;
  virtual void onTransportDisconnected(const char* why) = 0;
  // One complete companion frame from the node (first byte = RESP_CODE_* / PUSH_CODE_*).
  virtual void onTransportFrame(const uint8_t* frame, size_t len) = 0;
};

class CompanionTransport {
public:
  virtual ~CompanionTransport() {}
  virtual const char* name() const = 0;        // "serial", "tcp", "web-serial", "ble", "ws"
  virtual const char* describe() const = 0;    // human-readable endpoint ("/dev/cu.usbmodem101")
  void setListener(CompanionTransportListener* l) { _listener = l; }
  // Start connecting. Non-blocking; onTransportConnected() follows from poll().
  virtual bool open() = 0;
  virtual void close() = 0;
  virtual bool isConnected() const = 0;
  // Send ONE companion frame (the transport adds its framing). False = not sent.
  virtual bool sendFrame(const uint8_t* frame, size_t len) = 0;
  // Pump I/O and deliver events. Called every loop step.
  virtual void poll(uint32_t now_ms) = 0;
  // True when the transport reconnects by itself (native serial/TCP retry; the browser
  // ones need a user gesture to pick a device again, so they do not).
  virtual bool autoReconnect() const { return false; }

protected:
  CompanionTransportListener* _listener = nullptr;
};

// ---- framing helpers -----------------------------------------------------------------

// Largest frame either side writes (BaseSerialInterface MAX_FRAME_SIZE is 176; leave room).
static const size_t COMPANION_MAX_FRAME = 300;

// '<' + len16 LE + payload: what the node's serial / TCP / WS readers expect.
inline std::vector<uint8_t> companionEncodeHostFrame(const uint8_t* frame, size_t len) {
  std::vector<uint8_t> out(3 + len);
  out[0] = '<';
  out[1] = (uint8_t)(len & 0xFF);
  out[2] = (uint8_t)(len >> 8);
  if (len) memcpy(&out[3], frame, len);
  return out;
}

// Is `code` something a node writes as the first byte of a frame? Used to resync the
// stream deframer when firmware log text happens to contain a '>'.
inline bool companionPlausibleCode(uint8_t code) {
  return code <= 29 || code == 61 || (code >= 0x80 && code <= 0x90);
}

// Finds '>' len16 payload frames in a byte stream (serial / TCP). Bytes outside frames
// (boot log, debug prints) are skipped.
class StreamDeframer {
public:
  template <typename F>
  void feed(const uint8_t* data, size_t n, F&& on_frame) {
    _buf.insert(_buf.end(), data, data + n);
    size_t pos = 0;
    while (true) {
      while (pos < _buf.size() && _buf[pos] != '>') pos++;
      if (_buf.size() - pos < 4) break;   // need '>' + len + first payload byte
      const size_t len = (size_t)_buf[pos + 1] | ((size_t)_buf[pos + 2] << 8);
      if (len == 0 || len > COMPANION_MAX_FRAME || !companionPlausibleCode(_buf[pos + 3])) {
        pos++;                             // a '>' in log text: resync after it
        continue;
      }
      if (_buf.size() - pos < 3 + len) break;
      on_frame(&_buf[pos + 3], len);
      pos += 3 + len;
    }
    _buf.erase(_buf.begin(), _buf.begin() + (long)pos);
    if (_buf.size() > 4096) _buf.clear();  // runaway garbage: start over
  }
  void reset() { _buf.clear(); }

private:
  std::vector<uint8_t> _buf;
};

// Factories (each defined in its own file; the ones a platform lacks return nullptr).
CompanionTransport* wadaCreateSerialTransport(const char* device_path);   // native only
CompanionTransport* wadaCreateTcpTransport(const char* host, int port);    // native only
// Browser: kind = "web-serial", "ble" or "ws"; arg = the WS URL for "ws" (else unused).
CompanionTransport* wadaCreateBrowserTransport(const char* kind, const char* arg);
