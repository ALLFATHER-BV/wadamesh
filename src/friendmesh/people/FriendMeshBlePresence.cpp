#include "FriendMeshBlePresence.h"

#include <string.h>

namespace friendmesh {
namespace {
constexpr uint8_t kPresenceCompanyLo = 0xFF;
constexpr uint8_t kPresenceCompanyHi = 0xFF;
constexpr uint8_t kPresenceMagic0 = 'F';
constexpr uint8_t kPresenceMagic1 = 'M';
constexpr uint8_t kPresenceVersion = 1;
}  // namespace

bool encodeBlePresencePayload(const uint8_t* fullPubKey,
                              uint8_t* destination, size_t capacity) {
  if (!fullPubKey || !destination || capacity < kBlePresencePayloadBytes)
    return false;
  destination[0] = kPresenceCompanyLo;
  destination[1] = kPresenceCompanyHi;
  destination[2] = kPresenceMagic0;
  destination[3] = kPresenceMagic1;
  destination[4] = kPresenceVersion;
  memcpy(destination + 5, fullPubKey, kBlePresencePrefixBytes);
  return true;
}

bool decodeBlePresencePayload(const uint8_t* payload, size_t length,
                              uint8_t* pubKeyPrefix) {
  if (!payload || !pubKeyPrefix || length != kBlePresencePayloadBytes ||
      payload[0] != kPresenceCompanyLo ||
      payload[1] != kPresenceCompanyHi ||
      payload[2] != kPresenceMagic0 || payload[3] != kPresenceMagic1 ||
      payload[4] != kPresenceVersion) return false;
  memcpy(pubKeyPrefix, payload + 5, kBlePresencePrefixBytes);
  return true;
}

}  // namespace friendmesh

#if defined(FRIENDMESH_FEATURES) && FRIENDMESH_FEATURES && \
    defined(ESP32) && defined(BLE_PIN_CODE)

#include <Arduino.h>
#include <NimBLEDevice.h>

namespace friendmesh {
namespace {

constexpr char kNordicUartService[] =
    "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr int8_t kMinimumNearbyRssi = -85;

portMUX_TYPE s_presenceMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_localPrefix[kBlePresencePrefixBytes] = {};
char s_shortBleName[16] = {};
bool s_identityReady = false;
bool s_advertisingApplied = false;
volatile bool s_scanActive = false;
volatile bool s_scanFinished = false;
volatile uint32_t s_scanCompletedMillis = 0;
BlePresencePeer s_peers[kBlePresenceMaxPeers] = {};
size_t s_peerCount = 0;

bool parsePresence(const std::string& data, uint8_t* prefix) {
  return decodeBlePresencePayload(
      reinterpret_cast<const uint8_t*>(data.data()), data.size(), prefix);
}

void recordPeer(const uint8_t* prefix, int8_t rssi) {
  if (!prefix || rssi < kMinimumNearbyRssi ||
      memcmp(prefix, s_localPrefix, kBlePresencePrefixBytes) == 0) return;
  portENTER_CRITICAL(&s_presenceMux);
  for (size_t i = 0; i < s_peerCount; ++i) {
    if (memcmp(s_peers[i].pubKeyPrefix, prefix,
               kBlePresencePrefixBytes) == 0) {
      if (rssi > s_peers[i].rssi) s_peers[i].rssi = rssi;
      portEXIT_CRITICAL(&s_presenceMux);
      return;
    }
  }
  if (s_peerCount < kBlePresenceMaxPeers) {
    memcpy(s_peers[s_peerCount].pubKeyPrefix, prefix,
           kBlePresencePrefixBytes);
    s_peers[s_peerCount].rssi = rssi;
    ++s_peerCount;
  }
  portEXIT_CRITICAL(&s_presenceMux);
}

class PresenceScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
 public:
  void onResult(NimBLEAdvertisedDevice* device) override {
    if (!device || !device->haveManufacturerData()) return;
    for (uint8_t i = 0; i < device->getManufacturerDataCount(); ++i) {
      uint8_t prefix[kBlePresencePrefixBytes] = {};
      if (parsePresence(device->getManufacturerData(i), prefix)) {
        recordPeer(prefix, (int8_t)device->getRSSI());
        return;
      }
    }
  }
};

PresenceScanCallbacks s_scanCallbacks;

void scanComplete(NimBLEScanResults) {
  s_scanCompletedMillis = millis();
  s_scanActive = false;
  s_scanFinished = true;
}

}  // namespace

