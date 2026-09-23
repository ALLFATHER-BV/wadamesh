#if defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)
#include "MqttBridge.h"

// The single global bridge instance (extern in the header). Defined for every board: the header
// makes MqttBridge a no-op stub on the Tanmatsu, and the real bridge below on all other boards.
MqttBridge mqtt_bridge;

namespace {
enum class ObserverAuth : uint8_t { None, UserPass, Jwt };
enum class ObserverTopicStyle : uint8_t { MeshCore, MeshRank };

struct ObserverPreset {
    const char* label;
    const char* name;
    const char* uri;
    const char* host;
    uint16_t port;
    const char* audience;
    ObserverAuth auth;
    ObserverTopicStyle topicStyle;
    uint32_t tokenLifetime;
    uint16_t keepalive;
    bool allowRetain;
    bool requiresIata;
    const char* username;
    const char* password;
};

constexpr const char* OBSERVER_PUBKEY_USERNAME = "{pubkey}";
constexpr ObserverPreset OBSERVER_PRESETS[] = {
    {"Custom broker", "custom", nullptr, nullptr, 0, nullptr, ObserverAuth::UserPass, ObserverTopicStyle::MeshCore, 0, 60, true, false, nullptr, nullptr},
    {"NebraskaMesh", "nebraskamesh", "wss://mqtt.nebraskamesh.net:443/mqtt", "mqtt.nebraskamesh.net", 443, "mqtt.nebraskamesh.net", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"MichMesh (Analyzer US)", "michmesh", "wss://mqtt-us-v1.letsmesh.net:443/mqtt", "mqtt-us-v1.letsmesh.net", 443, "mqtt-us-v1.letsmesh.net", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"Analyzer US", "analyzer-us", "wss://mqtt-us-v1.letsmesh.net:443/mqtt", "mqtt-us-v1.letsmesh.net", 443, "mqtt-us-v1.letsmesh.net", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"Analyzer EU", "analyzer-eu", "wss://mqtt-eu-v1.letsmesh.net:443/mqtt", "mqtt-eu-v1.letsmesh.net", 443, "mqtt-eu-v1.letsmesh.net", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"NZ Analyzer", "nz-analyzer", "wss://meshcore-mqtt-1.baird.io:443", "meshcore-mqtt-1.baird.io", 443, "meshcore-mqtt-1.baird.io", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"MeshMapper", "meshmapper", "wss://mqtt.meshmapper.net:443/mqtt", "mqtt.meshmapper.net", 443, "mqtt.meshmapper.net", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"MeshRank", "meshrank", "mqtts://meshrank.net:8883", "meshrank.net", 8883, nullptr, ObserverAuth::None, ObserverTopicStyle::MeshRank, 0, 60, false, false, nullptr, nullptr},
    {"WAEV", "waev", "wss://mqtt.waev.app:443/mqtt", "mqtt.waev.app", 443, "mqtt.waev.app", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 3300, 55, false, true, nullptr, nullptr},
    {"Meshomatic", "meshomatic", "wss://us-east.meshomatic.net:443/mqtt", "us-east.meshomatic.net", 443, "us-east.meshomatic.net", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"CascadiaMesh", "cascadiamesh", "wss://mqtt-v1.cascadiamesh.org:443/mqtt", "mqtt-v1.cascadiamesh.org", 443, "mqtt-v1.cascadiamesh.org", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"TennMesh", "tennmesh", "mqtt://mqtt.tennmesh.com:1883", "mqtt.tennmesh.com", 1883, nullptr, ObserverAuth::UserPass, ObserverTopicStyle::MeshCore, 0, 55, true, true, "mqttfeed", "tc2live"},
    {"NashMesh", "nashmesh", "mqtt://mqtt.nashme.sh:1883", "mqtt.nashme.sh", 1883, nullptr, ObserverAuth::UserPass, ObserverTopicStyle::MeshCore, 0, 55, true, true, "meshdev", "large4cats"},
    {"CTMesh", "ctmesh", "mqtt://mqtt.ctmesh.org:1883", "mqtt.ctmesh.org", 1883, nullptr, ObserverAuth::UserPass, ObserverTopicStyle::MeshCore, 0, 60, true, true, "meshdev", "large4cats"},
    {"ChiMesh", "chimesh", "wss://mqtt.chimesh.org:443", "mqtt.chimesh.org", 443, "mqtt.chimesh.org", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"Meshat.se", "meshat.se", "wss://meshcore-mqtt.meshat.se:443", "meshcore-mqtt.meshat.se", 443, "meshcore-mqtt.meshat.se", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"East Idaho Mesh", "eastidahomesh", "mqtt://live.eastidahomesh.com:1883", "live.eastidahomesh.com", 1883, nullptr, ObserverAuth::None, ObserverTopicStyle::MeshCore, 0, 55, true, true, nullptr, nullptr},
    {"ColoradoMesh", "coloradomesh", "wss://mqtt.meshcore.coloradomesh.org:443", "mqtt.meshcore.coloradomesh.org", 443, "mqtt.meshcore.coloradomesh.org", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"Dutch MeshCore 1", "dutchmeshcore-1", "wss://collector1.dutchmeshcore.nl:443/mqtt", "collector1.dutchmeshcore.nl", 443, "collector1.dutchmeshcore.nl", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"Dutch MeshCore 2", "dutchmeshcore-2", "wss://collector2.dutchmeshcore.nl:443/mqtt", "collector2.dutchmeshcore.nl", 443, "collector2.dutchmeshcore.nl", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"MeshCore Canada 1", "meshcore-ca-1", "wss://mqtt1.meshcore.ca:443/mqtt", "mqtt1.meshcore.ca", 443, "mqtt1.meshcore.ca", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"MeshCore Canada 2", "meshcore-ca-2", "wss://mqtt2.meshcore.ca:443/mqtt", "mqtt2.meshcore.ca", 443, "mqtt2.meshcore.ca", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"MeshCore Finland", "meshcore-fi", "wss://mc-mqtt.meshcore.fi:443/", "mc-mqtt.meshcore.fi", 443, "mc-mqtt.meshcore.fi", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"OkiMesh 1", "okimesh-1", "wss://mqtt1.okimesh.org:9002/mqtt", "mqtt1.okimesh.org", 9002, "mqtt1.okimesh.org", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"OkiMesh 2", "okimesh-2", "wss://mqtt2.okimesh.org:9002/mqtt", "mqtt2.okimesh.org", 9002, "mqtt2.okimesh.org", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"INWMesh", "inwmesh", "mqtts://scope.inwmesh.org:8883", "scope.inwmesh.org", 8883, nullptr, ObserverAuth::UserPass, ObserverTopicStyle::MeshCore, 0, 55, true, true, nullptr, nullptr},
    {"BostonMesh", "bostonmesh", "wss://mqttmc01.bostonme.sh:443/mqtt", "mqttmc01.bostonme.sh", 443, "mqttmc01.bostonme.sh", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"RFLab", "rflab", "wss://mqtt.rflab.io:443", "mqtt.rflab.io", 443, "mqtt.rflab.io", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"IP Network UK", "ipnt.uk", "wss://mqtt.ipnt.uk:443", "mqtt.ipnt.uk", 443, "mqtt.ipnt.uk", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"FLMesh", "flmesh", "wss://mcmqtt.jntconnections.com:443", "mcmqtt.jntconnections.com", 443, "mcmqtt.jntconnections.com", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"CoreComms", "corecomms", "wss://mqtt.corecomms.net:443/mqtt", "mqtt.corecomms.net", 443, "mqtt.corecomms.net", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"MeshTexas", "meshtexas", "wss://mqtt.meshtexas.org:443/mqtt", "mqtt.meshtexas.org", 443, "mqtt.meshtexas.org", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"Mesh Chaun14", "mesh-chaun14", "mqtt://mqtt.mesh.chaun14.fr:1884", "mqtt.mesh.chaun14.fr", 1884, nullptr, ObserverAuth::UserPass, ObserverTopicStyle::MeshCore, 0, 60, true, true, OBSERVER_PUBKEY_USERNAME, nullptr},
    {"WCMesh", "wcmesh", "wss://mqtt.wcmesh.com:443", "mqtt.wcmesh.com", 443, "mqtt.wcmesh.com", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"Atviras Tinklas", "atvirastinklas", "wss://mqtt-mc.atvirastinklas.lt:443", "mqtt-mc.atvirastinklas.lt", 443, "mqtt-mc.atvirastinklas.lt", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"GoMesh", "gomesh", "wss://mqtt.gomesh.dev:443", "mqtt.gomesh.dev", 443, "mqtt.gomesh.dev", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"IdahoMesh", "idahomesh", "wss://mqtt.idahomesh.org:443/mqtt", "mqtt.idahomesh.org", 443, "mqtt.idahomesh.org", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"NTXMesh", "ntxmesh", "wss://ntxmesh.dhovin.me:8883", "ntxmesh.dhovin.me", 8883, "ntxmesh.dhovin.me", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
    {"BSMesh", "bsmesh", "wss://mqtt.bsmesh.de:8885", "mqtt.bsmesh.de", 8885, "mqtt.bsmesh.de", ObserverAuth::Jwt, ObserverTopicStyle::MeshCore, 86400, 55, true, true, nullptr, nullptr},
};

static_assert(sizeof(OBSERVER_PRESETS) / sizeof(OBSERVER_PRESETS[0]) ==
              (size_t)MqttObserverProfile::Count,
              "Observer profile enum and preset table must stay aligned");

const ObserverPreset& observerPreset(MqttObserverProfile profile) {
    const uint8_t index = (uint8_t)profile;
    return OBSERVER_PRESETS[index < (uint8_t)MqttObserverProfile::Count ? index : 0];
}

bool isBuiltInObserverProfile(MqttObserverProfile profile) {
    return profile != MqttObserverProfile::Custom &&
           (uint8_t)profile < (uint8_t)MqttObserverProfile::Count;
}
} // namespace

uint8_t MqttBridge::observerProfileCount() {
    return (uint8_t)MqttObserverProfile::Count;
}

const char* MqttBridge::observerProfileOptions() {
    static char options[768] = {};
    if (!options[0]) {
        size_t pos = 0;
        for (uint8_t i = 0; i < observerProfileCount(); ++i) {
            const char* label = OBSERVER_PRESETS[i].label;
            const int written = snprintf(options + pos, sizeof(options) - pos,
                                         "%s%s", i ? "\n" : "", label);
            if (written <= 0 || (size_t)written >= sizeof(options) - pos) break;
            pos += (size_t)written;
        }
    }
    return options;
}

const char* MqttBridge::observerProfileLabel(MqttObserverProfile profile) {
    return observerPreset(profile).label;
}

MqttObserverProfile MqttBridge::observerProfileFromIndex(uint16_t index) {
    return index < observerProfileCount() ? (MqttObserverProfile)index
                                          : MqttObserverProfile::Custom;
}

uint16_t MqttBridge::observerProfileIndex(MqttObserverProfile profile) {
    const uint8_t index = (uint8_t)profile;
    return index < observerProfileCount() ? index : 0;
}

bool MqttBridge::observerProfileNeedsIata(MqttObserverProfile profile) {
    return observerPreset(profile).requiresIata;
}

bool MqttBridge::observerProfileNeedsToken(MqttObserverProfile profile) {
    return observerPreset(profile).topicStyle == ObserverTopicStyle::MeshRank;
}

bool MqttBridge::observerProfileNeedsUsername(MqttObserverProfile profile) {
    const ObserverPreset& preset = observerPreset(profile);
    return preset.auth == ObserverAuth::UserPass && preset.username == nullptr;
}

bool MqttBridge::observerProfileNeedsPassword(MqttObserverProfile profile) {
    const ObserverPreset& preset = observerPreset(profile);
    return preset.auth == ObserverAuth::UserPass && preset.password == nullptr;
}

#if !defined(HAS_TANMATSU) && !defined(HAS_TDISPLAY_P4)   // ---- real PubSubClient/WiFiClient implementation; NOT built on the P4 boards ----
#include "SdNvsPrefs.h"   // NOT raw NVS Preferences: the touch firmware abandoned NVS (tiny, shared,
                         // doesn't survive Launcher) for a file-backed store. MQTT was the last setting
                         // still on NVS, so its writes silently failed / didn't persist (GH #128).
#include <Identity.h>
#include <Packet.h>
#include <WiFi.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <lwip/sockets.h>
#include "esp_random.h"
#include "mqtt_client.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"

extern "C" esp_err_t esp_crt_bundle_attach(void* conf);

namespace {
constexpr const char* OBSERVER_DEFAULT_TOPIC = "meshcore/{iata}/{device}";
constexpr uint32_t OBSERVER_STATUS_INTERVAL_MS = 300000;
constexpr uint32_t OBSERVER_TOKEN_REFRESH_SECS = 300;

void appendTopicText(char* out, size_t outCap, size_t& pos, const char* text) {
    if (!out || outCap == 0 || !text) return;
    while (*text && pos + 1 < outCap) out[pos++] = *text++;
    out[pos] = '\0';
}

void formatObserverTimestamp(uint32_t epoch, char* out, size_t outCap,
                             char* timeOut = nullptr, size_t timeCap = 0,
                             char* dateOut = nullptr, size_t dateCap = 0) {
    time_t value = epoch ? (time_t)epoch : time(nullptr);
    struct tm utc{};
    if (!gmtime_r(&value, &utc)) {
        snprintf(out, outCap, "1970-01-01T00:00:00.000000+00:00");
        if (timeOut && timeCap) snprintf(timeOut, timeCap, "00:00:00");
        if (dateOut && dateCap) snprintf(dateOut, dateCap, "01/01/1970");
        return;
    }
    char base[24];
    strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc);
    snprintf(out, outCap, "%s.000000+00:00", base);
    if (timeOut && timeCap) strftime(timeOut, timeCap, "%H:%M:%S", &utc);
    if (dateOut && dateCap) strftime(dateOut, dateCap, "%d/%m/%Y", &utc);
}

void bytesToHex(const uint8_t* data, size_t len, char* out, size_t outCap, bool uppercase) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!out || outCap == 0) return;
    out[0] = '\0';
    if (!data || outCap < len * 2 + 1) return;
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

