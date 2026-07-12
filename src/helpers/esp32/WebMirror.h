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
  size_t   ringBytes() const { return _cap; }   // 0 = ring alloc failed (feature can't stream)

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

  // ---- exit-remote request (browser -> device): the Exit button leaves remote mode.
  //      Board-agnostic way out for keyboard-less boards (V4/RAK). ----
  void requestExit() { _exit_req = true; }
  bool takeExit() { bool v = _exit_req; _exit_req = false; return v; }

  // ============ web mesh terminal (text CLI over WS; separate from the framebuffer) ============
  // Lightweight text path: browser sends a command line, the device runs runLocalCli() and
  // streams the CLI reply text back. No framebuffer, no reboot. Runtime toggle; the WS server
  // serves the terminal page + a /term socket while on.
  void     termBegin();                          // lazily alloc the reply ring (PSRAM); idempotent
  void     setTerminalOn(bool on);               // runtime enable (Remote app "Remote terminal")
  bool     terminalOn() const { return _term_on; }
  void     noteTermClients(int n) { _term_clients = n; }
  int      termClients() const { return _term_clients; }
  bool     termActive() const { return _term_on && _term_rep && _term_clients > 0; }

  // browser -> device: one command line. PRODUCER = WS server (net core), CONSUMER = UI/mesh loop.
  bool     pushTermCmd(const char* s, size_t len);
  size_t   popTermCmd(char* dst, size_t max);    // one command (0 if none)

  // device -> browser: CLI reply text stream. PRODUCER = mesh loop (CLI sink), CONSUMER = WS server.
  void     pushTermReply(const char* s);         // append text (drops if the ring is full)
  size_t   popTermReply(uint8_t* dst, size_t max);   // drain up to max bytes (0 if empty)

  // device -> browser: framed JSON data messages (contacts / threads / msgs / live push for the
  // Contacts + Chats tabs). PRODUCER = UI loop, CONSUMER = WS server. Length-prefixed, so each
  // pops as one complete message the browser can JSON.parse.
  bool     pushTermData(const char* json);       // one complete JSON message (dropped if it doesn't fit)
  size_t   popTermData(uint8_t* dst, size_t max);   // one complete message (0 if none)

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
  volatile bool _exit_req = false;        // browser asked to leave remote mode

  // ---- web terminal channels ----
  volatile bool _term_on = false;
  volatile int  _term_clients = 0;
  static const int TERM_CMD_CAP = 512;    // command ring: length-prefixed lines (small; commands are short)
  uint8_t  _term_cmd[TERM_CMD_CAP];
  volatile size_t _tc_head = 0, _tc_tail = 0;
  uint8_t* _term_rep = nullptr;           // reply ring: byte stream, PSRAM, lazily allocated
  size_t   _tr_cap = 0;
  volatile size_t _tr_head = 0, _tr_tail = 0;
  uint8_t* _term_data = nullptr;          // data ring: length-prefixed JSON messages, PSRAM
  size_t   _td_cap = 0;
  volatile size_t _td_head = 0, _td_tail = 0;
};

extern WebMirror g_web_mirror;
