#include "WebFileTransfer.h"

#if WADA_WEB_FILE_TRANSFER

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>

WebFileTransfer g_web_file_transfer;

bool WebFileTransfer::begin() {
  if (!_inbound) {
    const size_t bytes = INBOUND_SLOTS * MAX_INBOUND_BYTES;
    _inbound = static_cast<uint8_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_inbound) _inbound = static_cast<uint8_t*>(malloc(bytes));
  }
  if (!_outbound) {
    _outbound = static_cast<uint8_t*>(
        heap_caps_malloc(MAX_OUTBOUND_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_outbound) _outbound = static_cast<uint8_t*>(malloc(MAX_OUTBOUND_BYTES));
  }
  return _inbound != nullptr && _outbound != nullptr;
}

bool WebFileTransfer::setEnabled(bool enabled, uint32_t code) {
  if (!enabled) {
    _enabled = false;
    __sync_synchronize();
    _code = 0;
    _clients = 0;
    _auth_failures = 0;
    _auth_blocked_until_ms = 0;
    discardTraffic();
    return true;
  }
  if (!begin() || code < 100000 || code > 999999) return false;
  discardTraffic();
  _code = code;
  _clients = 0;
  _auth_failures = 0;
  _auth_blocked_until_ms = 0;
  _last_activity_ms = millis();
  __sync_synchronize();
  _enabled = true;
  return true;
}

bool WebFileTransfer::matchesCode(const uint8_t* text, size_t len) const {
  if (!_enabled || !text || len != 6) return false;
  uint32_t value = 0;
  for (size_t i = 0; i < len; ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    value = value * 10u + static_cast<uint32_t>(text[i] - '0');
  }
  return value == _code;
}

bool WebFileTransfer::authAllowed() const {
  return _auth_blocked_until_ms == 0 ||
         static_cast<int32_t>(millis() - _auth_blocked_until_ms) >= 0;
}

void WebFileTransfer::noteAuthFailure() {
  if (++_auth_failures >= 3) {
    _auth_failures = 0;
    _auth_blocked_until_ms = millis() + 30000u;
  }
}

void WebFileTransfer::noteAuthSuccess() {
  _auth_failures = 0;
  _auth_blocked_until_ms = 0;
  touch();
}

void WebFileTransfer::touch() {
  _last_activity_ms = millis();
}

bool WebFileTransfer::pushInbound(uint8_t opcode, const uint8_t* data, size_t len) {
  if (!_enabled || !_inbound || !data || len == 0 || len > MAX_INBOUND_BYTES) return false;
  portENTER_CRITICAL(&_inbound_mux);
  const uint8_t next = static_cast<uint8_t>((_inbound_head + 1) % INBOUND_SLOTS);
  if (next == _inbound_tail) {
    portEXIT_CRITICAL(&_inbound_mux);
    return false;
  }
  const uint8_t slot = _inbound_head;
  memcpy(_inbound + static_cast<size_t>(slot) * MAX_INBOUND_BYTES, data, len);
  _inbound_len[slot] = static_cast<uint16_t>(len);
  _inbound_opcode[slot] = opcode;
  _inbound_head = next;
  portEXIT_CRITICAL(&_inbound_mux);
  touch();
  return true;
}

size_t WebFileTransfer::popInbound(uint8_t* opcode, uint8_t* dst, size_t max_len) {
  portENTER_CRITICAL(&_inbound_mux);
  const uint8_t head = _inbound_head;
  if (_inbound_tail == head) {
    portEXIT_CRITICAL(&_inbound_mux);
    return 0;
  }
  const uint8_t slot = _inbound_tail;
  const size_t len = _inbound_len[slot];
  if (!dst || len == 0 || len > max_len) {
    _inbound_tail = static_cast<uint8_t>((slot + 1) % INBOUND_SLOTS);
    portEXIT_CRITICAL(&_inbound_mux);
    return 0;
  }
  memcpy(dst, _inbound + static_cast<size_t>(slot) * MAX_INBOUND_BYTES, len);
  if (opcode) *opcode = _inbound_opcode[slot];
  _inbound_tail = static_cast<uint8_t>((slot + 1) % INBOUND_SLOTS);
  portEXIT_CRITICAL(&_inbound_mux);
  return len;
}

bool WebFileTransfer::pushReply(const char* text) {
  if (!_enabled || !text) return false;
  const size_t len = strlen(text);
  if (len == 0 || len >= MAX_REPLY_BYTES) return false;
  portENTER_CRITICAL(&_reply_mux);
  const uint8_t next = static_cast<uint8_t>((_reply_head + 1) % REPLY_SLOTS);
  if (next == _reply_tail) {
    portEXIT_CRITICAL(&_reply_mux);
    return false;
  }
  const uint8_t slot = _reply_head;
  memcpy(_reply[slot], text, len + 1);
  _reply_len[slot] = static_cast<uint8_t>(len);
  _reply_head = next;
  portEXIT_CRITICAL(&_reply_mux);
  touch();
  return true;
}

size_t WebFileTransfer::popReply(uint8_t* dst, size_t max_len) {
  portENTER_CRITICAL(&_reply_mux);
  const uint8_t head = _reply_head;
  if (_reply_tail == head) {
    portEXIT_CRITICAL(&_reply_mux);
    return 0;
  }
  const uint8_t slot = _reply_tail;
  const size_t len = _reply_len[slot];
  if (!dst || len == 0 || len > max_len) {
    portEXIT_CRITICAL(&_reply_mux);
    return 0;
  }
  memcpy(dst, _reply[slot], len);
  _reply_tail = static_cast<uint8_t>((slot + 1) % REPLY_SLOTS);
  portEXIT_CRITICAL(&_reply_mux);
  return len;
}

bool WebFileTransfer::pushData(const uint8_t* data, size_t len) {
  if (!_enabled || !_outbound || !data || len == 0 || len > MAX_OUTBOUND_BYTES) return false;
  portENTER_CRITICAL(&_outbound_mux);
  if (_outbound_len != 0) {
    portEXIT_CRITICAL(&_outbound_mux);
    return false;
  }
  memcpy(_outbound, data, len);
  _outbound_len = static_cast<uint16_t>(len);
  portEXIT_CRITICAL(&_outbound_mux);
  touch();
  return true;
}

size_t WebFileTransfer::popData(uint8_t* dst, size_t max_len) {
  if (!dst) return 0;
  portENTER_CRITICAL(&_outbound_mux);
  const size_t len = _outbound_len;
  if (len == 0 || len > max_len) {
    portEXIT_CRITICAL(&_outbound_mux);
    return 0;
  }
  memcpy(dst, _outbound, len);
  _outbound_len = 0;
  portEXIT_CRITICAL(&_outbound_mux);
  return len;
}

void WebFileTransfer::clearData() {
  portENTER_CRITICAL(&_outbound_mux);
  _outbound_len = 0;
  portEXIT_CRITICAL(&_outbound_mux);
}

void WebFileTransfer::discardTraffic() {
  portENTER_CRITICAL(&_inbound_mux);
  _inbound_tail = _inbound_head;
  portEXIT_CRITICAL(&_inbound_mux);
  portENTER_CRITICAL(&_reply_mux);
  _reply_tail = _reply_head;
  portEXIT_CRITICAL(&_reply_mux);
  clearData();
}

#endif  // WADA_WEB_FILE_TRANSFER