size_t base64UrlEncode(const uint8_t* input, size_t inputLen, char* output, size_t outputCap) {
    if (!input || !output || outputCap == 0) return 0;
    size_t written = 0;
    if (mbedtls_base64_encode((unsigned char*)output, outputCap - 1, &written,
                              input, inputLen) != 0) return 0;
    for (size_t i = 0; i < written; ++i) {
        if (output[i] == '+') output[i] = '-';
        else if (output[i] == '/') output[i] = '_';
    }
    while (written > 0 && output[written - 1] == '=') --written;
    output[written] = '\0';
    return written;
}
} // namespace

// Only hand bytes to lwIP when the socket can accept them right now (zero-timeout
// select). Otherwise WiFiClient::write() blocks in 1 s select() retries against a
// broker that stopped ACKing — on the loop thread that is a visible UI freeze per
// publish. A refused write fails the publish; PubSubClient then flags the
// connection down and the (async) reconnect path takes over.
size_t MqttNbClient::write(const uint8_t* buf, size_t size) {
    int fd_ = fd();
    if (fd_ < 0) return 0;
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd_, &wset);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(fd_ + 1, NULL, &wset, NULL, &tv) <= 0 || !FD_ISSET(fd_, &wset)) return 0;
    return WiFiClient::write(buf, size);
}

