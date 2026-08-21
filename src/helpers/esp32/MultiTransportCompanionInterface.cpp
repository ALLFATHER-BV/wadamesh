#include "MultiTransportCompanionInterface.h"
#include <helpers/RepeaterTcpOtaEmit.h>
#include "WifiRuntimeStore.h"   // persist BLE on/off (ble_en) across reboots
#include "WebMirror.h"          // web UI mirror bridge (served over the WS server)
#include <esp_heap_caps.h>
#include <string.h>

// Companion push code for the per-packet RX log (matches MyMesh.cpp). It is kept OFF
// the BLE transport in writeFrameToAll — see the note there (issues #46, #54) — EXCEPT
// for one-shot passes armed via bleAllowNextRxLog() (echoes of our own sends, #94).
#define PUSH_CODE_LOG_RX_DATA   0x88

bool MultiTransportCompanionInterface::s_ble_rxlog_once = false;
bool MultiTransportCompanionInterface::s_ble_rxlog_all  = false;   // #256: opt-in, per session

MultiTransportCompanionInterface::MultiTransportCompanionInterface()
  : _tcp_port(0), _ws_port(0), _tcp_started(false), _ws_started(false), _tcp_enabled(true), _isEnabled(false), _broadcast(false), _last_reply_target(REPLY_TARGET_USB), _ota_tcp_suspended(false), _ota_ws_suspended(false), _ota_ws_listen_paused(false)
#ifdef BLE_PIN_CODE
  , _ble_begun(false), _ble_enabled(false), _ota_ble_released(false), _ota_ble_was_enabled(false), _ble_pin_code(0)
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
#if defined(HAS_TDISPLAY_P4)
  // ESP-AT allows ONE listening port, so the router server (_p4_srv) owns it and dispatches
  // each inbound connection by first byte — see p4RouteClients(). _tcp/_ws never listen
  // themselves here; started flags only mark them willing to adopt routed clients.
  if (_tcp_enabled && !_tcp_started && _tcp_port != 0) {
    _p4_srv.begin(_tcp_port);
    _tcp_started = true;
  }
  if (_tcp_enabled && !_ws_started && _ws_port != 0 && wifi_connected) {
    _ws_started = true;   // enables tickHandshake + the mirror stream task; _ws itself stays listen-less
    printf("[WS] P4 router: web UI shares TCP:%u (first-byte dispatch)\n", (unsigned)_tcp_port);
  }
#else
  if (_tcp_enabled && !_tcp_started && _tcp_port != 0) {
    _tcp.begin(_tcp_port);
    _tcp_started = true;
  }
  // Plain WebSocket: start only when Wi-Fi has an address (caller defers TCP/WS start after splash).
  if (_tcp_enabled && !_ws_started && _ws_port != 0 && wifi_connected) {
    _ws.begin(_ws_port);
    _ws_started = true;
  }
#endif
}

#if defined(HAS_TDISPLAY_P4)
// One AT listener serves BOTH protocols. Accept every inbound connection here, peek its first
// byte, and hand the socket to the right server: HTTP requests start with an ASCII verb
// (GET/HEAD/POST/PUT/OPTIONS/DELETE...) and cover the web viewer page, the mirror/terminal
// WebSockets and the WS companion; companion frames from the phone app always start with '<'.
// A peer that stays silent gets the legacy benefit of the doubt after 3 s: some companion
// clients connect and only listen for pushes without ever sending.
void MultiTransportCompanionInterface::p4RouteClients() {
  if (!_tcp_started && !_ws_started) return;
  const int NPEND = (int)(sizeof(_p4_pend) / sizeof(_p4_pend[0]));
  while (_p4_srv.hasClient()) {
    int slot = -1;
    for (int i = 0; i < NPEND; i++) {
      if (!_p4_pend[i].used) {
        slot = i;
        break;
      }
    }
    if (slot < 0) break;                 // all pending slots mid-peek; the accept FIFO holds the rest
    WiFiClient incoming = _p4_srv.accept();
    if (!incoming) continue;
    _p4_pend[slot].c = incoming;
    _p4_pend[slot].ms = millis();
    _p4_pend[slot].used = true;
  }
  for (int i = 0; i < NPEND; i++) {
    if (!_p4_pend[i].used) continue;
    WiFiClient& c = _p4_pend[i].c;
    if (!c.connected()) {
      c.stop();
      _p4_pend[i].used = false;
      continue;
    }
    int b = c.peek();
    bool is_http = (b == 'G' || b == 'H' || b == 'P' || b == 'O' || b == 'D');
    if (b < 0 && millis() - _p4_pend[i].ms < 3000) continue;   // no first byte yet
    if (is_http && _ws_started) {
      _ws.adoptClient(c);
    } else if (!is_http && _tcp_started) {
      _tcp.adoptClient(c);
    } else {
      c.stop();               // the matching server is suspended (OTA) — refuse politely
    }
    _p4_pend[i].used = false;
    _p4_pend[i].c = WiFiClient();   // drop our shared link ref; the adopting server holds its own
  }
}
#endif

