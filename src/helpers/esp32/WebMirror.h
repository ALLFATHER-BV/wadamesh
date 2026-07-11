#pragma once
// ============================================================================
// Web UI mirror — bridge between the LVGL UI thread and the network loop thread.
//
// The device streams its live framebuffer (every LVGL flush) out to a phone
// browser over the existing WebSocket companion server, and the browser sends
// taps back. The browser renders the device's OWN UI verbatim, so any change to
// the firmware UI shows up on the web with zero web-side code.
//
// Two independent single-producer / single-consumer channels, so no locks:
//   * display : LVGL flush thread PRODUCES rects  -> network thread CONSUMES + sends
//   * input   : network thread PRODUCES pointers  -> LVGL indev CONSUMES
//
// Header is lvgl-free (stdint only) so WebSocketCompanionServer can include it
// without pulling in LVGL.
// ============================================================================
#include <stdint.h>
#include <stddef.h>

class WebMirror {
public:
  void begin(size_t ring_bytes = 0);   // lazily allocate the PSRAM display ring (0 = default size)
  void setEnabled(bool en);        // user opt-in (Settings > Wi-Fi toggle)
  bool enabled() const { return _enabled; }

  void setScreenSize(int w, int h) { _scr_w = (uint16_t)w; _scr_h = (uint16_t)h; }
  uint16_t screenW() const { return _scr_w; }
  uint16_t screenH() const { return _scr_h; }

  // True only when we should capture/stream: opted in, ring ready, >=1 client.
  // The LVGL flush hook checks this first, so the feature costs one bool load
  // when off.
  bool active() const { return _enabled && _ring && _clients > 0; }

  // ---- display: PRODUCER (LVGL flush thread) ----
  // Push one framebuffer rect as header+body. Returns false if dropped (ring
  // backed up); a drop arms a full-repaint so the mirror self-heals.
  bool pushFrame(const uint8_t* hdr, size_t hdr_len, const uint8_t* body, size_t body_len);

  // ---- display: CONSUMER (network thread) ----
  size_t popFrame(uint8_t* dst, size_t max_len);   // 0 if the ring is empty
  bool empty() const { return _head == _tail; }    // ring fully drained -> producer backpressure gate

  // Full-screen repaint handshake (new client / after a drop). The UI thread
  // consumes takeFullRepaint() and invalidates the screen.
  void requestFullRepaint() { _need_full = true; }
  bool takeFullRepaint() { if (!_need_full) return false; _need_full = false; return true; }

  // Mirror client count, refreshed by the WS server each loop.
  void noteClients(int n) { _clients = n; }
  int  clients() const { return _clients; }

  // ---- pointer: PRODUCER (network thread) / CONSUMER (LVGL indev) ----
  void pushPointer(int16_t x, int16_t y, uint8_t pressed);
  bool readPointer(int16_t* x, int16_t* y, bool* pressed) const;  // latest known state

  // ---- keyboard: PRODUCER (network thread) / CONSUMER (LVGL/UI thread) ----
  void pushKey(uint16_t cp);       // codepoint, or a control code (0x08 backspace, 0x0D enter)
  bool popKey(uint16_t* cp);       // false if the queue is empty

  // ---- text-field focus (device -> browser): show/hide the phone soft keyboard ----
  void setKbFocused(bool f) { if (f != _kb_focused) { _kb_focused = f; _kb_dirty = true; } }
  bool kbTake(bool* out) { if (!_kb_dirty) return false; _kb_dirty = false; if (out) *out = _kb_focused; return true; }

  // ---- remote-mode flag (device -> browser, carried in the size meta): the browser
  //      shows the Rotate button only in remote mode (VNC mirrors a fixed panel). ----
  void setRemote(bool r) { _remote = r; }
  bool remote() const { return _remote; }

  // ---- orientation request (browser -> device): the Rotate button asks for a new axis;
  //      the UI thread consumes it, saves the pref and reboots into that orientation. ----
  void requestOrient(uint8_t v) { _orient_req = v; }   // 1 = landscape, 2 = portrait, 0 = none
  uint8_t takeOrient() { uint8_t v = _orient_req; _orient_req = 0; return v; }

private:
  uint8_t* _ring = nullptr;
  size_t   _cap  = 0;
  volatile size_t _head = 0;       // producer writes, consumer reads
  volatile size_t _tail = 0;       // consumer writes, producer reads
  volatile bool   _enabled   = false;
  volatile bool   _need_full = false;
  volatile int    _clients   = 0;
  uint16_t _scr_w = 0, _scr_h = 0;
  volatile int16_t _px = 0, _py = 0;
  volatile uint8_t _ppressed = 0;
  volatile uint8_t _pknown   = 0;

  static const int KEY_RING = 32;   // typed keys awaiting the UI thread (SPSC)
  uint16_t _keys[KEY_RING];
  volatile uint8_t _khead = 0, _ktail = 0;
  volatile bool _kb_focused = false, _kb_dirty = false;   // editable-field focus signal
  volatile bool _remote = false;          // remote mode -> browser shows the Rotate button
  volatile uint8_t _orient_req = 0;       // browser-requested orientation (1=landscape, 2=portrait, 0=none)
};

extern WebMirror g_web_mirror;