void MqttBridge::getStatus(MqttBridgeStatus& out) const {
    portENTER_CRITICAL(&_statusMux);
    out = _status;
    portEXIT_CRITICAL(&_statusMux);
}

void MqttBridge::syncStatusConfig(MqttBridgePhase phase) {
    MqttBridgeStatus next;
    next.phase = phase;
    next.available = true;
    next.requestedEnabled = _requestedEnabled;
    next.publishDm = _plainEnabled && _pubDm;
    next.publishChannel = _plainEnabled && _pubChannel;
    next.publishObserver = _pubObserver;
    next.observerProfile = _observerProfile;
    next.encrypted = _plainEnabled && _encOn;
    if (isBuiltInObserverProfile(_observerProfile)) {
        const ObserverPreset& preset = observerPreset(_observerProfile);
        strncpy(next.host, preset.host, sizeof(next.host) - 1);
        next.port = preset.port;
    } else {
        strncpy(next.host, _host, sizeof(next.host) - 1);
        next.port = _port;
    }
    next.observerPublished = _observerPublished;
    next.observerDropped = _observerDropped;
    next.observerQueued = _observerQueueCount;
    portENTER_CRITICAL(&_statusMux);
    _status = next;
    portEXIT_CRITICAL(&_statusMux);
}

void MqttBridge::setStatusPhase(MqttBridgePhase phase, int result, uint32_t now,
                                bool recordResult, uint32_t retryAt) {
    portENTER_CRITICAL(&_statusMux);
    _status.phase = phase;
    _status.retryAtMs = retryAt;
    if (phase == MqttBridgePhase::Connecting) _status.lastAttemptMs = now;
    if (phase == MqttBridgePhase::Connected)  _status.lastConnectedMs = now;
    if (recordResult) {
        _status.lastResult = (int8_t)result;
        _status.lastResultMs = now;
    }
    portEXIT_CRITICAL(&_statusMux);
}

// Read every field from the "mqtt" NVS namespace into the members. isKey() guards
// avoid the [E] NOT_FOUND log spam that getString emits for absent keys on the
// USB-CDC companion stream (see TouchPrefsStore prefsGetStr note).
void MqttBridge::loadConfig() {
    _requestedEnabled = false; _enabled = false; _pubDm = false; _pubChannel = true;
    _pubObserver = false; _plainEnabled = false;
    _observerProfile = MqttObserverProfile::Custom;
    _host[0] = _user[0] = _pwd[0] = _psk[0] = _observerOrigin[0] = _observerIata[0] =
        _observerToken[0] = '\0';
    snprintf(_observerTopic, sizeof(_observerTopic), "%s", OBSERVER_DEFAULT_TOPIC);
    _port = 1883;

    SdNvsPrefs p;
    if (p.begin("mqtt", true)) {
        _requestedEnabled = p.getBool("en", false);
        _port       = (uint16_t)p.getUInt("port", 1883);
        _pubDm      = p.getBool("dm", false);
        _pubChannel = p.getBool("ch", true);
        _pubObserver = p.getBool("obs", false);
        const uint32_t profile = p.getUInt("obs_profile", 0);
        if (profile < (uint32_t)MqttObserverProfile::Count)
            _observerProfile = (MqttObserverProfile)profile;
        if (p.isKey("host")) p.getString("host", _host, sizeof(_host));
        if (p.isKey("user")) p.getString("user", _user, sizeof(_user));
        if (p.isKey("pwd"))  p.getString("pwd",  _pwd,  sizeof(_pwd));
        if (p.isKey("psk"))  p.getString("psk",  _psk,  sizeof(_psk));
        if (p.isKey("obs_origin")) p.getString("obs_origin", _observerOrigin, sizeof(_observerOrigin));
        if (p.isKey("obs_iata"))   p.getString("obs_iata", _observerIata, sizeof(_observerIata));
        if (p.isKey("obs_topic"))  p.getString("obs_topic", _observerTopic, sizeof(_observerTopic));
        if (p.isKey("obs_token"))  p.getString("obs_token", _observerToken, sizeof(_observerToken));
        p.end();
    }
    if (_observerOrigin[0] == '\0') snprintf(_observerOrigin, sizeof(_observerOrigin), "%s", _nodeName);
    if (_observerTopic[0] == '\0') snprintf(_observerTopic, sizeof(_observerTopic), "%s", OBSERVER_DEFAULT_TOPIC);
    deriveKey();
    _plainEnabled = _requestedEnabled && _observerProfile == MqttObserverProfile::Custom && _host[0] != '\0';
    const ObserverPreset& preset = observerPreset(_observerProfile);
    const bool hasLocation = !preset.requiresIata || _observerIata[0] != '\0';
    bool hasCredentials = true;
    if (preset.topicStyle == ObserverTopicStyle::MeshRank) {
        hasCredentials = _observerToken[0] != '\0';
    } else if (preset.auth == ObserverAuth::UserPass) {
        const bool hasUsername = preset.username != nullptr || _user[0] != '\0';
        const bool hasPassword = preset.password != nullptr || _pwd[0] != '\0';
        hasCredentials = hasUsername && hasPassword;
    }
    const bool cloudEnabled = _requestedEnabled && _pubObserver && hasLocation && hasCredentials &&
                              isBuiltInObserverProfile(_observerProfile);
    _enabled = _plainEnabled || cloudEnabled;
    MqttBridgePhase phase = MqttBridgePhase::Disabled;
    if (_requestedEnabled && !_enabled) phase = MqttBridgePhase::Misconfigured;
    else if (_enabled) phase = WiFi.status() == WL_CONNECTED
                                ? MqttBridgePhase::RetryWait
                                : MqttBridgePhase::WaitingForWifi;
    syncStatusConfig(phase);
}

