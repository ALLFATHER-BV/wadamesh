#include "WebMirror.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

WebMirror g_web_mirror;

#ifndef WEB_MIRROR_RING_BYTES
#define WEB_MIRROR_RING_BYTES  (384 * 1024)   // PSRAM; holds several banded frames
#endif

void WebMirror::begin(size_t ring_bytes) {
  if (_ring) return;
  if (ring_bytes == 0) ring_bytes = WEB_MIRROR_RING_BYTES;
  _ring = (uint8_t*)heap_caps_malloc(ring_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!_ring) _ring = (uint8_t*)malloc(ring_bytes);   // last-ditch internal DRAM
  _cap  = _ring ? ring_bytes : 0;
  _head = _tail = 0;
}

void WebMirror::setEnabled(bool en) {
  _enabled = en;
  if (en) {
    _need_full = true;         // next connect wants a full frame
  } else {
    _tail = _head;             // drain the ring so nothing stale lingers
  }
}

static inline size_t ringUsed(size_t head, size_t tail, size_t cap) {
  return (head + cap - tail) % cap;
}

bool WebMirror::pushFrame(const uint8_t* hdr, size_t hdr_len, const uint8_t* body, size_t body_len) {
  if (!_ring || _cap == 0) return false;
  const size_t payload = hdr_len + body_len;
  if (payload == 0 || payload > 0xFFFF) { _need_full = true; return false; }
  const size_t total = 2 + payload;                 // 2-byte length prefix + payload

  size_t head = _head;
  const size_t tail = _tail;                         // written by the consumer; a stale read is conservative
  const size_t freeb = _cap - ringUsed(head, tail, _cap) - 1;
  if (freeb < total) { _need_full = true; return false; }  // backed up -> drop, heal via full repaint

  _ring[head] = (uint8_t)(payload & 0xFF); head = (head + 1) % _cap;
  _ring[head] = (uint8_t)(payload >> 8);   head = (head + 1) % _cap;
  for (size_t i = 0; i < hdr_len; i++) { _ring[head] = hdr[i]; head = (head + 1) % _cap; }
  {
    size_t first = _cap - head;
    if (first > body_len) first = body_len;
    memcpy(_ring + head, body, first);
    if (first < body_len) memcpy(_ring, body + first, body_len - first);
    head = (head + body_len) % _cap;
  }
  __sync_synchronize();          // publish the payload before advancing head
  _head = head;
  return true;
}

size_t WebMirror::popFrame(uint8_t* dst, size_t max_len) {
  if (!_ring || _cap == 0) return 0;
  const size_t head = _head;
  __sync_synchronize();          // read head before the payload it guards
  size_t tail = _tail;
  const size_t used = ringUsed(head, tail, _cap);
  if (used < 2) return 0;

  size_t l0 = _ring[tail];             tail = (tail + 1) % _cap;
  size_t l1 = _ring[tail];             tail = (tail + 1) % _cap;
  const size_t payload = l0 | (l1 << 8);
  if (payload == 0 || payload + 2 > used || payload > max_len) {
    _tail = head;                      // corrupt/oversized -> resync by dropping the ring
    return 0;
  }
  size_t first = _cap - tail;
  if (first > payload) first = payload;
  memcpy(dst, _ring + tail, first);
  if (first < payload) memcpy(dst + first, _ring, payload - first);
  _tail = (tail + payload) % _cap;
  return payload;
}

void WebMirror::pushPointer(int16_t x, int16_t y, uint8_t pressed) {
  // Producer = network/stream core, consumer = UI core: publish the coords before the
  // _pknown flag so the reader never sees a half-updated point.
  _px = x; _py = y; _ppressed = pressed ? 1 : 0;
  __sync_synchronize();
  _pknown = 1;
}

bool WebMirror::readPointer(int16_t* x, int16_t* y, bool* pressed) const {
  if (!_pknown) return false;
  __sync_synchronize();
  if (x) *x = _px;
  if (y) *y = _py;
  if (pressed) *pressed = (_ppressed != 0);
  return true;
}

void WebMirror::pushKey(uint16_t cp) {
  uint8_t n = (uint8_t)((_khead + 1) % KEY_RING);
  if (n != _ktail) { _keys[_khead] = cp; __sync_synchronize(); _khead = n; }   // publish key before advancing head; drop if full
}

bool WebMirror::popKey(uint16_t* cp) {
  if (_khead == _ktail) return false;
  __sync_synchronize();                 // read head before the key slot it guards
  if (cp) *cp = _keys[_ktail];
  _ktail = (uint8_t)((_ktail + 1) % KEY_RING);
  return true;
}

// ============================ web mesh terminal ============================
#ifndef WEB_TERM_REP_BYTES
#define WEB_TERM_REP_BYTES  (16 * 1024)   // PSRAM reply-text ring
#endif
#ifndef WEB_TERM_DATA_BYTES
#define WEB_TERM_DATA_BYTES (24 * 1024)   // PSRAM framed-JSON ring (Contacts/Chats data)
#endif

void WebMirror::termBegin() {
  if (_term_rep && _term_data) return;
  if (!_term_rep) {
    _term_rep = (uint8_t*)heap_caps_malloc(WEB_TERM_REP_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_term_rep) _term_rep = (uint8_t*)malloc(WEB_TERM_REP_BYTES);
    _tr_cap  = _term_rep ? WEB_TERM_REP_BYTES : 0;
    _tr_head = _tr_tail = 0;
  }
  if (!_term_data) {
    _term_data = (uint8_t*)heap_caps_malloc(WEB_TERM_DATA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!_term_data) _term_data = (uint8_t*)malloc(WEB_TERM_DATA_BYTES);
    _td_cap  = _term_data ? WEB_TERM_DATA_BYTES : 0;
    _td_head = _td_tail = 0;
  }
}

