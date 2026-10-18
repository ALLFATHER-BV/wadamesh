#pragma once
// Wi-Fi on the web board: there is no Wi-Fi radio to drive. The browser (or the host OS)
// already has a network, so status() is WL_CONNECTED while it is online (wada_http.h
// wadaNetOnline: what the UI's map tile download checks) and the "network" is the
// browser's own. There is still no address of our own (localIP 0.0.0.0, so the
// version check, File Transfer and Remote stay off), scans fail, and nothing here
// configures anything (Settings > Wi-Fi says so: CAP_WIFI_SETUP 0). Network access for
// the UI goes through HTTPClient.h -> wadaHttpRequest().
#include "Arduino.h"
#include "wada_http.h"
#include "arduino/IPAddress.h"
#include "WiFiClient.h"
#include "WiFiServer.h"

typedef enum {
  WL_NO_SHIELD = 255, WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL = 1, WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3, WL_CONNECT_FAILED = 4, WL_CONNECTION_LOST = 5, WL_DISCONNECTED = 6
} wl_status_t;
typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA, WIFI_MODE_MAX } wifi_mode_t;
#define WIFI_OFF   WIFI_MODE_NULL
#define WIFI_STA   WIFI_MODE_STA
#define WIFI_AP    WIFI_MODE_AP
#define WIFI_AP_STA WIFI_MODE_APSTA
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED  (-2)
typedef enum { WIFI_AUTH_OPEN = 0, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK,
               WIFI_AUTH_WPA2_ENTERPRISE, WIFI_AUTH_WPA3_PSK, WIFI_AUTH_WPA2_WPA3_PSK, WIFI_AUTH_MAX } wifi_auth_mode_t;
typedef enum { WIFI_POWER_19_5dBm = 78, WIFI_POWER_8_5dBm = 34, WIFI_POWER_2dBm = 8 } wifi_power_t;
typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
#define WIFI_PROTOCOL_11B 1
#define WIFI_PROTOCOL_11G 2
#define WIFI_PROTOCOL_11N 4
#define WIFI_BGN (WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N)
typedef int arduino_event_id_t;

class WiFiClass {
public:
  wl_status_t status() { return wadaNetOnline() ? WL_CONNECTED : WL_DISCONNECTED; }
  bool isConnected() { return wadaNetOnline(); }
  wifi_mode_t getMode() { return WIFI_MODE_NULL; }
  bool mode(wifi_mode_t m) { (void)m; return false; }
  bool enableSTA(bool) { return false; }
  wl_status_t begin(const char* ssid, const char* pass = nullptr, int32_t ch = 0, const uint8_t* bssid = nullptr, bool connect = true) {
    (void)ssid; (void)pass; (void)ch; (void)bssid; (void)connect; return WL_NO_SHIELD;
  }
  bool disconnect(bool wifioff = false, bool eraseap = false) { (void)wifioff; (void)eraseap; return true; }
  bool reconnect() { return false; }
  bool setAutoReconnect(bool) { return false; }
  bool getAutoReconnect() { return false; }
  bool setSleep(bool) { return true; }
  bool setSleep(wifi_ps_type_t) { return true; }
  bool setTxPower(wifi_power_t) { return false; }
  bool setHostname(const char*) { return true; }
  const char* getHostname() { return "wadamesh-web"; }
  void persistent(bool) {}
  void useStaticBuffers(bool) {}
  bool config(IPAddress, IPAddress, IPAddress, IPAddress = (uint32_t)0, IPAddress = (uint32_t)0) { return false; }
  int16_t scanNetworks(bool async = false, bool show_hidden = false, bool passive = false, uint32_t max_ms = 300, uint8_t ch = 0) {
    (void)async; (void)show_hidden; (void)passive; (void)max_ms; (void)ch; return WIFI_SCAN_FAILED;
  }
  int16_t scanComplete() { return WIFI_SCAN_FAILED; }
  void scanDelete() {}
  String SSID() const { return wadaNetOnline() ? String("Browser") : String(); }
  String SSID(uint8_t) const { return String(); }
  int32_t RSSI() { return wadaNetOnline() ? -50 : 0; }   // "online" reads as full bars
  int32_t RSSI(uint8_t) { return 0; }
  int32_t channel() { return 0; }
  int32_t channel(uint8_t) { return 0; }
  wifi_auth_mode_t encryptionType(uint8_t) { return WIFI_AUTH_OPEN; }
  uint8_t* BSSID(uint8_t = 0) { return nullptr; }
  String BSSIDstr(uint8_t = 0) { return String(); }
  IPAddress localIP() { return IPAddress((uint32_t)0); }
  IPAddress gatewayIP() { return IPAddress((uint32_t)0); }
  IPAddress subnetMask() { return IPAddress((uint32_t)0); }
  IPAddress dnsIP(uint8_t = 0) { return IPAddress((uint32_t)0); }
  IPAddress softAPIP() { return IPAddress((uint32_t)0); }
  String macAddress() { return String("02:57:41:44:41:01"); }
  uint8_t* macAddress(uint8_t* mac) { static const uint8_t k[6] = {0x02, 0x57, 0x41, 0x44, 0x41, 0x01}; memcpy(mac, k, 6); return mac; }
  bool softAP(const char*, const char* = nullptr, int = 1, int = 0, int = 4) { return false; }
  bool softAPdisconnect(bool = false) { return true; }
  int hostByName(const char*, IPAddress&) { return 0; }
  static void onEvent(void (*)(arduino_event_id_t), arduino_event_id_t = 0) {}
};
extern WiFiClass WiFi;
