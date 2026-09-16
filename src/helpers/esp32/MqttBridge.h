#pragma once
#if defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)

#include <Arduino.h>

namespace mesh { class LocalIdentity; }

enum class MqttObserverProfile : uint8_t {
    Custom = 0,
    NebraskaMesh = 1,
    MichMesh = 2,
    AnalyzerUs,
    AnalyzerEu,
    NzAnalyzer,
    MeshMapper,
    MeshRank,
    Waev,
    Meshomatic,
    CascadiaMesh,
    TennMesh,
    NashMesh,
    CtMesh,
    ChiMesh,
    MeshatSe,
    EastIdahoMesh,
    ColoradoMesh,
    DutchMeshcore1,
    DutchMeshcore2,
    MeshcoreCa1,
    MeshcoreCa2,
    MeshcoreFi,
    OkiMesh1,
    OkiMesh2,
    InwMesh,
    BostonMesh,
    RfLab,
    IpntUk,
    FlMesh,
    CoreComms,
    MeshTexas,
    MeshChaun14,
    WcMesh,
    AtvirasTinklas,
    GoMesh,
    IdahoMesh,
    NtxMesh,
    BsMesh,
    Count,
};

enum class MqttBridgePhase : uint8_t {
    Unavailable,
    Disabled,
    Misconfigured,
    WaitingForWifi,
    WaitingForTime,
    Connecting,
    Connected,
    RetryWait,
};

struct MqttBridgeStatus {
    MqttBridgePhase phase = MqttBridgePhase::Unavailable;
    bool available = false;
    bool requestedEnabled = false;
    bool publishDm = false;
    bool publishChannel = false;
    bool publishObserver = false;
    MqttObserverProfile observerProfile = MqttObserverProfile::Custom;
    bool encrypted = false;
    char host[64] = {};
    uint16_t port = 1883;
    int32_t lastResult = -1;
    int32_t lastTlsError = 0;
    int32_t lastTlsStackError = 0;
    int32_t lastSocketError = 0;
    uint32_t lastAttemptMs = 0;
    uint32_t lastConnectedMs = 0;
    uint32_t lastResultMs = 0;
    uint32_t retryAtMs = 0;
    uint32_t observerPublished = 0;
    uint32_t observerDropped = 0;
    uint8_t observerQueued = 0;
};

#if defined(HAS_TANMATSU) || defined(HAS_TDISPLAY_P4)
// ---- MQTT DISABLED on the ESP32-P4 boards (Tanmatsu + T-Display P4) ----
// The P4's Wi-Fi runs through the esp-hosted C6 co-processor; the Arduino WiFiClient / PubSubClient
// path this bridge uses is not wired for that and crashed at boot. Provide a no-op bridge with the
// SAME public API so the shared companion + settings code links without pulling PubSubClient into
// the P4 build. Re-enable once esp-hosted TCP is proven on the P4.
class MqttBridge {
public:
    void begin(const mesh::LocalIdentity*, const char*, float, float, uint8_t, uint8_t, const char*) {}
    void loop() {}
    void observeRx(float, float, const uint8_t*, int) {}
    void publishDM(const char*, const uint8_t*, float, uint8_t, uint32_t, const char*) {}
    void publishChannel(int, const char*, float, uint8_t, uint32_t, const char*) {}
    bool enabled() const { return false; }
    void getStatus(MqttBridgeStatus& out) const { out = MqttBridgeStatus{}; }
    static void saveConfig(const char*, uint16_t, const char*, const char*,
                           bool, bool, const char*, bool) {}
    static void saveObserverConfig(bool, MqttObserverProfile, const char*, const char*, const char*, const char*) {}
    static uint8_t observerProfileCount();
    static const char* observerProfileOptions();
    static const char* observerProfileLabel(MqttObserverProfile profile);
    static MqttObserverProfile observerProfileFromIndex(uint16_t index);
    static uint16_t observerProfileIndex(MqttObserverProfile profile);
    static bool observerProfileNeedsIata(MqttObserverProfile profile);
    static bool observerProfileNeedsToken(MqttObserverProfile profile);
    static bool observerProfileNeedsUsername(MqttObserverProfile profile);
    static bool observerProfileNeedsPassword(MqttObserverProfile profile);
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
//   - If an encryption key (PSK) is set, decoded message JSON is sealed with
//     AES-256-GCM (fresh random 12-byte nonce per message) before it leaves the
//     device. Observer payloads remain standard JSON. The custom profile uses
//     plain MQTT; community profiles use their WSS endpoints with CA-bundle
//     verification and an Ed25519-signed identity token.
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
//   en · host · port · user · pwd · dm · ch · psk, plus obs · obs_profile ·
//   obs_origin · obs_iata · obs_topic · obs_token.
//
// Call begin() once after the_mesh.begin() and SdNvsPrefs::useFile().
// Call loop() every iteration of the Arduino loop().
class MqttBridge {
public:
    void begin(const mesh::LocalIdentity* identity, const char* nodeName,
               float frequencyMhz, float bandwidthKhz, uint8_t spreadingFactor,
               uint8_t codingRate, const char* firmwareVersion);
    void loop();
    void observeRx(float snr, float rssi, const uint8_t* raw, int len);

    void publishDM(const char* senderName, const uint8_t* senderKey32,
                   float snr, uint8_t hops, uint32_t ts, const char* text);
    void publishChannel(int channelIdx, const char* channelName,
                        float snr, uint8_t hops, uint32_t ts, const char* text);