// Web-mirror streaming runs on CORE 0 (parallel to the UI on core 1) so the browser's
// socket stays fed at full link speed even while core 1 is busy with a heavy LVGL render.
// The WS client array is guarded by WebSocketCompanionServer's recursive mutex, so this
// task and the main-loop companion/handshake code never corrupt each other. serviceMirror
// is non-blocking, so the mutex is held only briefly and cross-core contention is tiny.
static void wsMirrorStreamTask(void* arg) {
  WebSocketCompanionServer* ws = static_cast<WebSocketCompanionServer*>(arg);
  for (;;) {
    ws->serviceMirror(g_web_mirror);
    vTaskDelay(pdMS_TO_TICKS(g_web_mirror.clients() > 0 ? 2 : 25));  // fast when a browser is watching, idle otherwise
  }
}

void MultiTransportCompanionInterface::tickWebSocketHandshake() {
#if defined(HAS_TDISPLAY_P4)
  p4RouteClients();        // dispatch newly accepted AT-listener connections to _tcp / _ws
#endif
  if (_ws_started) {
    _ws.tickHandshake();   // accept + WS handshake stay on core 1 (the main loop)
    if (!_mirror_task) {   // spawn the core-0 streamer once, on first tick after the WS server is up
      xTaskCreatePinnedToCore(wsMirrorStreamTask, "ws_mirror", 4096, &_ws, 2,
                              reinterpret_cast<TaskHandle_t*>(&_mirror_task), 0);
    }
  }
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
#if defined(HAS_TDISPLAY_P4)
  _p4_srv.stop();          // the router owns the AT listener (see startTcpServer)
  for (size_t i = 0; i < sizeof(_p4_pend) / sizeof(_p4_pend[0]); i++) {
    if (_p4_pend[i].used) {
      _p4_pend[i].c.stop();
      _p4_pend[i].c = WiFiClient();
      _p4_pend[i].used = false;
    }
  }
#endif
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
  // ATT notification payload is ATT_MTU-3, so SerialBLEInterface::begin()'s
  // setMTU(MAX_FRAME_SIZE) leaves a full-length frame 3 bytes too big and NimBLE
  // silently drops the overflow (ble_att_truncate_to_mtu()). MyMesh::queueMessage()
  // clamps message text to exactly MAX_FRAME_SIZE, so a maximum-length message lost
  // its last 3 bytes -- over BLE only; USB/TCP/WS were unaffected. Widen it here
  // rather than in the core: that file is upstream MeshCore, this transport is
  // vendored in our src/, and setMTU() is a public static, so no core change is
  // needed. Verified on an M9: the app now negotiates ATT MTU 179 (was 176).
  NimBLEDevice::setMTU(MAX_FRAME_SIZE + 3);
  _ble_begun = true;
  _ble_enabled = true;
  _ota_ble_released = false;
  _ota_ble_was_enabled = false;
  _ble.enable();
}

