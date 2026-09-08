#pragma once

#include <stddef.h>
#include <stdint.h>
#include "WebFileTransferConfig.h"

#if WADA_WEB_FILE_TRANSFER
#include <freertos/FreeRTOS.h>

class WebFileTransfer {
public:
  static constexpr size_t MAX_INBOUND_BYTES = 2052;  // LE32 offset + a 2 KiB chunk
  static constexpr size_t MAX_REPLY_BYTES = 160;
  static constexpr size_t MAX_OUTBOUND_BYTES = 2053; // type + LE32 offset + 2 KiB

  bool begin();
  bool setEnabled(bool enabled, uint32_t code = 0);
  bool enabled() const { return _enabled; }
  uint32_t code() const { return _code; }
  bool matchesCode(const uint8_t* text, size_t len) const;
  bool authAllowed() const;
  void noteAuthFailure();
  void noteAuthSuccess();

  bool pushInbound(uint8_t opcode, const uint8_t* data, size_t len);
  size_t popInbound(uint8_t* opcode, uint8_t* dst, size_t max_len);

  bool pushReply(const char* text);
  size_t popReply(uint8_t* dst, size_t max_len);
  bool pushData(const uint8_t* data, size_t len);
  size_t popData(uint8_t* dst, size_t max_len);
  void clearData();
  void discardTraffic();

  void noteClients(int count) { _clients = count; }
  int clients() const { return _clients; }
  void touch();
  uint32_t lastActivityMs() const { return _last_activity_ms; }

private:
  static constexpr uint8_t INBOUND_SLOTS = 3;
  static constexpr uint8_t REPLY_SLOTS = 8;

  uint8_t* _inbound = nullptr;
  uint16_t _inbound_len[INBOUND_SLOTS] = {0};
  uint8_t _inbound_opcode[INBOUND_SLOTS] = {0};
  volatile uint8_t _inbound_head = 0;
  volatile uint8_t _inbound_tail = 0;
  portMUX_TYPE _inbound_mux = portMUX_INITIALIZER_UNLOCKED;

  char _reply[REPLY_SLOTS][MAX_REPLY_BYTES] = {{0}};
  uint8_t _reply_len[REPLY_SLOTS] = {0};
  volatile uint8_t _reply_head = 0;
  volatile uint8_t _reply_tail = 0;
  portMUX_TYPE _reply_mux = portMUX_INITIALIZER_UNLOCKED;

  uint8_t* _outbound = nullptr;
  volatile uint16_t _outbound_len = 0;
  portMUX_TYPE _outbound_mux = portMUX_INITIALIZER_UNLOCKED;

  volatile bool _enabled = false;
  volatile uint32_t _code = 0;
  volatile int _clients = 0;
  volatile uint32_t _last_activity_ms = 0;
  volatile uint32_t _auth_blocked_until_ms = 0;
  volatile uint8_t _auth_failures = 0;
};

extern WebFileTransfer g_web_file_transfer;

#endif  // WADA_WEB_FILE_TRANSFER