void WebMirror::setTerminalOn(bool on) {
  if (on) termBegin();
  _term_on = on;
  if (!on) { _tr_tail = _tr_head; _tc_tail = _tc_head; _td_tail = _td_head; }   // drain rings on disable
}

// command ring (browser -> device): [len_lo, len_hi, bytes...]
bool WebMirror::pushTermCmd(const char* s, size_t len) {
  if (!s || len == 0 || len > 250) return false;
  size_t head = _tc_head;
  const size_t tail = _tc_tail;
  const size_t used  = (head + TERM_CMD_CAP - tail) % TERM_CMD_CAP;
  const size_t freeb = TERM_CMD_CAP - used - 1;
  if (freeb < len + 2) return false;
  _term_cmd[head] = (uint8_t)(len & 0xFF); head = (head + 1) % TERM_CMD_CAP;
  _term_cmd[head] = (uint8_t)(len >> 8);   head = (head + 1) % TERM_CMD_CAP;
  for (size_t i = 0; i < len; i++) { _term_cmd[head] = (uint8_t)s[i]; head = (head + 1) % TERM_CMD_CAP; }
  __sync_synchronize();
  _tc_head = head;
  return true;
}

size_t WebMirror::popTermCmd(char* dst, size_t max) {
  const size_t head = _tc_head;
  __sync_synchronize();
  size_t tail = _tc_tail;
  const size_t used = (head + TERM_CMD_CAP - tail) % TERM_CMD_CAP;
  if (used < 2) return 0;
  size_t l0 = _term_cmd[tail]; tail = (tail + 1) % TERM_CMD_CAP;
  size_t l1 = _term_cmd[tail]; tail = (tail + 1) % TERM_CMD_CAP;
  const size_t len = l0 | (l1 << 8);
  if (len == 0 || len + 2 > used || len + 1 > max) { _tc_tail = head; return 0; }   // corrupt/too-big -> resync
  for (size_t i = 0; i < len; i++) { dst[i] = (char)_term_cmd[tail]; tail = (tail + 1) % TERM_CMD_CAP; }
  dst[len] = '\0';
  _tc_tail = tail;
  return len;
}

// reply ring (device -> browser): raw byte stream (browser just appends text)
void WebMirror::pushTermReply(const char* s) {
  if (!s || !_term_rep || _tr_cap == 0) return;
  size_t head = _tr_head;
  const size_t tail = _tr_tail;
  size_t used  = (head + _tr_cap - tail) % _tr_cap;
  size_t freeb = _tr_cap - used - 1;
  for (const char* p = s; *p && freeb > 0; ++p) {
    _term_rep[head] = (uint8_t)*p;
    head = (head + 1) % _tr_cap;
    freeb--;
  }
  __sync_synchronize();
  _tr_head = head;
}

size_t WebMirror::popTermReply(uint8_t* dst, size_t max) {
  if (!_term_rep || _tr_cap == 0) return 0;
  const size_t head = _tr_head;
  __sync_synchronize();
  size_t tail = _tr_tail;
  size_t n = 0;
  while (tail != head && n < max) {
    dst[n++] = _term_rep[tail];
    tail = (tail + 1) % _tr_cap;
  }
  _tr_tail = tail;
  return n;
}

// data ring (device -> browser): [len_lo, len_hi, json bytes...] framed messages
bool WebMirror::pushTermData(const char* json) {
  if (!json || !_term_data || _td_cap == 0) return false;
  size_t len = strlen(json);
  if (len == 0 || len > 0xFFFF) return false;
  size_t head = _td_head;
  const size_t tail = _td_tail;
  const size_t used  = (head + _td_cap - tail) % _td_cap;
  const size_t freeb = _td_cap - used - 1;
  if (freeb < len + 2) return false;   // ring backed up -> drop this message (browser can re-request)
  _term_data[head] = (uint8_t)(len & 0xFF); head = (head + 1) % _td_cap;
  _term_data[head] = (uint8_t)(len >> 8);   head = (head + 1) % _td_cap;
  for (size_t i = 0; i < len; i++) { _term_data[head] = (uint8_t)json[i]; head = (head + 1) % _td_cap; }
  __sync_synchronize();
  _td_head = head;
  return true;
}

size_t WebMirror::popTermData(uint8_t* dst, size_t max) {
  if (!_term_data || _td_cap == 0) return 0;
  const size_t head = _td_head;
  __sync_synchronize();
  size_t tail = _td_tail;
  const size_t used = (head + _td_cap - tail) % _td_cap;
  if (used < 2) return 0;
  size_t l0 = _term_data[tail]; tail = (tail + 1) % _td_cap;
  size_t l1 = _term_data[tail]; tail = (tail + 1) % _td_cap;
  const size_t len = l0 | (l1 << 8);
  if (len == 0 || len + 2 > used || len > max) { _td_tail = head; return 0; }   // corrupt/too-big -> resync
  for (size_t i = 0; i < len; i++) { dst[i] = _term_data[tail]; tail = (tail + 1) % _td_cap; }
  _td_tail = tail;
  return len;
}