    bool enabled() const { return _enabled; }
    void getStatus(MqttBridgeStatus& out) const;

    // Persist config (called from Settings UI save).
    static void saveConfig(const char* host, uint16_t port,
                           const char* user, const char* pwd,
                           bool pubDm, bool pubChannel, const char* psk, bool enable);
    static void saveObserverConfig(bool enable, MqttObserverProfile profile, const char* origin,
                                   const char* iata, const char* topicTemplate,
                                   const char* profileToken);
    static uint8_t observerProfileCount();
    static const char* observerProfileOptions();
    static const char* observerProfileLabel(MqttObserverProfile profile);
    static MqttObserverProfile observerProfileFromIndex(uint16_t index);
    static uint16_t observerProfileIndex(MqttObserverProfile profile);
    static bool observerProfileNeedsIata(MqttObserverProfile profile);
    static bool observerProfileNeedsToken(MqttObserverProfile profile);
    static bool observerProfileNeedsUsername(MqttObserverProfile profile);
    static bool observerProfileNeedsPassword(MqttObserverProfile profile);
    // Re-read config from Preferences and reconnect (call after saveConfig).
    void reloadConfig();

private:
    MqttNbClient _wc;
    PubSubClient _mqtt{_wc};
    char     _nodeHex[13] = {};   // legacy topic: first 6 key bytes, lowercase
    char     _nodeId[65] = {};    // Observer topic: complete public key, uppercase
    char     _nodeName[33] = {};
    char     _observerRadio[48] = {};
    char     _firmwareVersion[24] = {};
    bool     _requestedEnabled = false;
    bool     _enabled     = false;
    bool     _plainEnabled = false;
    bool     _pubDm       = false; // DMs off by default (private 1:1 traffic)
    bool     _pubChannel  = true;  // channel messages on by default
    bool     _pubObserver = false; // raw RX packets in MeshCore Observer format
    MqttObserverProfile _observerProfile = MqttObserverProfile::Custom;
    const mesh::LocalIdentity* _identity = nullptr;
    char     _host[64]    = {};
    char     _user[65]    = {};
    char     _pwd[97]     = {};
    char     _psk[33]     = {};    // passphrase; empty = no payload encryption
    char     _observerOrigin[33] = {};
    char     _observerIata[8] = {};
    char     _observerTopic[96] = {};
    char     _observerToken[65] = {};
    uint8_t  _key[32]     = {};    // SHA-256(psk), valid when _encOn
    bool     _encOn       = false;
    uint16_t _port        = 1883;
    uint32_t _lastReconnectMs = 0;
    uint32_t _lastObserverStatusMs = 0;
    uint32_t _observerPublished = 0;
    uint32_t _observerDropped = 0;
    void*    _observerCloudClient = nullptr;
    volatile bool _observerCloudConnected = false;
    uint32_t _observerCloudRetryAtMs = 0;
    uint32_t _observerCloudTokenExpires = 0;

    static const uint8_t OBSERVER_QUEUE_SIZE = 8;
    static const uint16_t OBSERVER_MAX_PACKET = 255;
    struct ObserverPacket {
        uint32_t timestamp;
        int8_t rssi;
        int8_t snrQ4;
        uint8_t len;
        uint8_t raw[OBSERVER_MAX_PACKET];
    };
    ObserverPacket _observerQueue[OBSERVER_QUEUE_SIZE];
    uint8_t _observerQueueHead = 0;
    uint8_t _observerQueueCount = 0;
    portMUX_TYPE _observerQueueMux = portMUX_INITIALIZER_UNLOCKED;
    // True while the one-shot connect task owns _mqtt/_wc. The loop thread must
    // not touch either until it clears (PubSubClient is not thread-safe).
    volatile bool _connecting = false;
    mutable portMUX_TYPE _statusMux = portMUX_INITIALIZER_UNLOCKED;
    MqttBridgeStatus _status;

    static const uint32_t RECONNECT_INTERVAL_MS = 15000;

    void loadConfig();             // shared by begin() / reloadConfig()
    void deriveKey();              // _key = SHA-256(_psk); sets _encOn
    void setStatusPhase(MqttBridgePhase phase, int result, uint32_t now,
                        bool recordResult, uint32_t retryAt = 0);
    void syncStatusConfig(MqttBridgePhase phase);
    bool reconnect();
    static void reconnectTask(void* arg);   // one-shot task body wrapping reconnect()
    void pub(const char* subtopic, const char* json);  // seals if _encOn
    void publishObserverStatus(bool online);
    void publishObserverPacket(const ObserverPacket& observed);
    bool publishObserverPayload(const char* topic, const char* json, bool retain);
    bool popObserverPacket(ObserverPacket& observed);
    void observerTopic(char* out, size_t outCap, const char* type) const;
    void observerCloudLoop(uint32_t now);
    void observerCloudStop();
    bool observerCloudStart();
    bool createObserverJwt(char* token, size_t tokenCap) const;
    static void observerCloudEvent(void* arg, const char* base, int32_t eventId, void* eventData);
    bool sealToB64(const char* plain, char* out, size_t outCap);
    static void escapeJson(const char* src, char* dst, size_t dstLen);
};
#endif // HAS_TANMATSU (no-op stub) vs real MqttBridge

extern MqttBridge mqtt_bridge;

#endif // ESP32 && MULTI_TRANSPORT_COMPANION
