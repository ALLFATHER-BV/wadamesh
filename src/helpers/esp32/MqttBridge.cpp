#if defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)
#include "MqttBridge.h"

// The single global bridge instance (extern in the header). Defined for every board: the header
// makes MqttBridge a no-op stub on the Tanmatsu, and the real bridge below on all other boards.
MqttBridge mqtt_bridge;

#if !defined(HAS_TANMATSU) && !defined(HAS_TDISPLAY_P4)   // ---- real PubSubClient/WiFiClient implementation; NOT built on the P4 boards ----
#include <Preferences.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>
#include <lwip/sockets.h>
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"

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

// Read every field from the "mqtt" NVS namespace into the members. isKey() guards
// avoid the [E] NOT_FOUND log spam that getString emits for absent keys on the
// USB-CDC companion stream (see TouchPrefsStore prefsGetStr note).
void MqttBridge::loadConfig() {
    _enabled = false; _pubDm = false; _pubChannel = true;
    _host[0] = _user[0] = _pwd[0] = _psk[0] = '\0';
    _port = 1883;

    Preferences p;
    if (p.begin("mqtt", true)) {
        _enabled    = p.getBool("en", false);
        _port       = (uint16_t)p.getUInt("port", 1883);
        _pubDm      = p.getBool("dm", false);
        _pubChannel = p.getBool("ch", true);
        if (p.isKey("host")) p.getString("host", _host, sizeof(_host));
        if (p.isKey("user")) p.getString("user", _user, sizeof(_user));
        if (p.isKey("pwd"))  p.getString("pwd",  _pwd,  sizeof(_pwd));
        if (p.isKey("psk"))  p.getString("psk",  _psk,  sizeof(_psk));
        p.end();
    }
    deriveKey();
    if (!_enabled || _host[0] == '\0') _enabled = false;
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

void MqttBridge::begin(const char* nodeHex) {
    strncpy(_nodeHex, nodeHex, sizeof(_nodeHex) - 1);
    _nodeHex[sizeof(_nodeHex) - 1] = '\0';

    loadConfig();
    if (!_enabled) return;

    _mqtt.setServer(_host, _port);
    _mqtt.setKeepAlive(60);
    _mqtt.setSocketTimeout(2);                  // bound connect/read — a dead broker must not stall loop()
    _mqtt.setBufferSize(_encOn ? 1024 : 512);   // sealed base64 needs the bigger buffer
    Serial.printf("[MQTT] configured -> %s:%u dm=%d ch=%d enc=%d\n",
                  _host, _port, (int)_pubDm, (int)_pubChannel, (int)_encOn);
}

bool MqttBridge::reconnect() {
    if (!_enabled || WiFi.status() != WL_CONNECTED) return false;

    char clientId[32], lwtTopic[80];
    snprintf(clientId, sizeof(clientId), "wadamesh-%s", _nodeHex);
    snprintf(lwtTopic, sizeof(lwtTopic), "wadamesh/%s/status", _nodeHex);

    bool ok = _user[0]
        ? _mqtt.connect(clientId, _user, _pwd, lwtTopic, 0, true, "offline")
        : _mqtt.connect(clientId, nullptr, nullptr, lwtTopic, 0, true, "offline");

    if (ok) {
        _mqtt.publish(lwtTopic, "online", true);
        Serial.printf("[MQTT] connected as %s\n", clientId);
    } else {
        Serial.printf("[MQTT] connect failed, rc=%d\n", _mqtt.state());
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

void MqttBridge::loop() {
    if (!_enabled || _connecting) return;
    if (!_mqtt.connected()) {
        uint32_t now = millis();
        if ((uint32_t)(now - _lastReconnectMs) >= RECONNECT_INTERVAL_MS) {
            _lastReconnectMs = now;
            _connecting = true;
            if (xTaskCreatePinnedToCore(reconnectTask, "mqtt_conn", 6144, this,
                                        1, nullptr, 0) != pdPASS) {
                _connecting = false;   // task OOM — try again next interval
            }
        }
        return;
    }
    _mqtt.loop();
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
    if (!_enabled || _connecting || !_mqtt.connected()) return;
    char topic[80];
    snprintf(topic, sizeof(topic), "wadamesh/%s/%s", _nodeHex, subtopic);
    if (_encOn) {
        char b64[720];                            // base64 of the ~508-byte sealed blob
        if (sealToB64(json, b64, sizeof(b64))) _mqtt.publish(topic, b64);
    } else {
        _mqtt.publish(topic, json);
    }
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
    if (!_enabled || !_pubDm || _connecting || !_mqtt.connected()) return;   // DMs are opt-in
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
    if (!_enabled || !_pubChannel || _connecting || !_mqtt.connected()) return;   // channel publish toggle
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
    Preferences p;
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

void MqttBridge::reloadConfig() {
    // A connect attempt may be in flight on the one-shot task; PubSubClient is not
    // thread-safe, so wait it out (bounded: DNS + TCP + CONNACK <= ~10 s, and it
    // only overlaps when Save lands inside an attempt window on a dead broker).
    while (_connecting) delay(10);
    if (_mqtt.connected()) _mqtt.disconnect();
    loadConfig();
    if (!_enabled) { Serial.println("[MQTT] disabled"); return; }
    _mqtt.setServer(_host, _port);
    _mqtt.setKeepAlive(60);
    _mqtt.setSocketTimeout(2);
    _mqtt.setBufferSize(_encOn ? 1024 : 512);
    _lastReconnectMs = 0;   // reconnect on next loop() tick
    Serial.printf("[MQTT] reloaded -> %s:%u en=%d dm=%d ch=%d enc=%d\n",
                  _host, _port, (int)_enabled, (int)_pubDm, (int)_pubChannel, (int)_encOn);
}

#endif // !HAS_TANMATSU (real implementation)
#endif // ESP32 && MULTI_TRANSPORT_COMPANION
