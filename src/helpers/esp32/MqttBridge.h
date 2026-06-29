#pragma once
#if defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>

// Publishes received mesh messages to an MQTT broker over the existing WiFi link.
//
// Topics (QoS 0, retained where noted):
//   wadamesh/{node_hex}/msg/dm   — direct / signed messages received from the mesh
//   wadamesh/{node_hex}/msg/ch   — channel messages received from the mesh
//   wadamesh/{node_hex}/status   — LWT: "online" on connect, "offline" on drop (retained)
//
// Config persisted in Preferences namespace "mqtt" (file-backed via SdNvsPrefs):
//   en         bool    master enable (default false — opt-in)
//   host       string  broker hostname or IP
//   port       uint16  broker port (default 1883)
//   user       string  username (optional)
//   pwd        string  password (optional)
//   topic_pfx  string  topic prefix (default "wadamesh")
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
                           const char* topic_prefix, bool enable);
    // Re-read config from Preferences and reconnect (call after saveConfig).
    void reloadConfig();

private:
    WiFiClient   _wc;
    PubSubClient _mqtt{_wc};
    char     _nodeHex[13]      = {};   // 6-byte key → 12 hex chars + '\0'
    bool     _enabled          = false;
    char     _host[64]         = {};
    char     _user[32]         = {};
    char     _pwd[32]          = {};
    char     _topic_prefix[48] = {};   // MQTT topic prefix (default "wadamesh")
    uint16_t _port        = 1883;
    uint32_t _lastReconnectMs = 0;

    static const uint32_t RECONNECT_INTERVAL_MS = 15000;

    bool reconnect();
    void pub(const char* subtopic, const char* json);
    static void escapeJson(const char* src, char* dst, size_t dstLen);
};

extern MqttBridge mqtt_bridge;

#endif // ESP32 && MULTI_TRANSPORT_COMPANION