void blePresenceSetLocalIdentity(const uint8_t* pubKey, const char* bleName) {
  if (!pubKey) return;
  memcpy(s_localPrefix, pubKey, sizeof(s_localPrefix));
  if (bleName && bleName[0]) {
    strncpy(s_shortBleName, bleName, sizeof(s_shortBleName) - 1);
  } else {
    strcpy(s_shortBleName, "MeshCore");
  }
  s_shortBleName[sizeof(s_shortBleName) - 1] = '\0';
  s_identityReady = true;
  s_advertisingApplied = false;
}

bool blePresenceApplyAdvertising() {
  if (!s_identityReady || !NimBLEDevice::getInitialized()) return false;
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  if (!advertising) return false;

  const bool restart = advertising->isAdvertising();
  if (restart) advertising->stop();

  // Keep the existing Nordic UART service in the primary packet so companion
  // apps discover WadaMesh exactly as before. Presence data and a bounded short
  // name fit together in the scan response (30 bytes total).
  NimBLEAdvertisementData primary;
  primary.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  primary.setCompleteServices(NimBLEUUID(kNordicUartService));

  uint8_t payload[kBlePresencePayloadBytes] = {};
  if (!encodeBlePresencePayload(s_localPrefix, payload, sizeof(payload)))
    return false;
  NimBLEAdvertisementData response;
  response.setShortName(s_shortBleName);
  response.setManufacturerData(std::string(
      reinterpret_cast<const char*>(payload), sizeof(payload)));

  advertising->setAdvertisementData(primary);
  advertising->setScanResponseData(response);
  advertising->setScanResponse(true);
  s_advertisingApplied = true;
  return !restart || advertising->start();
}

bool blePresenceStartScan(uint32_t durationSeconds) {
  if (!s_identityReady || !s_advertisingApplied || durationSeconds == 0 ||
      !NimBLEDevice::getInitialized()) return false;
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan || scan->isScanning()) return false;

  portENTER_CRITICAL(&s_presenceMux);
  memset(s_peers, 0, sizeof(s_peers));
  s_peerCount = 0;
  portEXIT_CRITICAL(&s_presenceMux);
  s_scanFinished = false;
  s_scanCompletedMillis = 0;
  s_scanActive = true;
  scan->setAdvertisedDeviceCallbacks(&s_scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(45);
  scan->setDuplicateFilter(true);
  scan->setMaxResults(12);
  if (!scan->start(durationSeconds, scanComplete, false)) {
    s_scanActive = false;
    s_scanFinished = true;
    return false;
  }
  return true;
}

void blePresenceCancelScan() {
  if (!NimBLEDevice::getInitialized()) {
    s_scanActive = false;
    return;
  }
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan && scan->isScanning()) scan->stop();
  s_scanActive = false;
}

bool blePresenceIsScanning() { return s_scanActive; }
bool blePresenceScanFinished() { return s_scanFinished; }

size_t blePresenceCopyPeers(BlePresencePeer* destination, size_t capacity) {
  if (!destination || capacity == 0) return 0;
  portENTER_CRITICAL(&s_presenceMux);
  const size_t count = s_peerCount < capacity ? s_peerCount : capacity;
  memcpy(destination, s_peers, count * sizeof(BlePresencePeer));
  portEXIT_CRITICAL(&s_presenceMux);
  return count;
}

bool blePresenceWasSeen(const uint8_t* fullPubKey) {
  if (!fullPubKey || !s_scanFinished || s_scanCompletedMillis == 0 ||
      (uint32_t)(millis() - s_scanCompletedMillis) > 30000) return false;
  bool found = false;
  portENTER_CRITICAL(&s_presenceMux);
  for (size_t i = 0; i < s_peerCount; ++i) {
    if (memcmp(s_peers[i].pubKeyPrefix, fullPubKey,
               kBlePresencePrefixBytes) == 0) {
      found = true;
      break;
    }
  }
  portEXIT_CRITICAL(&s_presenceMux);
  return found;
}

}  // namespace friendmesh

#else

namespace friendmesh {
void blePresenceSetLocalIdentity(const uint8_t*, const char*) {}
bool blePresenceApplyAdvertising() { return false; }
bool blePresenceStartScan(uint32_t) { return false; }
void blePresenceCancelScan() {}
bool blePresenceIsScanning() { return false; }
bool blePresenceScanFinished() { return false; }
size_t blePresenceCopyPeers(BlePresencePeer*, size_t) { return 0; }
bool blePresenceWasSeen(const uint8_t*) { return false; }
}  // namespace friendmesh

#endif
