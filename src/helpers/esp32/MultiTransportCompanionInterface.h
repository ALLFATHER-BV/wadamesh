#pragma once

#include <helpers/BaseSerialInterface.h>
#include <helpers/ArduinoSerialInterface.h>
#include "TCPCompanionServer.h"
#include "WebSocketCompanionServer.h"
#ifdef BLE_PIN_CODE
#include <helpers/esp32/SerialBLEInterface.h>
#endif

#ifndef TCP_COMPANION_DEFAULT_PORT
#define TCP_COMPANION_DEFAULT_PORT  5000
#endif

// Reply target: -1 = USB, -2 = BLE (when BLE_PIN_CODE), 0..N = TCP client index, 100+ = WebSocket client index
#define REPLY_TARGET_USB   (-1)
#define REPLY_TARGET_BLE   (-2)
#define REPLY_TARGET_WS_0  100

// Implements BaseSerialInterface for simultaneous USB + TCP companion connections.
// One shared protocol handler; responses go to originating client, optionally broadcast to all.
class MultiTransportCompanionInterface : public BaseSerialInterface {
public:
  MultiTransportCompanionInterface();

  // USB uses Serial (or other Stream). TCP and optional WebSocket ports stored; call startTcpServer() after WiFi.begin().
  void begin(Stream& usb_serial, uint16_t tcp_port = TCP_COMPANION_DEFAULT_PORT, uint16_t ws_port = 0);
  // Pass wifi_connected so the WebSocket server starts only after Wi-Fi has an address.
  void startTcpServer(bool wifi_connected = true);  // idempotent; no-op if TCP disabled
  void stopTcpServer();   // stop TCP server and disconnect clients; prevents startTcpServer until enableTcp()

#ifdef BLE_PIN_CODE
  // Call after begin() and the_mesh is ready (e.g. after startInterface). Enables BLE by default.
  void beginBle(const char* prefix, char* name, uint32_t pin_code);
  // Store the BLE name/pin WITHOUT bringing the stack up. Used at boot when the
  // heap guard defers co-initialising BLE alongside Wi-Fi: the params are kept so
  // a later enableBle() can lazily bring BLE up live (no reboot).
  void prepareBle(const char* prefix, char* name, uint32_t pin_code);
  void enableBle() override;
  void disableBle() override;
  bool isBleEnabled() const override { return _ble_enabled; }
#if defined(HAS_TDISPLAY_P4)
  // T-Display P4: the factory C6 ESP-AT firmware has its BLE advertising commands stubbed
  // (BLEADVDATA/ADVSTART all ERROR — Meck-P4 hit the same wall and ships Wi-Fi companion), and
  // reflashing the C6 is ruled out for users. Phone pairing on this board = Wi-Fi (TCP:5000) or
  // USB; the UI hides its Bluetooth toggles off this. The MeshCore GATT table stays pre-provisioned
  // on the C6 (c6_at) so a future LilyGo AT firmware with working advertising lights up cheaply.
  bool hasBleCapability() const override { return false; }
#else
  bool hasBleCapability() const override { return true; }
#endif
  bool getBlePeerAddress(char* buf, size_t len) const override;
#endif

  void enableTcp() override;
  void disableTcp() override;
  bool isTcpEnabled() const override { return _tcp_enabled; }
  bool isWsStarted() const override { return _ws_started; }
#if defined(HAS_TDISPLAY_P4)
  // The web UI shares the companion TCP port (single AT listener + first-byte router),
  // so that's the port to show/open — _ws_port is never listened on here.
  uint16_t getWsPort() const override { return _tcp_port; }
#else
  uint16_t getWsPort() const override { return _ws_port; }
#endif
  int getWsConnectedCount() const override { return _ws.connectedCount(); }
  /** Accept WebSocket clients / handshakes; call from main loop. */
  void tickWebSocketHandshake() override;

  void setBroadcastResponses(bool enable) { _broadcast = enable; }

  void enable() override;
  void disable() override;
  bool isEnabled() const override { return _isEnabled; }
  void prepareForHttpOta() override;
  void restoreAfterHttpOta() override;
  bool isHttpOtaWifiControlSession() const override;
  bool isConnected() const override;
  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t writeFrameToAll(const uint8_t src[], size_t len) override;
  /** One-shot: let the NEXT RX-log frame (PUSH_CODE_LOG_RX_DATA) through to BLE.
   *  RX logs are normally kept OFF BLE (#46/#54 congestion), but the app's
   *  "Repeats heard" is computed from them — MyMesh::logRxRaw calls this right
   *  before writeFrameToAll when the frame is an echo of OUR OWN send, so BLE
   *  gets exactly the few frames the app needs and none of the flood (#94).
   *  Same-thread set-then-consume (both in the mesh loop), so no atomics. */
  static void bleAllowNextRxLog() { s_ble_rxlog_once = true; }
  bool companionUnsolicitedPushesBroadcastToAll() const override { return _broadcast; }
  size_t checkRecvFrame(uint8_t dest[]) override;

  void setCurrentClientId(const char* id) override;
  void getCurrentClientId(char* dest, size_t max_len) const override;

  int getReplyTarget() const override { return _last_reply_target; }
  void setReplyTarget(int target) override { _last_reply_target = target; }

private:
  static bool s_ble_rxlog_once;   // one-shot BLE pass for the next RX-log frame (#94)
public:

private:
  int _clientIdSlot() const;
  static const size_t _max_client_id_len = 32;

  ArduinoSerialInterface _usb;
  TCPCompanionServer _tcp;
  WebSocketCompanionServer _ws;
  uint16_t _tcp_port;
  uint16_t _ws_port;
  bool _tcp_started;
  bool _ws_started;
#if defined(HAS_TDISPLAY_P4)
  // ESP-AT allows ONE listening port, so this router server owns it: every inbound
  // connection is accepted here, held briefly, and dispatched on its first byte —
  // an HTTP verb ('G','H','P','O','D') goes to _ws (web mirror/VNC/remote/terminal +
  // WS companion), everything else ('<' companion frames) to _tcp (phone app).
  // _tcp/_ws never begin() their own listeners on this board (their C6Server stays
  // inert); they receive clients via adoptClient().
  WiFiServer _p4_srv;
  struct P4PendingClient { WiFiClient c; uint32_t ms = 0; bool used = false; };
  P4PendingClient _p4_pend[3];
  void p4RouteClients();
#endif
  void* _mirror_task = nullptr;   // TaskHandle_t for the core-0 web-mirror stream task (created once)
  bool _tcp_enabled;   // if false, startTcpServer() no-ops until enableTcp()
  bool _isEnabled;
  bool _broadcast;           // if true, also send responses to all other clients
  int _last_reply_target;    // REPLY_TARGET_USB, REPLY_TARGET_BLE, TCP index, or REPLY_TARGET_WS_0 + ws index
  bool _ota_tcp_suspended;
  bool _ota_ws_suspended;
  bool _ota_ws_listen_paused;
#ifdef BLE_PIN_CODE
  SerialBLEInterface _ble;
  bool _ble_begun;    // beginBle() was called
  bool _ble_enabled;  // user has BLE on (toggle via UI)
  bool _ota_ble_released;
  char _ble_prefix[24];
  char _ble_name[48];
  uint32_t _ble_pin_code;
#endif
  char _client_ids[2 + TCP_COMPANION_MAX_CLIENTS + WS_COMPANION_MAX_CLIENTS][_max_client_id_len];  // usb, [ble], tcp0.., ws0..
};
