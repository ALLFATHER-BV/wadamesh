#pragma once
#if defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>

// Publishes received mesh messages to an MQTT broker over the existing WiFi link.
//
// PRIVACY MODEL (mirrors how Meshtastic's MQTT module guards content):
//   - Master enable is opt-in (default off).
//   - Channel messages publish by default; DIRECT MESSAGES are off by default and
//     must be explicitly enabled — they are private 1:1 traffic, and may be from
//     someone else who never consented to being bridged.
//   - If an encryption key (PSK) is set, every JSON payload is sealed with
//     AES-256-GCM (fresh random 12-byte nonce per message) before it leaves the
//     device, so the broker only ever sees opaque base64. TLS is deliberately NOT
//     used: the mbedTLS handshake (~30 KB heap) does not fit the ESP32-S3 budget
//     alongside Wi-Fi + BLE + LVGL (same reason map tiles are HTTP-only). App-layer
//     GCM is the lightweight, hardware-accelerated equivalent.
//
// Topics (QoS 0, retained where noted):
//   wadamesh/{node_hex}/msg/dm   — direct / signed messages (only if DM publish ON)
//   wadamesh/{node_hex}/msg/ch   — channel messages (only if channel publish ON)
//   wadamesh/{node_hex}/status   — LWT: "online" on connect, "offline" on drop (retained)
//
// ENCRYPTED PAYLOAD WIRE FORMAT (when a PSK is set):
//   base64( nonce[12] || ciphertext[n] || tag[16] ),  AES-256-GCM,
//   key = SHA-256(psk).  A subscriber (Home Assistant / Node-RED) decrypts with the
//   same passphrase. With no PSK, the payload is the plain JSON (use a private broker).
//
// Config persisted in Preferences namespace "mqtt" (file-backed via SdNvsPrefs):
//   en bool · host str · port u16 · user str · pwd str · dm bool · ch bool · psk str
//
// Call begin() once after the_mesh.begin() and SdNvsPrefs::useFile().
// Call loop() every iteration of the Arduino loop().
class MqttBridge {
public:
    void begin(const char* nodeHex);
    void loop();

    void publishDM(const char* senderName, const uint8_t* senderKey32,
                   float snr, uint8_t hops, uint32_t ts, const char* text);
    void publishChannel(int channelIdx, const char* channelName,
                        float snr, uint8_t hops, uint32_t ts, const char* text);

    bool enabled() const { return _enabled; }

    // Persist config (called from Settings UI save).
    static void saveConfig(const char* host, uint16_t port,
                           const char* user, const char* pwd,
                           bool pubDm, bool pubChannel, const char* psk, bool enable);
    // Re-read config from Preferences and reconnect (call after saveConfig).
    void reloadConfig();

private:
    WiFiClient   _wc;
    PubSubClient _mqtt{_wc};
    char     _nodeHex[13] = {};   // 6-byte key → 12 hex chars + '\0'
    bool     _enabled     = false;
    bool     _pubDm       = false; // DMs off by default (private 1:1 traffic)
    bool     _pubChannel  = true;  // channel messages on by default
    char     _host[64]    = {};
    char     _user[32]    = {};
    char     _pwd[32]     = {};
    char     _psk[33]     = {};    // passphrase; empty = no payload encryption
    uint8_t  _key[32]     = {};    // SHA-256(psk), valid when _encOn
    bool     _encOn       = false;
    uint16_t _port        = 1883;
    uint32_t _lastReconnectMs = 0;

    static const uint32_t RECONNECT_INTERVAL_MS = 15000;

    void loadConfig();             // shared by begin() / reloadConfig()
    void deriveKey();              // _key = SHA-256(_psk); sets _encOn
    bool reconnect();
    void pub(const char* subtopic, const char* json);  // seals if _encOn
    bool sealToB64(const char* plain, char* out, size_t outCap);
    static void escapeJson(const char* src, char* dst, size_t dstLen);
};

extern MqttBridge mqtt_bridge;

#endif // ESP32 && MULTI_TRANSPORT_COMPANION
