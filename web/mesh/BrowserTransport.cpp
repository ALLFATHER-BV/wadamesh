// SPDX-License-Identifier: GPL-3.0-or-later
// Browser companion transports (Emscripten): Web Serial, Web Bluetooth, WebSocket.
//
// The browser APIs live in web/mesh/js/companion-transports.js (linked with --pre-js);
// it only moves bytes. The framing is here, in C++, as for the native transports:
//
//   kind         we hand JS                        JS hands us
//   web-serial   '<' len16 payload (bytes)         a byte stream ('>' len16 payload)
//   ws           '<' len16 payload (ONE message)   one frame per binary message
//   ble          payload (one write to NUS RX)     one frame per NUS TX notification
//
// JS -> C++ entry points are plain C functions (EMSCRIPTEN_KEEPALIVE, no
// EXPORTED_FUNCTIONS needed): wada_companion_js_alloc / _rx / _state. Received bytes
// are queued under a mutex and delivered from poll(), on the UI thread, so the glue is
// correct whether the page runs single-threaded or with pthreads.
#include "CompanionTransport.h"
#include <stdio.h>

#if !defined(__EMSCRIPTEN__)

CompanionTransport* wadaCreateBrowserTransport(const char*, const char*) { return nullptr; }

#else

#include <emscripten.h>
#include <mutex>
#include <stdlib.h>
#include <string>

// C++ -> JS (the functions WadaCompanion defines in companion-transports.js).
EM_JS(void, wada_js_companion_open, (int kind, const uint8_t* arg, int arg_len), {
  var a = new TextDecoder().decode(HEAPU8.slice(arg, arg + arg_len));
  WadaCompanion.open(["web-serial", "ble", "ws"][kind], a);
});
EM_JS(int, wada_js_companion_send, (const uint8_t* p, int n), {
  return WadaCompanion.send(HEAPU8.slice(p, p + n)) ? 1 : 0;
});
EM_JS(void, wada_js_companion_close, (), { WadaCompanion.close(); });

namespace {

enum Kind { K_SERIAL = 0, K_BLE = 1, K_WS = 2 };

class BrowserTransport;
BrowserTransport* s_active = nullptr;

class BrowserTransport : public CompanionTransport {
public:
  BrowserTransport(Kind k, const char* arg) : _kind(k), _arg(arg ? arg : "") {
    _desc = _kind == K_WS ? _arg : (_kind == K_BLE ? "Bluetooth" : "USB");
  }
  const char* name() const override { return _kind == K_SERIAL ? "web-serial" : _kind == K_BLE ? "ble" : "ws"; }
  const char* describe() const override { return _desc.c_str(); }
  bool open() override {
    s_active = this;
    // WS connects at once; Web Serial / Bluetooth reuse a device this page was granted
    // before, otherwise they wait for WadaCompanion.connect() from a user gesture.
    wada_js_companion_open((int)_kind, (const uint8_t*)_arg.data(), (int)_arg.size());
    return true;
  }
  void close() override { wada_js_companion_close(); }
  bool isConnected() const override { return _up; }
  bool sendFrame(const uint8_t* frame, size_t len) override {
    if (!_up) return false;
    if (_kind == K_BLE) return wada_js_companion_send(frame, (int)len) != 0;
    const std::vector<uint8_t> out = companionEncodeHostFrame(frame, len);
    return wada_js_companion_send(out.data(), (int)out.size()) != 0;
  }
  void poll(uint32_t now_ms) override {
    (void)now_ms;
    std::vector<std::vector<uint8_t>> rx;
    int state = -1;
    std::string why;
    {
      std::lock_guard<std::mutex> lk(_mu);
      rx.swap(_rx);
      if (_state_changed) { state = _pending_state; why = _why; _state_changed = false; }
    }
    if (state == 1 && !_up) {
      _up = true;
      _deframer.reset();
      fprintf(stderr, "[companion] %s %s: open\n", name(), describe());
      if (_listener) _listener->onTransportConnected();
    }
    for (auto& chunk : rx) {
      if (_kind == K_SERIAL) {
        _deframer.feed(chunk.data(), chunk.size(), [this](const uint8_t* f, size_t l) {
          if (_listener) _listener->onTransportFrame(f, l);
        });
      } else if (!chunk.empty() && _listener) {
        _listener->onTransportFrame(chunk.data(), chunk.size());
      }
    }
    if (state == 0 && _up) {
      _up = false;
      fprintf(stderr, "[companion] %s %s: %s\n", name(), describe(), why.c_str());
      if (_listener) _listener->onTransportDisconnected(why.c_str());
    }
  }

  // from JS (any thread)
  void jsRx(const uint8_t* p, size_t n) {
    std::lock_guard<std::mutex> lk(_mu);
    _rx.emplace_back(p, p + n);
  }
  void jsState(int up, const char* why) {
    std::lock_guard<std::mutex> lk(_mu);
    _pending_state = up;
    _why = why ? why : "";
    _state_changed = true;
  }

private:
  Kind _kind;
  std::string _arg, _desc;
  bool _up = false;
  StreamDeframer _deframer;
  std::mutex _mu;
  std::vector<std::vector<uint8_t>> _rx;
  bool _state_changed = false;
  int _pending_state = 0;
  std::string _why;
};

}  // namespace

extern "C" {
// JS allocates a buffer here, fills it, and hands it to _rx / _state, which free it.
EMSCRIPTEN_KEEPALIVE uint8_t* wada_companion_js_alloc(int n) { return (uint8_t*)malloc(n > 0 ? (size_t)n : 1); }
EMSCRIPTEN_KEEPALIVE void wada_companion_js_rx(uint8_t* p, int n) {
  if (s_active && p && n > 0) s_active->jsRx(p, (size_t)n);
  free(p);
}
// up = 1 connected, 0 closed; why = UTF-8 detail (may be null), freed here.
EMSCRIPTEN_KEEPALIVE void wada_companion_js_state(int up, char* why) {
  if (s_active) s_active->jsState(up, why);
  free(why);
}
}

CompanionTransport* wadaCreateBrowserTransport(const char* kind, const char* arg) {
  const std::string k = kind ? kind : "";
  if (k == "web-serial" || k == "serial") return new BrowserTransport(K_SERIAL, arg);
  if (k == "ble" || k == "bluetooth") return new BrowserTransport(K_BLE, arg);
  if (k == "ws" || k == "websocket") return new BrowserTransport(K_WS, arg);
  return nullptr;
}

#endif