// Derive the AES key from the passphrase. mbedtls_md() (generic digest) is used
// rather than mbedtls_sha256(), whose _ret suffix differs across mbedTLS 2.x/3.x.
void MqttBridge::deriveKey() {
    _encOn = (_psk[0] != '\0');
    if (!_encOn) { memset(_key, 0, sizeof(_key)); return; }
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md(info, (const unsigned char*)_psk, strlen(_psk), _key) != 0) {
        memset(_key, 0, sizeof(_key));
        _encOn = false;   // can't derive → fail closed to plaintext-disabled publishing
    }
}

void MqttBridge::begin(const mesh::LocalIdentity* identity, const char* nodeName,
                       float frequencyMhz, float bandwidthKhz, uint8_t spreadingFactor,
                       uint8_t codingRate, const char* firmwareVersion) {
    _identity = identity;
    if (_identity) {
        bytesToHex(_identity->pub_key, 6, _nodeHex, sizeof(_nodeHex), false);
        bytesToHex(_identity->pub_key, 32, _nodeId, sizeof(_nodeId), true);
    }
    snprintf(_nodeName, sizeof(_nodeName), "%s", nodeName && nodeName[0] ? nodeName : "WADAMESH");
    snprintf(_observerRadio, sizeof(_observerRadio), "%.6f,%.1f,%u,%u",
             (double)frequencyMhz, (double)bandwidthKhz,
             (unsigned)spreadingFactor, (unsigned)codingRate);
    snprintf(_firmwareVersion, sizeof(_firmwareVersion), "%s",
             firmwareVersion && firmwareVersion[0] ? firmwareVersion : "wadamesh");

    loadConfig();
    if (!_enabled) return;

    if (_plainEnabled) {
        _mqtt.setServer(_host, _port);
        _mqtt.setKeepAlive(60);
        _mqtt.setSocketTimeout(2);                  // bound connect/read — a dead broker must not stall loop()
        _mqtt.setBufferSize(_pubObserver ? 1536 : (_encOn ? 1024 : 512));
    }
    Serial.printf("[MQTT] configured -> profile=%s host=%s:%u dm=%d ch=%d obs=%d enc=%d\n",
                                    observerPreset(_observerProfile).name,
                                    isBuiltInObserverProfile(_observerProfile) ? observerPreset(_observerProfile).host : _host,
                                    isBuiltInObserverProfile(_observerProfile) ?
                                        (unsigned)observerPreset(_observerProfile).port : (unsigned)_port,
                  (int)_pubDm, (int)_pubChannel, (int)_pubObserver, (int)_encOn);
}

bool MqttBridge::reconnect() {
    if (!_plainEnabled) return false;
    if (WiFi.status() != WL_CONNECTED) {
        setStatusPhase(MqttBridgePhase::WaitingForWifi, MQTT_DISCONNECTED,
                       millis(), false);
        return false;
    }

    char clientId[80], lwtTopic[192], lwtPayload[512];
    snprintf(clientId, sizeof(clientId), "wadamesh-%s", _nodeHex);
    if (_pubObserver) {
        observerTopic(lwtTopic, sizeof(lwtTopic), "status");
        char timestamp[40], safeOrigin[72];
        formatObserverTimestamp((uint32_t)time(nullptr), timestamp, sizeof(timestamp));
        escapeJson(_observerOrigin, safeOrigin, sizeof(safeOrigin));
        snprintf(lwtPayload, sizeof(lwtPayload),
                 "{\"status\":\"offline\",\"timestamp\":\"%s\",\"origin\":\"%s\","
                 "\"origin_id\":\"%s\",\"firmware_version\":\"%s\","
                 "\"radio\":\"%s\",\"client_version\":\"wadamesh-observer/1\"}",
                 timestamp, safeOrigin, _nodeId, _firmwareVersion, _observerRadio);
    } else {
        snprintf(lwtTopic, sizeof(lwtTopic), "wadamesh/%s/status", _nodeHex);
        snprintf(lwtPayload, sizeof(lwtPayload), "offline");
    }

    bool ok = _user[0]
        ? _mqtt.connect(clientId, _user, _pwd, lwtTopic, 0, true, lwtPayload)
        : _mqtt.connect(clientId, nullptr, nullptr, lwtTopic, 0, true, lwtPayload);

    const uint32_t now = millis();
    const int result = _mqtt.state();
    if (ok) {
        setStatusPhase(MqttBridgePhase::Connected, result, now, true);
        if (_pubObserver) publishObserverStatus(true);
        else {
            char legacyStatusTopic[80];
            snprintf(legacyStatusTopic, sizeof(legacyStatusTopic), "wadamesh/%s/status", _nodeHex);
            _mqtt.publish(legacyStatusTopic, "online", true);
        }
        Serial.printf("[MQTT] connected as %s\n", clientId);
    } else {
        MqttBridgeStatus current;
        getStatus(current);
        const uint32_t retryAt = current.lastAttemptMs
                                   ? current.lastAttemptMs + RECONNECT_INTERVAL_MS
                                   : now + RECONNECT_INTERVAL_MS;
        setStatusPhase(MqttBridgePhase::RetryWait, result, now, true,
                       retryAt);
        Serial.printf("[MQTT] connect failed, rc=%d\n", result);
    }
    return ok;
}

// reconnect() blocks on DNS + TCP connect + CONNACK (2-10 s against an unreachable
// broker) — never run it on the loop thread, it drives LVGL. One-shot task instead;
// _connecting hands _mqtt/_wc to the task and back.
void MqttBridge::reconnectTask(void* arg) {
    MqttBridge* self = (MqttBridge*)arg;
    self->reconnect();
    self->_connecting = false;
    vTaskDelete(nullptr);
}

bool MqttBridge::createObserverJwt(char* token, size_t tokenCap) const {
    if (!_identity || !token || tokenCap == 0) return false;
    const ObserverPreset& preset = observerPreset(_observerProfile);
    if (preset.auth != ObserverAuth::Jwt || !preset.audience) return false;
    const uint32_t now = (uint32_t)time(nullptr);
    if (now < 1700000000UL) return false;

    static const char headerJson[] = "{\"alg\":\"Ed25519\",\"typ\":\"JWT\"}";
    char payloadJson[256];
    const int payloadLen = snprintf(payloadJson, sizeof(payloadJson),
        "{\"publicKey\":\"%s\",\"aud\":\"%s\",\"iat\":%lu,\"exp\":%lu,"
        "\"client\":\"wadamesh-observer/1\"}",
        _nodeId, preset.audience, (unsigned long)now,
        (unsigned long)(now + preset.tokenLifetime));
    if (payloadLen <= 0 || (size_t)payloadLen >= sizeof(payloadJson)) return false;

    char header[64], payload[384], signingInput[512];
    const size_t headerLen = base64UrlEncode((const uint8_t*)headerJson,
                                             strlen(headerJson), header, sizeof(header));
    const size_t encodedPayloadLen = base64UrlEncode((const uint8_t*)payloadJson,
                                                      (size_t)payloadLen, payload, sizeof(payload));
    if (headerLen == 0 || encodedPayloadLen == 0) return false;
    const int signingLen = snprintf(signingInput, sizeof(signingInput), "%s.%s", header, payload);
    if (signingLen <= 0 || (size_t)signingLen >= sizeof(signingInput)) return false;

    uint8_t signature[SIGNATURE_SIZE];
    _identity->sign(signature, (const uint8_t*)signingInput, signingLen);
    char signatureHex[SIGNATURE_SIZE * 2 + 1];
    bytesToHex(signature, sizeof(signature), signatureHex, sizeof(signatureHex), true);
    const int tokenLen = snprintf(token, tokenCap, "%s.%s", signingInput, signatureHex);
    return tokenLen > 0 && (size_t)tokenLen < tokenCap;
}

