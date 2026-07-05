#include "MultiTransportCompanionInterface.h"
#include <helpers/RepeaterTcpOtaEmit.h>
#include "WifiRuntimeStore.h"   // persist BLE on/off (ble_en) across reboots
#include <string.h>

// Companion push code for the per-packet RX log (matches MyMesh.cpp). It is kept OFF
// the BLE transport in writeFrameToAll — see the note there (issues #46, #54) — EXCEPT
// for one-shot passes armed via bleAllowNextRxLog() (echoes of our own sends, #94).
#define PUSH_CODE_LOG_RX_DATA   0x88

bool MultiTransportCompanionInterface::s_ble_rxlog_once = false;

MultiTransportCompanionInterface::MultiTransportCompanionInterface()
  : _tcp_port(0), _ws_port(0), _tcp_started(false), _ws_started(false), _tcp_enabled(true), _isEnabled(false), _broadcast(false), _last_reply_target(REPLY_TARGET_USB), _ota_tcp_suspended(false), _ota_ws_suspended(false), _ota_ws_listen_paused(false)
#ifdef BLE_PIN_CODE
  , _ble_begun(false), _ble_enabled(false), _ota_ble_released(false), _ble_pin_code(0)
#endif
{
  for (size_t i = 0; i < sizeof(_client_ids) / sizeof(_client_ids[0]); i++)
    _client_ids[i][0] = '\0';
#ifdef BLE_PIN_CODE
  _ble_prefix[0] = '\0';
  _ble_name[0] = '\0';
#endif
}

void MultiTransportCompanionInterface::begin(Stream& usb_serial, uint16_t tcp_port, uint16_t ws_port) {
  _usb.begin(usb_serial);
  _tcp_port = tcp_port;
  _ws_port = ws_port;
  _last_reply_target = REPLY_TARGET_USB;
}

void MultiTransportCompanionInterface::startTcpServer(bool wifi_connected) {
  if (_tcp_enabled && !_tcp_started && _tcp_port != 0) {
    _tcp.begin(_tcp_port);
    _tcp_started = true;
  }
  // Plain WebSocket: start only when Wi-Fi has an address (caller defers TCP/WS start after splash).
  if (_tcp_enabled && !_ws_started && _ws_port != 0 && wifi_connected) {
    _ws.begin(_ws_port);
    _ws_started = true;
  }
}

void MultiTransportCompanionInterface::tickWebSocketHandshake() {
  if (_ws_started) _ws.tickHandshake();
}

void MultiTransportCompanionInterface::stopTcpServer() {
  if (_ws_started) {
    _ws.stop();
    _ws_started = false;
  }
  if (_tcp_started) {
    _tcp.stop();
    _tcp_started = false;
  }
  _tcp_enabled = false;
}

void MultiTransportCompanionInterface::enableTcp() {
  _tcp_enabled = true;
  // Restart immediately: don't wait for the next main-loop tick, and don't
  // require wifi_started to be true (TCP itself doesn't need an IP address).
  startTcpServer(false);
}

void MultiTransportCompanionInterface::disableTcp() {
  stopTcpServer();
}

#ifdef BLE_PIN_CODE
void MultiTransportCompanionInterface::prepareBle(const char* prefix, char* name, uint32_t pin_code) {
  if (prefix) {
    strncpy(_ble_prefix, prefix, sizeof(_ble_prefix) - 1);
    _ble_prefix[sizeof(_ble_prefix) - 1] = '\0';
  } else {
    _ble_prefix[0] = '\0';
  }
  if (name) {
    strncpy(_ble_name, name, sizeof(_ble_name) - 1);
    _ble_name[sizeof(_ble_name) - 1] = '\0';
  } else {
    _ble_name[0] = '\0';
  }
  _ble_pin_code = pin_code;
}

void MultiTransportCompanionInterface::beginBle(const char* prefix, char* name, uint32_t pin_code) {
  prepareBle(prefix, name, pin_code);
  _ble.begin(prefix, name, pin_code);
  _ble_begun = true;
  _ble_enabled = true;
  _ota_ble_released = false;
  _ble.enable();
}