void MultiTransportCompanionInterface::enableBle() {
  if (!_ble_begun) {
    // Deferred at boot (heap guard) or toggled on from off: bring the stack up
    // now, live, from the params stashed by prepareBle()/beginBle().
    if (_ble_prefix[0] == '\0' && _ble_name[0] == '\0') return;   // no params known
#if defined(TLORA_PAGER)
    // An idle STA may be kept up for on-device scans with no credentials, and
    // a failed association explicitly falls back to BLE. Only the ordered WPA
    // phase blocks a cold NimBLE start; main.cpp owns every phase transition.
    if (wifiConfigPagerWifiBlocksBle()) {
      Serial.println("[ble] cold start deferred until T-Pager Wi-Fi associates");
      return;
    }
#endif
    // Only a cold NimBLE start needs the coexistence reserve. Re-enabling an
    // already-created stack below this threshold is allocation-free and must
    // not be rejected (UITask used to gate both cases identically, trapping the
    // user with BLE resident-but-off and neither radio enableable).
    const size_t BLE_COEXIST_MIN_FREE  = 50u * 1024u;
    const size_t BLE_COEXIST_MIN_BLOCK = 20u * 1024u;
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const size_t internal_free = heap_caps_get_free_size(internal_caps);
    const size_t internal_max  = heap_caps_get_largest_free_block(internal_caps);
    if (internal_free < BLE_COEXIST_MIN_FREE ||
        internal_max < BLE_COEXIST_MIN_BLOCK) {
      Serial.printf("[ble] cold start refused: free=%u maxblk=%u\n",
                    (unsigned)internal_free, (unsigned)internal_max);
      return;
    }
    char name[sizeof(_ble_name)];
    strncpy(name, _ble_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    _ble.begin(_ble_prefix, name, _ble_pin_code);
    NimBLEDevice::setMTU(MAX_FRAME_SIZE + 3);   // see beginBle(): ATT payload is MTU-3
    _ble_begun = true;
  }
  _ble_enabled = true;
  wifiConfigSetBleEnabled(true);    // persist so it survives reboot
  _ble.enable();
#if defined(TLORA_PAGER)
  // A successful live enable means NimBLE is now resident. Do not let the
  // Arduino Wi-Fi event path enter WPA automatically after a later link loss.
  // main.cpp first releases NimBLE, then explicitly re-associates and recreates
  // BLE after GOT_IP so the same Wi-Fi-first ordering holds without a reboot.
  // Avoid touching Wi-Fi when it has never been initialised (BLE-only mode).
  if (WiFi.getMode() != WIFI_MODE_NULL) WiFi.setAutoReconnect(false);
#endif
}

#ifdef BLE_PIN_CODE
// NimBLEDevice::deinit(true) deletes the NimBLEServer, and ~NimBLEServer() does
//   if (m_deleteCallbacks && m_pServerCallbacks != &defaultCallbacks)
//     delete m_pServerCallbacks;
// SerialBLEInterface::begin() registers itself with pServer->setCallbacks(this),
// whose deleteCallbacks parameter defaults to true. Our SerialBLEInterface is the
// by-value member _ble of this object, and this object is placement-new'd into a
// single heap block in main.cpp — so that delete would call operator delete on an
// INTERIOR pointer and corrupt the heap. setCallbacks(nullptr) installs NimBLE's
// static defaultCallbacks sentinel, which the destructor above explicitly skips.
// (NimBLECharacteristic never deletes its callbacks, so only the server matters.)
static void bleDetachServerCallbacks() {
  if (NimBLEServer* server = NimBLEDevice::getServer()) server->setCallbacks(nullptr);
}
#endif

void MultiTransportCompanionInterface::disableBle() {
  _ble_enabled = false;
  wifiConfigSetBleEnabled(false);   // persist so BT stays off across reboot
  // deinit(true) below deletes NimBLE's server, but SerialBLEInterface keeps
  // its cached server pointer. Never call disable() again after that teardown;
  // a later begin() replaces the cached pointers with a fresh GATT server.
  if (_ble_begun) _ble.disable();    // stop advertising + drop any connection
  // Pager only: release NimBLE whenever the user turns Bluetooth off. This
  // returns its internal heap; a later enable recreates the GATT server after
  // Wi-Fi is already stable. Pager reconnects remain main-loop-owned so their
  // WPA phase is always visible to the BLE ownership gate.
  // Other boards retain their established resident-stack disable/re-enable
  // behavior.
#if defined(TLORA_PAGER)
  if (_ble_begun) {
    bleDetachServerCallbacks();
    NimBLEDevice::deinit(true);
    _ble_begun = false;
  }
  if (WiFi.getMode() != WIFI_MODE_NULL) WiFi.setAutoReconnect(false);
#endif
}

#if defined(TLORA_PAGER)
bool MultiTransportCompanionInterface::suspendBleForWifiReconnect() {
  // Preserve both the user's BLE preference and NimBLE's live bond/GATT state.
  // Recreating the controller/server here made an already-bonded phone fall
  // into repeat pairing after every Wi-Fi handoff; NimBLE then discarded its
  // side of the bond while the phone retained the old LTK. Stopping advertising
  // and disconnecting the peer removes BLE traffic from the association window
  // without invalidating that long-term security state.
  if (_ble_begun) {
    if (_ble_enabled) _ble.disable();
    // ble_gap_terminate() is asynchronous. Do not start WPA while the old BLE
    // link is still on air; that recreates the exact overlap this handoff is
    // meant to prevent. Wait for the NimBLE host's connection table to drain,
    // with a bounded failure so a wedged peer cannot stall the main loop/WDT.
    NimBLEServer* server = NimBLEDevice::getServer();
    const uint32_t started = millis();
    while (server && server->getConnectedCount() != 0 &&
           (uint32_t)(millis() - started) < 1000u) {
      delay(1);
    }
    if (server && server->getConnectedCount() != 0) {
      Serial.println("[ble] disconnect timed out; Wi-Fi handoff cancelled");
      _ble_enabled = false;
      return false;
    }
  }
  _ble_enabled = false;
  if (WiFi.getMode() != WIFI_MODE_NULL) WiFi.setAutoReconnect(false);
  return true;
}
#endif

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
  // A prior disableBle() may have fully deinitialised NimBLE and left the
  // wrapped SerialBLEInterface's cached server pointer dangling.
  if (_ble_begun) _ble.disable();
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
  if (_ble_begun) {
    _ota_ble_was_enabled = _ble_enabled;
    if (_ble_enabled) _ble.disable();
    bleDetachServerCallbacks();
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
    _ble_enabled = _ota_ble_was_enabled;
    if (_ble_enabled) _ble.enable();
    _ota_ble_released = false;
    _ota_ble_was_enabled = false;
    meshcoreRepeaterTcpOtaEmitLine("OTA: restored BLE stack");
  }
#endif
  if (_ota_tcp_suspended) {
#if defined(HAS_TDISPLAY_P4)
    _tcp_started = true;   // the router still owns the AT listener; just resume adopting
#else
    _tcp.begin(_tcp_port);
    _tcp_started = true;
#endif
    _ota_tcp_suspended = false;
    meshcoreRepeaterTcpOtaEmitLine("OTA: restored companion TCP server");
  }
  if (_ota_ws_suspended) {
#if defined(HAS_TDISPLAY_P4)
    _ws_started = true;    // ditto — never begin a second AT listener on _ws_port
#else
    _ws.begin(_ws_port);
    _ws_started = true;
#endif
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
  // #256: a companion that wants the whole firehose (coverage/region mapping —
  // region and the full path exist ONLY in this frame) can ask for it per
  // session with CMD_SET_CUSTOM_VAR "ble.rxlog:1". Default stays off, so the
  // #46/#54 behaviour above is unchanged for every app that does not opt in.
  const bool ble_pass = rxlog && (s_ble_rxlog_once || s_ble_rxlog_all);
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