void MqttBridge::observerCloudEvent(void* arg, const char* base, int32_t eventId, void* eventData) {
    (void)base;
    MqttBridge* self = static_cast<MqttBridge*>(arg);
    if (!self) return;
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(eventData);
    const uint32_t now = millis();
    switch ((esp_mqtt_event_id_t)eventId) {
        case MQTT_EVENT_CONNECTED:
            self->_observerCloudConnected = true;
            self->_observerCloudRetryAtMs = 0;
            self->setStatusPhase(MqttBridgePhase::Connected, 0, now, true);
            portENTER_CRITICAL(&self->_statusMux);
            self->_status.lastTlsError = 0;
            self->_status.lastTlsStackError = 0;
            self->_status.lastSocketError = 0;
            portEXIT_CRITICAL(&self->_statusMux);
            self->publishObserverStatus(true);
            break;
        case MQTT_EVENT_DISCONNECTED:
            self->_observerCloudConnected = false;
            self->setStatusPhase(MqttBridgePhase::RetryWait, -1, now, false,
                                 now + RECONNECT_INTERVAL_MS);
            break;
        case MQTT_EVENT_ERROR: {
            self->_observerCloudConnected = false;
            int result = -2;
            int32_t tlsError = 0, tlsStackError = 0, socketError = 0;
            if (event && event->error_handle &&
                event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                result = (int)event->error_handle->connect_return_code;
            }
            if (event && event->error_handle) {
                tlsError = event->error_handle->esp_tls_last_esp_err;
                tlsStackError = event->error_handle->esp_tls_stack_err;
                socketError = event->error_handle->esp_transport_sock_errno;
            }
            self->setStatusPhase(MqttBridgePhase::RetryWait, result, now, true,
                                 now + RECONNECT_INTERVAL_MS);
            portENTER_CRITICAL(&self->_statusMux);
            self->_status.lastTlsError = tlsError;
            self->_status.lastTlsStackError = tlsStackError;
            self->_status.lastSocketError = socketError;
            portEXIT_CRITICAL(&self->_statusMux);
            break;
        }
        default:
            break;
    }
}

bool MqttBridge::observerCloudStart() {
    if (_observerCloudClient || !_identity) return _observerCloudClient != nullptr;
    const ObserverPreset& preset = observerPreset(_observerProfile);
    const uint32_t epoch = (uint32_t)time(nullptr);
    if (preset.auth == ObserverAuth::Jwt && epoch < 1700000000UL) return false;

    static char token[768], username[72], clientId[80], statusTopic[192], offlinePayload[512];
    const char* connectUsername = nullptr;
    const char* connectPassword = nullptr;
    if (preset.auth == ObserverAuth::Jwt) {
        if (!createObserverJwt(token, sizeof(token))) return false;
        snprintf(username, sizeof(username), "v1_%s", _nodeId);
        connectUsername = username;
        connectPassword = token;
    } else if (preset.auth == ObserverAuth::UserPass) {
        connectUsername = preset.username == OBSERVER_PUBKEY_USERNAME
                            ? _nodeId : (preset.username ? preset.username : _user);
        connectPassword = preset.password ? preset.password : _pwd;
        if (!connectUsername[0] || !connectPassword[0]) return false;
    }
    snprintf(clientId, sizeof(clientId), "mqtt_%s-%.6s", preset.name, _nodeId);
    observerTopic(statusTopic, sizeof(statusTopic), "status");
    char timestamp[40], safeOrigin[72];
    formatObserverTimestamp(epoch, timestamp, sizeof(timestamp));
    escapeJson(_observerOrigin, safeOrigin, sizeof(safeOrigin));
    snprintf(offlinePayload, sizeof(offlinePayload),
             "{\"status\":\"offline\",\"timestamp\":\"%s\",\"origin\":\"%s\","
             "\"origin_id\":\"%s\",\"firmware_version\":\"%s\","
             "\"radio\":\"%s\",\"client_version\":\"wadamesh-observer/1\"}",
             timestamp, safeOrigin, _nodeId, _firmwareVersion, _observerRadio);

    esp_mqtt_client_config_t config{};
    config.uri = preset.uri;
    config.client_id = clientId;
    config.username = connectUsername;
    config.password = connectPassword;
    config.lwt_topic = statusTopic;
    config.lwt_msg = offlinePayload;
    config.lwt_qos = 1;
    config.lwt_retain = preset.allowRetain ? 1 : 0;
    config.keepalive = preset.keepalive ? preset.keepalive : 60;
    config.buffer_size = 1536;
    config.out_buffer_size = 1536;
    config.network_timeout_ms = 10000;
    config.reconnect_timeout_ms = RECONNECT_INTERVAL_MS;
    if (strncmp(preset.uri, "wss://", 6) == 0 || strncmp(preset.uri, "mqtts://", 8) == 0)
        config.crt_bundle_attach = esp_crt_bundle_attach;
    config.user_context = this;

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&config);
    if (!client) return false;
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, observerCloudEvent, this);
    _observerCloudClient = client;
    _observerCloudTokenExpires = preset.auth == ObserverAuth::Jwt
                                   ? epoch + preset.tokenLifetime : 0;
    setStatusPhase(MqttBridgePhase::Connecting, -1, millis(), false);
    if (esp_mqtt_client_start(client) != ESP_OK) {
        esp_mqtt_client_destroy(client);
        _observerCloudClient = nullptr;
        _observerCloudTokenExpires = 0;
        return false;
    }
    return true;
}

void MqttBridge::observerCloudStop() {
    if (!_observerCloudClient) return;
    esp_mqtt_client_handle_t client = static_cast<esp_mqtt_client_handle_t>(_observerCloudClient);
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    _observerCloudClient = nullptr;
    _observerCloudConnected = false;
    _observerCloudTokenExpires = 0;
}