void MultiTransportCompanionInterface::enableBle() {
  if (!_ble_begun) {
    // Deferred at boot (heap guard) or toggled on from off: bring the stack up
    // now, live, from the params stashed by prepareBle()/beginBle().
    if (_ble_prefix[0] == '\0' && _ble_name[0] == '\0') return;   // no params known
    char name[sizeof(_ble_name)];
    strncpy(name, _ble_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    _ble.begin(_ble_prefix, name, _ble_pin_code);
    _ble_begun = true;
  }
  _ble_enabled = true;
  wifiConfigSetBleEnabled(true);    // persist so it survives reboot
  _ble.enable();
}

void MultiTransportCompanionInterface::disableBle() {
  _ble_enabled = false;
  wifiConfigSetBleEnabled(false);   // persist so BT stays off across reboot
  _ble.disable();                    // stop advertising + drop any connection
  // NOTE: we deliberately do NOT NimBLEDevice::deinit() here. Tearing the BT
  // controller down while Wi-Fi+BLE coexistence is active crashes — the esp_coex
  // layer still holds a reference to the controller — so "off" stops advertising
  // but keeps the NimBLE host resident. Its RAM is only fully reclaimed on reboot.
}

bool MultiTransportCompanionInterface::getBlePeerAddress(char* buf, size_t len) const {
  if (!_ble_begun || !_ble_enabled) {
    if (buf && len > 0) buf[0] = '\0';
    return false;
  }
  return _ble.getConnectedPeerAddress(buf, len);
}
#endif

void MultiTransportCompanionInterface::enable() {
  _isEnabled = true;
  _usb.enable();
  _last_reply_target = REPLY_TARGET_USB;
#ifdef BLE_PIN_CODE
  if (_ble_begun && _ble_enabled)
    _ble.enable();
#endif
}

void MultiTransportCompanionInterface::disable() {
  _isEnabled = false;
  _usb.disable();
#ifdef BLE_PIN_CODE
  _ble.disable();
#endif
}

void MultiTransportCompanionInterface::prepareForHttpOta() {
  _ota_tcp_suspended = false;
  _ota_ws_suspended = false;
  _ota_ws_listen_paused = false;

  // Keep the Wi-Fi control path that issued `ota url` (TCP or WebSocket) alive so the meshcomod
  // client stays connected for progress and the final OK/reboot message. Suspend the other
  // transport plus BLE to free RAM for HTTPS/TLS.
  const bool preserve_tcp = (_last_reply_target >= 0 && _last_reply_target < REPLY_TARGET_WS_0);
  const bool preserve_ws = (_last_reply_target >= REPLY_TARGET_WS_0);

  char line[168];
  if (preserve_tcp) {
    snprintf(line, sizeof(line), "OTA: minimal companion preserve=tcp suspend=ws,ble heap=%u",
             (unsigned)ESP.getFreeHeap());
  } else if (preserve_ws) {
    snprintf(line, sizeof(line), "OTA: minimal companion preserve=ws suspend=tcp,ble heap=%u",
             (unsigned)ESP.getFreeHeap());
  } else {
    snprintf(line, sizeof(line), "OTA: minimal companion unexpected reply_target=%d heap=%u", _last_reply_target,
             (unsigned)ESP.getFreeHeap());
  }
  meshcoreRepeaterTcpOtaEmitLine(line);

  if (preserve_ws && _tcp_started) {
    _tcp.stop();
    _tcp_started = false;
    _ota_tcp_suspended = true;
    meshcoreRepeaterTcpOtaEmitLine("OTA: suspended companion TCP server");
  }
  if (preserve_tcp && _ws_started) {
    _ws.stop();
    _ws_started = false;
    _ota_ws_suspended = true;
    meshcoreRepeaterTcpOtaEmitLine("OTA: suspended companion WebSocket server");
  }
  if (preserve_ws && _ws_started) {
    _ws.pauseListen();
    _ota_ws_listen_paused = true;
    meshcoreRepeaterTcpOtaEmitLine("OTA: paused WS listen (client kept, no new connections)");
  }

#ifdef BLE_PIN_CODE
  if (_ble_begun && _ble_enabled) {
    _ble.disable();
    NimBLEDevice::deinit(true);
    _ble_begun = false;
    _ble_enabled = false;
    _ota_ble_released = true;
    meshcoreRepeaterTcpOtaEmitLine("OTA: released BLE stack");
  }
#endif

  snprintf(line, sizeof(line), "OTA: minimal companion after heap=%u max=%u", (unsigned)ESP.getFreeHeap(),
           (unsigned)ESP.getMaxAllocHeap());
  meshcoreRepeaterTcpOtaEmitLine(line);
}

bool MultiTransportCompanionInterface::isHttpOtaWifiControlSession() const {
  return _last_reply_target != REPLY_TARGET_USB && _last_reply_target != REPLY_TARGET_BLE;
}

void MultiTransportCompanionInterface::restoreAfterHttpOta() {
  if (_ota_ws_listen_paused) {
    _ws.resumeListen();
    _ota_ws_listen_paused = false;
    meshcoreRepeaterTcpOtaEmitLine("OTA: resumed WebSocket listen");
  }
#ifdef BLE_PIN_CODE
  if (_ota_ble_released && _ble_prefix[0] && _ble_name[0]) {
    char ble_name[sizeof(_ble_name)];
    strncpy(ble_name, _ble_name, sizeof(ble_name) - 1);
    ble_name[sizeof(ble_name) - 1] = '\0';
    _ble.begin(_ble_prefix, ble_name, _ble_pin_code);
    _ble_begun = true;
    _ble_enabled = true;
    _ble.enable();
    _ota_ble_released = false;
    meshcoreRepeaterTcpOtaEmitLine("OTA: restored BLE stack");
  }
#endif
  if (_ota_tcp_suspended) {
    _tcp.begin(_tcp_port);
    _tcp_started = true;
    _ota_tcp_suspended = false;
    meshcoreRepeaterTcpOtaEmitLine("OTA: restored companion TCP server");
  }
  if (_ota_ws_suspended) {
    _ws.begin(_ws_port);
    _ws_started = true;
    _ota_ws_suspended = false;
    meshcoreRepeaterTcpOtaEmitLine("OTA: restored companion WebSocket server");
  }
}

bool MultiTransportCompanionInterface::isConnected() const {
  if (_usb.isConnected()) return true;
#ifdef BLE_PIN_CODE
  if (_ble_begun && _ble_enabled && _ble.isConnected()) return true;
#endif
  if (_tcp_started && _tcp.connectedCount() > 0) return true;
  if (_ws_started && _ws.connectedCount() > 0) return true;
  return false;
}

bool MultiTransportCompanionInterface::isWriteBusy() const {
  if (_usb.isWriteBusy()) return true;
#ifdef BLE_PIN_CODE
  if (_ble_begun && _ble_enabled && _ble.isWriteBusy()) return true;
#endif
  return false;
}

size_t MultiTransportCompanionInterface::checkRecvFrame(uint8_t dest[]) {
  if (!_isEnabled) return 0;

#ifdef BLE_PIN_CODE
  // Drain BLE send queue every loop so PC (or any BLE client) gets pushes even when USB/TCP are polled first.
  if (_ble_begun && _ble_enabled)
    _ble.drainSendQueue();
#endif

  // Poll USB first (preserve Home Assistant / USB priority). Do not overwrite _last_reply_target
  // when caller is in the middle of a contact-list stream (handled by caller saving/restoring target).
  size_t len = _usb.checkRecvFrame(dest);
  if (len > 0) {
    _last_reply_target = REPLY_TARGET_USB;
    return len;
  }

  // Then poll TCP clients (only after TCP server was started)
  if (_tcp_started) {
    int tcp_client = -1;
    len = _tcp.pollRecvFrame(dest, &tcp_client);
    if (len > 0) {
      _last_reply_target = tcp_client;
      return len;
    }
  }

  // Then poll WebSocket clients
  if (_ws_started) {
    int ws_client = -1;
    len = _ws.pollRecvFrame(dest, &ws_client);
    if (len > 0) {
      _last_reply_target = REPLY_TARGET_WS_0 + ws_client;
      return len;
    }
  }

#ifdef BLE_PIN_CODE
  if (_ble_begun && _ble_enabled) {
    len = _ble.checkRecvFrame(dest);
    if (len > 0) {
      _last_reply_target = REPLY_TARGET_BLE;
      return len;
    }
  }
#endif

  return 0;
}

size_t MultiTransportCompanionInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) return 0;
  // Single-target only (command responses, sync history). Never broadcast.
  if (_last_reply_target == REPLY_TARGET_USB)
    return _usb.writeFrame(src, len);
