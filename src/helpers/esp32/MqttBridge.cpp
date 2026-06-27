#if defined(ESP32) && defined(MULTI_TRANSPORT_COMPANION)
#include "MqttBridge.h"
#include <Preferences.h>
#include <WiFi.h>
#include <stdio.h>
#include <string.h>

MqttBridge mqtt_bridge;

void MqttBridge::begin(const char* nodeHex) {
    strncpy(_nodeHex, nodeHex, sizeof(_nodeHex) - 1);
    _nodeHex[sizeof(_nodeHex) - 1] = '\0';

    Preferences p;
    if (p.begin("mqtt", true)) {
        _enabled = p.getBool("en", false);
        p.getString("host", _host, sizeof(_host));
        _port = (uint16_t)p.getUInt("port", 1883);
        p.getString("user", _user, sizeof(_user));
        p.getString("pwd",  _pwd,  sizeof(_pwd));
        p.end();
    }
    if (!_enabled || _host[0] == '\0') {
        _enabled = false;
        return;
    }

    _mqtt.setServer(_host, _port);
    _mqtt.setKeepAlive(60);
    _mqtt.setBufferSize(512);
    Serial.printf("[MQTT] configured → %s:%u\n", _host, _port);
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

void MqttBridge::loop() {
    if (!_enabled) return;
    if (!_mqtt.connected()) {
        uint32_t now = millis();
        if ((uint32_t)(now - _lastReconnectMs) >= RECONNECT_INTERVAL_MS) {
            _lastReconnectMs = now;
            reconnect();
        }
        return;
    }
    _mqtt.loop();
}

void MqttBridge::pub(const char* subtopic, const char* json) {
    if (!_enabled || !_mqtt.connected()) return;
    char topic[80];
    snprintf(topic, sizeof(topic), "wadamesh/%s/%s", _nodeHex, subtopic);
    _mqtt.publish(topic, json);
}

void MqttBridge::escapeJson(const char* src, char* dst, size_t dstLen) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 3 < dstLen; ++i) {
        char c = src[i];
        if (c == '"' || c == '\\') { dst[j++] = '\\'; dst[j++] = c; }
        else if (c == '\n')        { dst[j++] = '\\'; dst[j++] = 'n'; }
        else if (c == '\r')        { dst[j++] = '\\'; dst[j++] = 'r'; }
        else                       { dst[j++] = c; }
    }
    dst[j] = '\0';
}

void MqttBridge::publishDM(const char* senderName, const uint8_t* senderKey32,
                            float snr, uint8_t hops, uint32_t ts, const char* text) {
    if (!_enabled || !_mqtt.connected()) return;
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
    if (!_enabled || !_mqtt.connected()) return;
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
                             const char* user, const char* pwd, bool enable) {
    Preferences p;
    if (!p.begin("mqtt", false)) return;
    p.putBool("en",    enable);
    p.putString("host", host);
    p.putUInt("port",   port);
    p.putString("user", user);
    p.putString("pwd",  pwd);
    p.end();
}

#endif // ESP32 && MULTI_TRANSPORT_COMPANION