void MqttBridge::observerCloudLoop(uint32_t now) {
    const ObserverPreset& preset = observerPreset(_observerProfile);
    if (WiFi.status() != WL_CONNECTED) {
        if (_observerCloudClient) observerCloudStop();
        setStatusPhase(MqttBridgePhase::WaitingForWifi, -1, now, false);
        return;
    }
    const uint32_t epoch = (uint32_t)time(nullptr);
    if (preset.auth == ObserverAuth::Jwt && epoch < 1700000000UL) {
        if (_observerCloudClient) observerCloudStop();
        setStatusPhase(MqttBridgePhase::WaitingForTime, -1, now, false);
        return;
    }
    if (preset.auth == ObserverAuth::Jwt && _observerCloudClient &&
        _observerCloudTokenExpires > 0 &&
        epoch + OBSERVER_TOKEN_REFRESH_SECS >= _observerCloudTokenExpires) {
        observerCloudStop();
    }
    if (!_observerCloudClient) {
        if (_observerCloudRetryAtMs && (int32_t)(now - _observerCloudRetryAtMs) < 0) return;
        if (!observerCloudStart()) {
            _observerCloudRetryAtMs = now + RECONNECT_INTERVAL_MS;
            setStatusPhase(MqttBridgePhase::RetryWait, -2, now, true,
                           _observerCloudRetryAtMs);
            return;
        }
    }
    if (!_observerCloudConnected) return;
    if (_lastObserverStatusMs == 0 ||
        (uint32_t)(now - _lastObserverStatusMs) >= OBSERVER_STATUS_INTERVAL_MS) {
        publishObserverStatus(true);
    }
    ObserverPacket observed;
    if (popObserverPacket(observed)) publishObserverPacket(observed);
}

void MqttBridge::loop() {
    if (!_enabled) return;
    uint32_t now = millis();
    if (isBuiltInObserverProfile(_observerProfile)) {
        observerCloudLoop(now);
        return;
    }
    if (!_plainEnabled || _connecting) return;
    if (WiFi.status() != WL_CONNECTED) {
        if (_mqtt.connected()) _mqtt.loop();
        setStatusPhase(MqttBridgePhase::WaitingForWifi, MQTT_DISCONNECTED,
                       now, false);
        return;
    }
    if (!_mqtt.connected()) {
        MqttBridgeStatus current;
        getStatus(current);
        if (current.phase == MqttBridgePhase::Connected) {
            setStatusPhase(MqttBridgePhase::RetryWait, _mqtt.state(), now, true,
                           now);
        }
        if ((uint32_t)(now - _lastReconnectMs) >= RECONNECT_INTERVAL_MS) {
            _lastReconnectMs = now;
            _connecting = true;
            setStatusPhase(MqttBridgePhase::Connecting, MQTT_DISCONNECTED,
                           now, false);
            if (xTaskCreatePinnedToCore(reconnectTask, "mqtt_conn", 6144, this,
                                        1, nullptr, 0) != pdPASS) {
                _connecting = false;   // task OOM — try again next interval
                setStatusPhase(MqttBridgePhase::RetryWait, MQTT_DISCONNECTED,
                               now, false, now + RECONNECT_INTERVAL_MS);
            }
        } else {
            setStatusPhase(MqttBridgePhase::RetryWait, MQTT_DISCONNECTED,
                           now, false, _lastReconnectMs + RECONNECT_INTERVAL_MS);
        }
        return;
    }
    if (!_mqtt.loop()) {
        setStatusPhase(MqttBridgePhase::RetryWait, _mqtt.state(), now, true,
                       now);
        return;
    }
    if (_pubObserver) {
        if (_lastObserverStatusMs == 0 ||
            (uint32_t)(now - _lastObserverStatusMs) >= OBSERVER_STATUS_INTERVAL_MS) {
            publishObserverStatus(true);
        }
        ObserverPacket observed;
        if (popObserverPacket(observed)) publishObserverPacket(observed);
    }
}

// AES-256-GCM seal: out = base64( nonce[12] || ciphertext || tag[16] ). Single-task
// use (mesh callbacks + loop() run on the same Arduino task), so the stack buffers
// are safe. Returns false on any crypto/size failure (caller then publishes nothing).
bool MqttBridge::sealToB64(const char* plain, char* out, size_t outCap) {
    size_t plen = strlen(plain);
    if (plen > 480) return false;                 // matches the json[] producers below

    uint8_t blob[12 + 480 + 16];
    esp_fill_random(blob, 12);                    // fresh nonce per message
    uint8_t tag[16];

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, _key, 256);
    if (rc == 0) {
        rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plen,
                                       blob, 12, nullptr, 0,
                                       (const unsigned char*)plain, blob + 12,
                                       16, tag);
    }
    mbedtls_gcm_free(&gcm);
    if (rc != 0) return false;

    memcpy(blob + 12 + plen, tag, 16);
    size_t blobLen = 12 + plen + 16, olen = 0;
    return mbedtls_base64_encode((unsigned char*)out, outCap, &olen, blob, blobLen) == 0;
}

void MqttBridge::pub(const char* subtopic, const char* json) {
    if (!_plainEnabled || _connecting || !_mqtt.connected()) return;
    char topic[80];
    snprintf(topic, sizeof(topic), "wadamesh/%s/%s", _nodeHex, subtopic);
    if (_encOn) {
        char b64[720];                            // base64 of the ~508-byte sealed blob
        if (sealToB64(json, b64, sizeof(b64))) _mqtt.publish(topic, b64);
    } else {
        _mqtt.publish(topic, json);
    }
}

void MqttBridge::observerTopic(char* out, size_t outCap, const char* type) const {
    if (!out || outCap == 0) return;
    out[0] = '\0';
    const ObserverPreset& preset = observerPreset(_observerProfile);
    if (preset.topicStyle == ObserverTopicStyle::MeshRank) {
        snprintf(out, outCap, "meshrank/uplink/%s/%s/%s",
                 _observerToken, _nodeId, type);
        return;
    }
    const char* pattern = (isBuiltInObserverProfile(_observerProfile) || !_observerTopic[0])
                            ? OBSERVER_DEFAULT_TOPIC : _observerTopic;
    const bool hasType = strstr(pattern, "{type}") != nullptr;
    size_t pos = 0;
    while (*pattern && pos + 1 < outCap) {
        if (strncmp(pattern, "{iata}", 6) == 0) {
            appendTopicText(out, outCap, pos, _observerIata[0] ? _observerIata : "UNSET");
            pattern += 6;
        } else if (strncmp(pattern, "{device}", 8) == 0) {
            appendTopicText(out, outCap, pos, _nodeId);
            pattern += 8;
        } else if (strncmp(pattern, "{type}", 6) == 0) {
            appendTopicText(out, outCap, pos, type);
            pattern += 6;
        } else {
            out[pos++] = *pattern++;
            out[pos] = '\0';
        }
    }
    if (!hasType) {
        if (pos > 0 && out[pos - 1] != '/') appendTopicText(out, outCap, pos, "/");
        appendTopicText(out, outCap, pos, type);
    }
}