#ifdef BLE_PIN_CODE
  if (_last_reply_target == REPLY_TARGET_BLE && _ble_begun && _ble_enabled)
    return _ble.writeFrame(src, len);
#endif
  if (_last_reply_target >= REPLY_TARGET_WS_0 && _last_reply_target < REPLY_TARGET_WS_0 + WS_COMPANION_MAX_CLIENTS && _ws_started)
    return _ws.writeToClient(_last_reply_target - REPLY_TARGET_WS_0, src, len);
  if (_tcp_started)
    return _tcp.writeToClient(_last_reply_target, src, len);
  return 0;
}

size_t MultiTransportCompanionInterface::writeFrameToAll(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) return 0;
  if (!_broadcast)
    return writeFrame(src, len);
  bool all_ok = true;
  if (_usb.isConnected() && _usb.writeFrame(src, len) != len)
    all_ok = false;
#ifdef BLE_PIN_CODE
  // The per-packet RX log floods BLE's ~16 frames/sec budget on a busy mesh, starving
  // the frames that matter — chat messages + admin responses (issues #46, #54). So it
  // is kept OFF BLE (USB/TCP/WS have the bandwidth) — EXCEPT when MyMesh::logRxRaw
  // armed a one-shot pass because this frame is an echo of OUR OWN flood send: the
  // app's "Repeats heard" is computed exactly from those (it broke on BLE when the
  // blanket skip landed in beta_23 — issue #94), and a few echoes per send are
  // nowhere near the flood that caused #46/#54.
  const bool rxlog    = (len > 0 && src[0] == PUSH_CODE_LOG_RX_DATA);
  const bool ble_pass = rxlog && s_ble_rxlog_once;
  if (rxlog) s_ble_rxlog_once = false;                // consume the one-shot either way
  const bool skip_ble = rxlog && !ble_pass;
  if (!skip_ble && _ble_begun && _ble_enabled && _ble.isConnected() && _ble.writeFrame(src, len) != len)
    all_ok = false;
