#pragma once
#if defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)

#include <Arduino.h>

#if defined(HAS_TANMATSU) || defined(HAS_TDISPLAY_P4)
// ---- MQTT DISABLED on the ESP32-P4 boards (Tanmatsu + T-Display P4) ----
// The P4's Wi-Fi runs through the esp-hosted C6 co-processor; the Arduino WiFiClient / PubSubClient
// path this bridge uses is not wired for that and crashed at boot. Provide a no-op bridge with the
// SAME public API so the shared companion + settings code links without pulling PubSubClient into
// the P4 build. Re-enable once esp-hosted TCP is proven on the P4.
class MqttBridge {
public:
    void begin(const char* /*nodeHex*/) {}
    void loop() {}
    void publishDM(const char*, const uint8_t*, float, uint8_t, uint32_t, const char*) {}
    void publishChannel(int, const char*, float, uint8_t, uint32_t, const char*) {}
    bool enabled() const { return false; }
    static void saveConfig(const char*, uint16_t, const char*, const char*,
                           bool, bool, const char*, bool) {}
    void reloadConfig() {}
};
#else
#include <PubSubClient.h>
#include <WiFiClient.h>

// WiFiClient whose write() refuses to enter the framework's blocking-select path:
// bytes are only handed to lwIP when the socket can take them right now, so a
// wedged broker connection (half-open socket, peer stopped ACKing) fails the
// publish instead of stalling the loop thread ~1 s per write attempt. Same probe
// as the TCP/WS companion servers.
class MqttNbClient : public WiFiClient {
public:
    size_t write(const uint8_t* buf, size_t size) override;
    size_t write(uint8_t b) override { return write(&b, 1); }
};

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
    MqttNbClient _wc;
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
    // True while the one-shot connect task owns _mqtt/_wc. The loop thread must
    // not touch either until it clears (PubSubClient is not thread-safe).
    volatile bool _connecting = false;

    static const uint32_t RECONNECT_INTERVAL_MS = 15000;

    void loadConfig();             // shared by begin() / reloadConfig()
    void deriveKey();              // _key = SHA-256(_psk); sets _encOn
    bool reconnect();
    static void reconnectTask(void* arg);   // one-shot task body wrapping reconnect()
    void pub(const char* subtopic, const char* json);  // seals if _encOn
    bool sealToB64(const char* plain, char* out, size_t outCap);
    static void escapeJson(const char* src, char* dst, size_t dstLen);
};
#endif // HAS_TANMATSU (no-op stub) vs real MqttBridge

extern MqttBridge mqtt_bridge;

#endif // ESP32 && MULTI_TRANSPORT_COMPANION