void MqttBridge::observeRx(float snr, float rssi, const uint8_t* raw, int len) {
    if (!_requestedEnabled || !_pubObserver || !raw || len <= 0 || len > OBSERVER_MAX_PACKET) return;

    portENTER_CRITICAL(&_observerQueueMux);
    ObserverPacket& observed = _observerQueue[_observerQueueHead];
    observed.timestamp = (uint32_t)time(nullptr);
    observed.rssi = (int8_t)rssi;
    observed.snrQ4 = (int8_t)(snr * 4.0f);
    observed.len = (uint8_t)len;
    memcpy(observed.raw, raw, (size_t)len);
    _observerQueueHead = (uint8_t)((_observerQueueHead + 1) % OBSERVER_QUEUE_SIZE);
    if (_observerQueueCount < OBSERVER_QUEUE_SIZE) {
        ++_observerQueueCount;
    } else {
        ++_observerDropped; // full queue: overwrite the oldest observation
    }
    const uint8_t queued = _observerQueueCount;
    const uint32_t dropped = _observerDropped;
    portEXIT_CRITICAL(&_observerQueueMux);

    portENTER_CRITICAL(&_statusMux);
    _status.observerQueued = queued;
    _status.observerDropped = dropped;
    portEXIT_CRITICAL(&_statusMux);
}

bool MqttBridge::popObserverPacket(ObserverPacket& observed) {
    portENTER_CRITICAL(&_observerQueueMux);
    if (_observerQueueCount == 0) {
        portEXIT_CRITICAL(&_observerQueueMux);
        return false;
    }
    const uint8_t tail = (uint8_t)((_observerQueueHead + OBSERVER_QUEUE_SIZE -
                                    _observerQueueCount) % OBSERVER_QUEUE_SIZE);
    observed = _observerQueue[tail];
    --_observerQueueCount;
    const uint8_t queued = _observerQueueCount;
    portEXIT_CRITICAL(&_observerQueueMux);

    portENTER_CRITICAL(&_statusMux);
    _status.observerQueued = queued;
    portEXIT_CRITICAL(&_statusMux);
    return true;
}

void MqttBridge::publishObserverStatus(bool online) {
    if (!_pubObserver) return;
    char topic[192], timestamp[40], safeOrigin[72];
    observerTopic(topic, sizeof(topic), "status");
    formatObserverTimestamp((uint32_t)time(nullptr), timestamp, sizeof(timestamp));
    escapeJson(_observerOrigin, safeOrigin, sizeof(safeOrigin));

    char json[640];
    snprintf(json, sizeof(json),
             "{\"status\":\"%s\",\"timestamp\":\"%s\",\"origin\":\"%s\","
             "\"origin_id\":\"%s\",\"model\":\"WADAMESH\","
             "\"firmware_version\":\"%s\",\"radio\":\"%s\","
             "\"client_version\":\"wadamesh-observer/1\","
             "\"location\":\"%s\","
             "\"stats\":{\"uptime_secs\":%lu,\"queue_len\":%u,"
             "\"packets_received\":%lu,\"observer_dropped\":%lu}}",
             online ? "online" : "offline", timestamp, safeOrigin, _nodeId,
             _firmwareVersion, _observerRadio,
             _observerIata[0] ? _observerIata : "UNSET",
             (unsigned long)(millis() / 1000u), (unsigned)_observerQueueCount,
             (unsigned long)(_observerPublished + _observerDropped + _observerQueueCount),
             (unsigned long)_observerDropped);
    publishObserverPayload(topic, json, true);
    _lastObserverStatusMs = millis();
}

bool MqttBridge::publishObserverPayload(const char* topic, const char* json, bool retain) {
    if (!topic || !json) return false;
    if (isBuiltInObserverProfile(_observerProfile)) {
        if (!_observerCloudConnected || !_observerCloudClient) return false;
        const ObserverPreset& preset = observerPreset(_observerProfile);
        const bool effectiveRetain = retain && preset.allowRetain;
        esp_mqtt_client_handle_t client = static_cast<esp_mqtt_client_handle_t>(_observerCloudClient);
        if (esp_mqtt_client_get_outbox_size(client) > 16384) return false;
        return esp_mqtt_client_enqueue(client, topic, json, 0,
                                       effectiveRetain ? 1 : 0,
                                       effectiveRetain ? 1 : 0, true) >= 0;
    }
    return _plainEnabled && !_connecting && _mqtt.connected() &&
           _mqtt.publish(topic, json, retain);
}

void MqttBridge::publishObserverPacket(const ObserverPacket& observed) {
    static mesh::Packet packet;
    static char rawHex[OBSERVER_MAX_PACKET * 2 + 1];
    static char pathJson[520];
    static char json[1280];

    if (!packet.readFrom(observed.raw, observed.len)) {
        ++_observerDropped;
        return;
    }

    bytesToHex(observed.raw, observed.len, rawHex, sizeof(rawHex), true);
    uint8_t hash[MAX_HASH_SIZE];
    char hashHex[MAX_HASH_SIZE * 2 + 1];
    packet.calculatePacketHash(hash);
    bytesToHex(hash, sizeof(hash), hashHex, sizeof(hashHex), true);

    pathJson[0] = '\0';
    if (packet.isRouteDirect() && packet.getPathHashCount() > 0) {
        size_t pos = 0;
        pos += snprintf(pathJson + pos, sizeof(pathJson) - pos, ",\"path\":[");
        const uint8_t hashSize = packet.getPathHashSize();
        const uint8_t hashCount = packet.getPathHashCount();
        for (uint8_t hop = 0; hop < hashCount && pos + hashSize * 2 + 4 < sizeof(pathJson); ++hop) {
            if (hop) pathJson[pos++] = ',';
            pathJson[pos++] = '\"';
            char hopHex[7];
            bytesToHex(packet.path + hop * hashSize, hashSize, hopHex, sizeof(hopHex), false);
            pos += snprintf(pathJson + pos, sizeof(pathJson) - pos, "%s\"", hopHex);
        }
        if (pos + 2 < sizeof(pathJson)) {
            pathJson[pos++] = ']';
            pathJson[pos] = '\0';
        }
    }

    char timestamp[40], timeText[16], dateText[16], safeOrigin[72];
    formatObserverTimestamp(observed.timestamp, timestamp, sizeof(timestamp),
                            timeText, sizeof(timeText), dateText, sizeof(dateText));
    escapeJson(_observerOrigin, safeOrigin, sizeof(safeOrigin));
    const int written = snprintf(
        json, sizeof(json),
        "{\"origin\":\"%s\",\"origin_id\":\"%s\",\"timestamp\":\"%s\","
        "\"type\":\"PACKET\",\"direction\":\"rx\",\"time\":\"%s\",\"date\":\"%s\","
        "\"len\":\"%u\",\"packet_type\":\"%u\",\"route\":\"%s\","
        "\"payload_len\":\"%u\",\"raw\":\"%s\",\"SNR\":\"%.1f\","
        "\"RSSI\":\"%d\",\"hash\":\"%s\"%s}",
        safeOrigin, _nodeId, timestamp, timeText, dateText,
        (unsigned)observed.len, (unsigned)packet.getPayloadType(),
        packet.isRouteDirect() ? "D" : "F", (unsigned)packet.payload_len,
        rawHex, (double)observed.snrQ4 / 4.0, (int)observed.rssi, hashHex, pathJson);

    char topic[192];
    observerTopic(topic, sizeof(topic), "packets");
    const bool published = written > 0 && (size_t)written < sizeof(json) &&
                           publishObserverPayload(topic, json, false);
    if (published) ++_observerPublished;
    else ++_observerDropped;
    portENTER_CRITICAL(&_statusMux);
    _status.observerPublished = _observerPublished;
    _status.observerDropped = _observerDropped;
    portEXIT_CRITICAL(&_statusMux);
}