#endif
  if (_tcp_started && _tcp.connectedCount() > 0 && _tcp.writeToAllClients(src, len) != len)
    all_ok = false;
  if (_ws_started && _ws.connectedCount() > 0 && _ws.writeToAllClients(src, len) != len)
    all_ok = false;
  return all_ok ? len : 0;
}

int MultiTransportCompanionInterface::_clientIdSlot() const {
#ifdef BLE_PIN_CODE
  if (_last_reply_target == REPLY_TARGET_USB) return 0;
  if (_last_reply_target == REPLY_TARGET_BLE) return 1;
  if (_last_reply_target >= REPLY_TARGET_WS_0 && _last_reply_target < REPLY_TARGET_WS_0 + WS_COMPANION_MAX_CLIENTS)
    return 2 + TCP_COMPANION_MAX_CLIENTS + (_last_reply_target - REPLY_TARGET_WS_0);
  return _last_reply_target + 2;  // TCP 0..N -> slots 2..
#else
  if (_last_reply_target >= REPLY_TARGET_WS_0 && _last_reply_target < REPLY_TARGET_WS_0 + WS_COMPANION_MAX_CLIENTS)
    return 1 + TCP_COMPANION_MAX_CLIENTS + (_last_reply_target - REPLY_TARGET_WS_0);
  return _last_reply_target + 1;
#endif
}

void MultiTransportCompanionInterface::setCurrentClientId(const char* id) {
  int slot = _clientIdSlot();
  if (slot >= 0 && slot < (int)(sizeof(_client_ids) / sizeof(_client_ids[0]))) {
    if (id) {
      strncpy(_client_ids[slot], id, _max_client_id_len - 1);
      _client_ids[slot][_max_client_id_len - 1] = '\0';
    } else {
      _client_ids[slot][0] = '\0';
    }
  }
}

void MultiTransportCompanionInterface::getCurrentClientId(char* dest, size_t max_len) const {
  if (!dest || max_len == 0) return;
  dest[0] = '\0';
  int slot = _clientIdSlot();
  if (slot < 0 || slot >= (int)(sizeof(_client_ids) / sizeof(_client_ids[0]))) return;
  // If app sent client_id in CMD_APP_START, use it; otherwise use connection-based id
  // so non-custom clients (HA, MeshCore app) still get per-connection history without sending anything.
  if (_client_ids[slot][0] != '\0') {
    strncpy(dest, _client_ids[slot], max_len - 1);
    dest[max_len - 1] = '\0';
    return;
  }
#ifdef BLE_PIN_CODE
  static const char* const default_ids[] = { "usb", "ble", "tcp0", "tcp1", "tcp2", "ws0", "ws1" };
#else
  static const char* const default_ids[] = { "usb", "tcp0", "tcp1", "tcp2", "ws0", "ws1" };
#endif
  size_t n = sizeof(default_ids) / sizeof(default_ids[0]);
  if ((size_t)slot < n) {
    strncpy(dest, default_ids[slot], max_len - 1);
    dest[max_len - 1] = '\0';
  }
}