void MqttBridge::escapeJson(const char* src, char* dst, size_t dstLen) {
    static const char* hexd = "0123456789abcdef";
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 7 < dstLen; ++i) {   // +7: worst case is \u00XX (6) + NUL
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = (char)c; }
        else if (c == '\n')        { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c == '\r')        { dst[j++] = '\\'; dst[j++] = 'r'; }
        else if (c == '\t')        { dst[j++] = '\\'; dst[j++] = 't'; }
        else if (c < 0x20) {                              // other control chars → \u00XX (valid JSON)
            dst[j++] = '\\'; dst[j++] = 'u'; dst[j++] = '0'; dst[j++] = '0';
            dst[j++] = hexd[(c >> 4) & 0xF]; dst[j++] = hexd[c & 0xF];
        } else { dst[j++] = (char)c; }
    }
    dst[j] = '\0';
}

void MqttBridge::publishDM(const char* senderName, const uint8_t* senderKey32,
                            float snr, uint8_t hops, uint32_t ts, const char* text) {
    if (!_plainEnabled || !_pubDm || _connecting || !_mqtt.connected()) return;   // DMs are opt-in
    char keyHex[13] = {};
    for (int i = 0; i < 6; ++i) snprintf(keyHex + i * 2, 3, "%02x", senderKey32[i]);

    char safeName[48], safeText[300];
    escapeJson(senderName, safeName, sizeof(safeName));
    escapeJson(text,       safeText, sizeof(safeText));

    char json[480];
    snprintf(json, sizeof(json),
        "{\"sender\":\"%s\",\"key\":\"%s\",\"snr\":%.1f,\"hops\":%u,\"ts\":%lu,\"text\":\"%s\"}",
        safeName, keyHex, snr, (unsigned)hops, (unsigned long)ts, safeText);
    pub("msg/dm", json);
}

void MqttBridge::publishChannel(int channelIdx, const char* channelName,
                                 float snr, uint8_t hops, uint32_t ts, const char* text) {
    if (!_plainEnabled || !_pubChannel || _connecting || !_mqtt.connected()) return;   // channel publish toggle
    char safeName[48], safeText[300];
    escapeJson(channelName, safeName, sizeof(safeName));
    escapeJson(text,        safeText, sizeof(safeText));

    char json[480];
    snprintf(json, sizeof(json),
        "{\"channel\":\"%s\",\"ch_idx\":%d,\"snr\":%.1f,\"hops\":%u,\"ts\":%lu,\"text\":\"%s\"}",
        safeName, channelIdx, snr, (unsigned)hops, (unsigned long)ts, safeText);
    pub("msg/ch", json);
}

void MqttBridge::saveConfig(const char* host, uint16_t port,
                             const char* user, const char* pwd,
                             bool pubDm, bool pubChannel, const char* psk, bool enable) {
    SdNvsPrefs p;
    if (!p.begin("mqtt", false)) return;
    p.putBool("en",     enable);
    p.putString("host", host);
    p.putUInt("port",   port);
    p.putString("user", user);
    p.putString("pwd",  pwd);
    p.putBool("dm",     pubDm);
    p.putBool("ch",     pubChannel);
    p.putString("psk",  psk);
    p.end();
}

void MqttBridge::saveObserverConfig(bool enable, MqttObserverProfile profile, const char* origin,
                                    const char* iata, const char* topicTemplate,
                                    const char* profileToken) {
    char cleanIata[8] = {};
    size_t out = 0;
    for (size_t i = 0; iata && iata[i] && out + 1 < sizeof(cleanIata); ++i) {
        const unsigned char c = (unsigned char)iata[i];
        if (isalnum(c) || c == '-' || c == '_') cleanIata[out++] = (char)toupper(c);
    }

    SdNvsPrefs p;
    if (!p.begin("mqtt", false)) return;
    p.putBool("obs", enable);
    p.putUInt("obs_profile", (uint32_t)profile);
    p.putString("obs_origin", origin ? origin : "");
    p.putString("obs_iata", cleanIata);
    p.putString("obs_topic", topicTemplate && topicTemplate[0]
                                  ? topicTemplate : OBSERVER_DEFAULT_TOPIC);
    p.putString("obs_token", profileToken ? profileToken : "");
    p.end();
}

void MqttBridge::reloadConfig() {
    // A connect attempt may be in flight on the one-shot task; PubSubClient is not
    // thread-safe, so wait it out (bounded: DNS + TCP + CONNACK <= ~10 s, and it
    // only overlaps when Save lands inside an attempt window on a dead broker).
    while (_connecting) delay(10);
    if (_pubObserver && ((isBuiltInObserverProfile(_observerProfile) && _observerCloudConnected) ||
                         (_observerProfile == MqttObserverProfile::Custom && _mqtt.connected()))) {
        publishObserverStatus(false);
    }
    observerCloudStop();
    if (_mqtt.connected()) _mqtt.disconnect();
    loadConfig();
    portENTER_CRITICAL(&_observerQueueMux);
    _observerDropped += _observerQueueCount;
    _observerQueueHead = _observerQueueCount = 0;
    const uint32_t dropped = _observerDropped;
    portEXIT_CRITICAL(&_observerQueueMux);
    portENTER_CRITICAL(&_statusMux);
    _status.observerQueued = 0;
    _status.observerDropped = dropped;
    portEXIT_CRITICAL(&_statusMux);
    if (!_enabled) { Serial.println("[MQTT] disabled"); return; }
    if (_plainEnabled) {
        _mqtt.setServer(_host, _port);
        _mqtt.setKeepAlive(60);
        _mqtt.setSocketTimeout(2);
        _mqtt.setBufferSize(_pubObserver ? 1536 : (_encOn ? 1024 : 512));
    }
    _lastReconnectMs = 0;   // reconnect on next loop() tick
    _observerCloudRetryAtMs = 0;
    _lastObserverStatusMs = 0;
    Serial.printf("[MQTT] reloaded -> profile=%s host=%s:%u en=%d dm=%d ch=%d obs=%d enc=%d\n",
                                    observerPreset(_observerProfile).name,
                                    isBuiltInObserverProfile(_observerProfile) ? observerPreset(_observerProfile).host : _host,
                                    isBuiltInObserverProfile(_observerProfile) ?
                                        (unsigned)observerPreset(_observerProfile).port : (unsigned)_port,
                  (int)_enabled, (int)_pubDm, (int)_pubChannel, (int)_pubObserver, (int)_encOn);
}

#endif // !HAS_TANMATSU (real implementation)
#endif // ESP32 && MULTI_TRANSPORT_COMPANION
