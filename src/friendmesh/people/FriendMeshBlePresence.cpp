#include "FriendMeshBlePresence.h"

#include "FriendMeshFriendRequest.h"

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
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

namespace friendmesh {
namespace {

constexpr char kNordicUartService[] =
    "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char kJoinService[] =
    "7d2b4f10-6c57-4d83-9a49-46524d4a0001";
constexpr char kJoinRequestCharacteristic[] =
    "7d2b4f10-6c57-4d83-9a49-46524d4a0002";
constexpr char kJoinResponseCharacteristic[] =
    "7d2b4f10-6c57-4d83-9a49-46524d4a0003";
constexpr char kFriendRequestCharacteristic[] =
    "7d2b4f10-6c57-4d83-9a49-46524d4a0004";
constexpr char kFriendResponseCharacteristic[] =
    "7d2b4f10-6c57-4d83-9a49-46524d4a0005";
constexpr char kFriendIdentityCharacteristic[] =
    "7d2b4f10-6c57-4d83-9a49-46524d4a0006";
constexpr int8_t kMinimumNearbyRssi = -85;
constexpr uint8_t kJoinPresenceVersion = 2;
constexpr size_t kJoinPresencePayloadBytes = kBlePresencePayloadBytes + 4;
constexpr uint32_t kJoinSessionLifetimeMs = 120000;
constexpr uint8_t kJoinRequestPlainBytes = 64;
constexpr uint8_t kJoinResponsePlainBytes = 112;
constexpr size_t kJoinRequestBytes = 4 + 1 + 4 + 8 + 1 +
                                     kJoinRequestPlainBytes + 16;
constexpr size_t kJoinResponseHeaderBytes = 4 + 1 + 4 + 8 + 2;
constexpr size_t kFriendIdentityBytes = 4 + 1 + 32 + 32;
constexpr size_t kFriendResponseBytes = 4 + 1 + 1 + kFriendRequestIdBytes;

portMUX_TYPE s_presenceMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_localPrefix[kBlePresencePrefixBytes] = {};
uint8_t s_localPubKey[32] = {};
char s_localName[32] = {};
char s_shortBleName[16] = {};
bool s_identityReady = false;
bool s_advertisingApplied = false;
volatile bool s_scanActive = false;
volatile bool s_scanFinished = false;
volatile uint32_t s_scanCompletedMillis = 0;
BlePresencePeer s_peers[kBlePresenceMaxPeers] = {};
size_t s_peerCount = 0;
NimBLEService* s_joinService = nullptr;
NimBLECharacteristic* s_joinResponse = nullptr;
NimBLECharacteristic* s_friendResponse = nullptr;
NimBLECharacteristic* s_friendIdentity = nullptr;
uint8_t s_pendingFriendRequest[kFriendRequestEncodedBytes] = {};
size_t s_pendingFriendRequestLength = 0;
bool s_pendingFriendRequestReady = false;
bool s_joinHostActive = false;
uint32_t s_joinSessionId = 0;
uint32_t s_joinExpiresAt = 0;
char s_joinCode[7] = {};
uint8_t s_joinAdminPub[32] = {};
char s_joinAdminName[32] = {};
char s_joinChannelName[32] = {};
uint8_t s_joinChannelSecret[16] = {};
BleJoinAcceptedMember s_joinAccepted = {};
bool s_joinAcceptedReady = false;
uint8_t s_joinMemberPrefixes[kBlePresenceMaxPeers][kBlePresencePrefixBytes] = {};
size_t s_joinMemberPrefixCount = 0;
uint8_t s_joinFailedAttempts = 0;

static void putU32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

static uint32_t getU32(const uint8_t* in) {
  return static_cast<uint32_t>(in[0]) |
      (static_cast<uint32_t>(in[1]) << 8) |
      (static_cast<uint32_t>(in[2]) << 16) |
      (static_cast<uint32_t>(in[3]) << 24);
}

static bool validCode(const char* code) {
  if (!code) return false;
  for (int i = 0; i < 6; ++i)
    if (code[i] < '0' || code[i] > '9') return false;
  return code[6] == '\0';
}

static bool deriveJoinKey(const char code[7], uint32_t sessionId,
                          const uint8_t nonce[8], uint8_t key[32]) {
  if (!validCode(code) || !nonce || !key) return false;
  uint8_t material[13 + 6 + 4 + 8] = {};
  memcpy(material, "FM-BLE-JOIN-1", 13);
  memcpy(material + 13, code, 6);
  putU32(material + 19, sessionId);
  memcpy(material + 23, nonce, 8);
  const mbedtls_md_info_t* info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  const bool ok = info &&
      mbedtls_md(info, material, sizeof(material), key) == 0;
  memset(material, 0, sizeof(material));
  return ok;
}

static bool cryptJoin(bool encrypt, const uint8_t key[32],
                      uint32_t sessionId, const uint8_t nonce[8],
                      const uint8_t* input, size_t length, uint8_t* output,
                      uint8_t tag[16]) {
  uint8_t iv[12] = {};
  putU32(iv, sessionId);
  memcpy(iv + 4, nonce, 8);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (rc == 0 && encrypt) {
    rc = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, length,
                                   iv, sizeof(iv), nullptr, 0, input, output,
                                   16, tag);
  } else if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&gcm, length, iv, sizeof(iv), nullptr, 0,
                                  tag, 16, input, output);
  }
  mbedtls_gcm_free(&gcm);
  memset(iv, 0, sizeof(iv));
  return rc == 0;
}

static void setJoinFailureResponse(uint32_t sessionId, const uint8_t nonce[8],
                                   uint8_t status) {
  if (!s_joinResponse) return;
  uint8_t response[kJoinResponseHeaderBytes] = {'F','M','B','R',1};
  putU32(response + 5, sessionId);
  memcpy(response + 9, nonce, 8);
  response[17] = status;
  response[18] = 0;
  s_joinResponse->setValue(response, sizeof(response));
}

static void setFriendResponse(const uint8_t* requestId, uint8_t status) {
  if (!s_friendResponse) return;
  uint8_t response[kFriendResponseBytes] = {'F','M','B','F',1,status};
  if (requestId) memcpy(response + 6, requestId, kFriendRequestIdBytes);
  s_friendResponse->setValue(response, sizeof(response));
}

class FriendRequestCallbacks : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic) override {
    if (!characteristic) return;
    const NimBLEAttValue value = characteristic->getValue();
    const uint8_t* encoded = value.data();
    const size_t length = value.length();
    friendmesh::FriendRequestEnvelope request = {};
    if (!encoded || length != kFriendRequestEncodedBytes ||
        decodeFriendRequest(encoded, length, request) != ResultCode::Ok ||
        !(request.flags & kFriendRequestFlagNearbyBle)) {
      Serial.printf("[FM-FRIEND] BLE GATT RX invalid len=%u\n",
                    (unsigned)length);
      setFriendResponse(nullptr, 2);
      return;
    }
    bool queued = false;
    portENTER_CRITICAL(&s_presenceMux);
    if (!s_pendingFriendRequestReady) {
      memcpy(s_pendingFriendRequest, encoded, length);
      s_pendingFriendRequestLength = length;
      s_pendingFriendRequestReady = true;
      queued = true;
    }
    portEXIT_CRITICAL(&s_presenceMux);
    Serial.printf("[FM-FRIEND] BLE GATT RX %s id=%02X%02X%02X%02X from=%02X%02X%02X%02X\n",
                  queued ? "queued" : "busy",
                  request.requestId[0], request.requestId[1],
                  request.requestId[2], request.requestId[3],
                  request.requesterPublicKey[0],
                  request.requesterPublicKey[1],
                  request.requesterPublicKey[2],
                  request.requesterPublicKey[3]);
    setFriendResponse(request.requestId, queued ? 0 : 1);
  }
};

FriendRequestCallbacks s_friendRequestCallbacks;

class JoinRequestCallbacks : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* characteristic) override {
    if (!characteristic || !s_joinResponse) return;
    const NimBLEAttValue value = characteristic->getValue();
    const uint8_t* request = value.data();
    const size_t length = value.length();
    uint8_t zeroNonce[8] = {};
    if (length != kJoinRequestBytes ||
        memcmp(request, "FMBJ", 4) != 0 || request[4] != 1) {
      setJoinFailureResponse(0, zeroNonce, 3);
      return;
    }
    const uint32_t sessionId = getU32(request + 5);
    const uint8_t* nonce = request + 9;
    if (!s_joinHostActive || sessionId != s_joinSessionId ||
        (int32_t)(millis() - s_joinExpiresAt) >= 0) {
      setJoinFailureResponse(sessionId, nonce, 2);
      return;
    }
    uint8_t key[32] = {};
    uint8_t plain[kJoinRequestPlainBytes] = {};
    uint8_t tag[16] = {};
    memcpy(tag, request + 18 + kJoinRequestPlainBytes, sizeof(tag));
    if (!deriveJoinKey(s_joinCode, sessionId, nonce, key) ||
        !cryptJoin(false, key, sessionId, nonce, request + 18,
                   sizeof(plain), plain, tag)) {
      setJoinFailureResponse(sessionId, nonce, 1);
      if (++s_joinFailedAttempts >= 5) {
        s_joinHostActive = false;
        s_joinExpiresAt = 0;
      }
      memset(key, 0, sizeof(key));
      return;
    }

    for (size_t i = 0; i < s_joinMemberPrefixCount; ++i) {
      if (memcmp(s_joinMemberPrefixes[i], plain,
                 kBlePresencePrefixBytes) == 0) {
        setJoinFailureResponse(sessionId, nonce, 4);
        memset(key, 0, sizeof(key));
        memset(plain, 0, sizeof(plain));
        return;
      }
    }

    memcpy(s_joinAccepted.pubKey, plain, 32);
    memcpy(s_joinAccepted.name, plain + 32, 32);
    s_joinAccepted.name[31] = '\0';
    s_joinAcceptedReady = true;

    uint8_t responsePlain[kJoinResponsePlainBytes] = {};
    memcpy(responsePlain, s_joinAdminPub, 32);
    memcpy(responsePlain + 32, s_joinAdminName, 32);
    memcpy(responsePlain + 64, s_joinChannelName, 32);
    memcpy(responsePlain + 96, s_joinChannelSecret, 16);
    uint8_t response[kJoinResponseHeaderBytes + kJoinResponsePlainBytes + 16] =
        {'F','M','B','R',1};
    putU32(response + 5, sessionId);
    memcpy(response + 9, nonce, 8);
    response[17] = 0;
    response[18] = kJoinResponsePlainBytes;
    uint8_t responseTag[16] = {};
    if (!cryptJoin(true, key, sessionId, nonce, responsePlain,
                   sizeof(responsePlain), response + kJoinResponseHeaderBytes,
                   responseTag)) {
      setJoinFailureResponse(sessionId, nonce, 3);
    } else {
      memcpy(response + kJoinResponseHeaderBytes + sizeof(responsePlain),
             responseTag, sizeof(responseTag));
      s_joinResponse->setValue(response, sizeof(response));
    }
    memset(key, 0, sizeof(key));
    memset(plain, 0, sizeof(plain));
    memset(responsePlain, 0, sizeof(responsePlain));
  }
};

JoinRequestCallbacks s_joinRequestCallbacks;

bool parsePresence(const std::string& data, uint8_t* prefix,
                   bool& joinHost, uint32_t& sessionId) {
  joinHost = false;
  sessionId = 0;
  const uint8_t* payload =
      reinterpret_cast<const uint8_t*>(data.data());
  if (decodeBlePresencePayload(payload, data.size(), prefix)) return true;
  if (data.size() != kJoinPresencePayloadBytes ||
      payload[0] != kPresenceCompanyLo || payload[1] != kPresenceCompanyHi ||
      payload[2] != kPresenceMagic0 || payload[3] != kPresenceMagic1 ||
      payload[4] != kJoinPresenceVersion) return false;
  memcpy(prefix, payload + 5, kBlePresencePrefixBytes);
  joinHost = true;
  sessionId = getU32(payload + kBlePresencePayloadBytes);
  return sessionId != 0;
}

void recordPeer(const uint8_t* prefix, int8_t rssi, bool joinHost,
                uint32_t sessionId, NimBLEAdvertisedDevice* device) {
  if (!prefix || rssi < kMinimumNearbyRssi ||
      memcmp(prefix, s_localPrefix, kBlePresencePrefixBytes) == 0) return;
  portENTER_CRITICAL(&s_presenceMux);
  for (size_t i = 0; i < s_peerCount; ++i) {
    if (memcmp(s_peers[i].pubKeyPrefix, prefix,
               kBlePresencePrefixBytes) == 0) {
      if (rssi > s_peers[i].rssi) s_peers[i].rssi = rssi;
      if (joinHost) {
        s_peers[i].joinHost = true;
        s_peers[i].joinSessionId = sessionId;
      }
      portEXIT_CRITICAL(&s_presenceMux);
      return;
    }
  }
  if (s_peerCount < kBlePresenceMaxPeers) {
    memcpy(s_peers[s_peerCount].pubKeyPrefix, prefix,
           kBlePresencePrefixBytes);
    s_peers[s_peerCount].rssi = rssi;
    s_peers[s_peerCount].joinHost = joinHost;
    s_peers[s_peerCount].joinSessionId = sessionId;
    if (device) {
      const std::string address = device->getAddress().toString();
      strncpy(s_peers[s_peerCount].address, address.c_str(),
              sizeof(s_peers[s_peerCount].address) - 1);
      s_peers[s_peerCount].addressType = device->getAddressType();
      if (device->haveName()) {
        const std::string name = device->getName();
        strncpy(s_peers[s_peerCount].name, name.c_str(),
                sizeof(s_peers[s_peerCount].name) - 1);
      }
    }
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
      bool joinHost = false;
      uint32_t sessionId = 0;
      if (parsePresence(device->getManufacturerData(i), prefix, joinHost,
                        sessionId)) {
        recordPeer(prefix, (int8_t)device->getRSSI(), joinHost, sessionId,
                   device);
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
  memcpy(s_localPubKey, pubKey, sizeof(s_localPubKey));
  memcpy(s_localPrefix, pubKey, sizeof(s_localPrefix));
  memset(s_localName, 0, sizeof(s_localName));
  if (bleName && bleName[0]) {
    strncpy(s_localName, bleName, sizeof(s_localName) - 1);
    strncpy(s_shortBleName, bleName, sizeof(s_shortBleName) - 1);
  } else {
    strcpy(s_localName, "MeshCore");
    strcpy(s_shortBleName, "MeshCore");
  }
  s_shortBleName[sizeof(s_shortBleName) - 1] = '\0';
  s_identityReady = true;
  s_advertisingApplied = false;
}

bool blePresenceApplyAdvertising() {
  if (!s_identityReady || !NimBLEDevice::getInitialized()) return false;
  NimBLEServer* server = NimBLEDevice::getServer();
  if (!server) return false;
  // Resolve from the live server every time. OTA tears NimBLE down and may
  // allocate the replacement server at the same address, so cached service
  // pointers alone cannot identify a fresh host stack.
  s_joinService = server->getServiceByUUID(kJoinService);
  if (s_joinService) {
    s_joinResponse = s_joinService->getCharacteristic(
        kJoinResponseCharacteristic);
    s_friendResponse = s_joinService->getCharacteristic(
        kFriendResponseCharacteristic);
    s_friendIdentity = s_joinService->getCharacteristic(
        kFriendIdentityCharacteristic);
  } else {
    s_joinService = server->createService(kJoinService);
    NimBLECharacteristic* request = s_joinService->createCharacteristic(
        kJoinRequestCharacteristic, NIMBLE_PROPERTY::WRITE);
    s_joinResponse = s_joinService->createCharacteristic(
        kJoinResponseCharacteristic, NIMBLE_PROPERTY::READ);
    NimBLECharacteristic* friendRequest = s_joinService->createCharacteristic(
        kFriendRequestCharacteristic, NIMBLE_PROPERTY::WRITE);
    s_friendResponse = s_joinService->createCharacteristic(
        kFriendResponseCharacteristic, NIMBLE_PROPERTY::READ);
    s_friendIdentity = s_joinService->createCharacteristic(
        kFriendIdentityCharacteristic, NIMBLE_PROPERTY::READ);
    if (!request || !s_joinResponse || !friendRequest ||
        !s_friendResponse || !s_friendIdentity) return false;
    request->setCallbacks(&s_joinRequestCallbacks);
    friendRequest->setCallbacks(&s_friendRequestCallbacks);
    uint8_t unavailable[kJoinResponseHeaderBytes] = {'F','M','B','R',1};
    unavailable[17] = 2;
    s_joinResponse->setValue(unavailable, sizeof(unavailable));
    setFriendResponse(nullptr, 2);
    s_joinService->start();
  }
  if (!s_joinResponse || !s_friendResponse || !s_friendIdentity) return false;
  uint8_t identity[kFriendIdentityBytes] = {'F','M','I','D',1};
  memcpy(identity + 5, s_localPubKey, sizeof(s_localPubKey));
  memcpy(identity + 37, s_localName, sizeof(s_localName));
  s_friendIdentity->setValue(identity, sizeof(identity));
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

  uint8_t payload[kJoinPresencePayloadBytes] = {};
  if (!encodeBlePresencePayload(s_localPrefix, payload, sizeof(payload))) return false;
  size_t payloadLength = kBlePresencePayloadBytes;
  if (s_joinHostActive &&
      (int32_t)(millis() - s_joinExpiresAt) < 0) {
    payload[4] = kJoinPresenceVersion;
    putU32(payload + kBlePresencePayloadBytes, s_joinSessionId);
    payloadLength = kJoinPresencePayloadBytes;
  }
  NimBLEAdvertisementData response;
  // A host packet is four bytes longer, so cap the short name to keep the
  // complete scan response within BLE's 31-byte legacy advertising limit.
  char advertisedName[16] = {};
  strncpy(advertisedName, s_shortBleName,
          s_joinHostActive ? 10 : sizeof(advertisedName) - 1);
  response.setShortName(advertisedName);
  response.setManufacturerData(std::string(
      reinterpret_cast<const char*>(payload), payloadLength));

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

BleFriendRequestResult bleFriendReadIdentity(
    const BlePresencePeer& peer, BleFriendIdentity& identity) {
  memset(&identity, 0, sizeof(identity));
  if (!peer.address[0] || !NimBLEDevice::getInitialized())
    return BleFriendRequestResult::Unavailable;
  blePresenceCancelScan();
  NimBLEClient* client = NimBLEDevice::createClient(
      NimBLEAddress(std::string(peer.address), peer.addressType));
  if (!client) return BleFriendRequestResult::Unavailable;
  client->setConnectTimeout(5);
  BleFriendRequestResult result = BleFriendRequestResult::ConnectFailed;
  do {
    if (!client->connect(NimBLEAddress(std::string(peer.address),
                                      peer.addressType))) break;
    NimBLERemoteService* service = client->getService(kJoinService);
    if (!service) { result = BleFriendRequestResult::ProtocolError; break; }
    NimBLERemoteCharacteristic* characteristic =
        service->getCharacteristic(kFriendIdentityCharacteristic);
    if (!characteristic) {
      result = BleFriendRequestResult::ProtocolError;
      break;
    }
    const NimBLEAttValue value = characteristic->readValue();
    const uint8_t* data = value.data();
    if (!data || value.length() != kFriendIdentityBytes ||
        memcmp(data, "FMID", 4) != 0 || data[4] != 1) {
      result = BleFriendRequestResult::ProtocolError;
      break;
    }
    memcpy(identity.pubKey, data + 5, sizeof(identity.pubKey));
    memcpy(identity.name, data + 37, sizeof(identity.name));
    identity.name[sizeof(identity.name) - 1] = '\0';
    if (!identity.name[0] ||
        memcmp(identity.pubKey, peer.pubKeyPrefix,
               kBlePresencePrefixBytes) != 0) {
      memset(&identity, 0, sizeof(identity));
      result = BleFriendRequestResult::IdentityMismatch;
      break;
    }
    result = BleFriendRequestResult::Ok;
  } while (false);
  if (client->isConnected()) client->disconnect();
  NimBLEDevice::deleteClient(client);
  return result;
}

BleFriendRequestResult bleFriendSendRequest(
    const BlePresencePeer& peer, const uint8_t* encodedRequest,
    size_t encodedLength) {
  friendmesh::FriendRequestEnvelope request = {};
  if (!peer.address[0] || !encodedRequest ||
      encodedLength != kFriendRequestEncodedBytes ||
      decodeFriendRequest(encodedRequest, encodedLength, request) !=
          ResultCode::Ok ||
      !(request.flags & kFriendRequestFlagNearbyBle) ||
      !NimBLEDevice::getInitialized())
    return BleFriendRequestResult::Unavailable;
  blePresenceCancelScan();
  NimBLEClient* client = NimBLEDevice::createClient(
      NimBLEAddress(std::string(peer.address), peer.addressType));
  if (!client) return BleFriendRequestResult::Unavailable;
  client->setConnectTimeout(5);
  BleFriendRequestResult result = BleFriendRequestResult::ConnectFailed;
  do {
    if (!client->connect(NimBLEAddress(std::string(peer.address),
                                      peer.addressType))) break;
    NimBLERemoteService* service = client->getService(kJoinService);
    if (!service) { result = BleFriendRequestResult::ProtocolError; break; }
    NimBLERemoteCharacteristic* requestCharacteristic =
        service->getCharacteristic(kFriendRequestCharacteristic);
    NimBLERemoteCharacteristic* responseCharacteristic =
        service->getCharacteristic(kFriendResponseCharacteristic);
    if (!requestCharacteristic || !responseCharacteristic) {
      result = BleFriendRequestResult::ProtocolError;
      break;
    }
    if (!requestCharacteristic->writeValue(
            encodedRequest, encodedLength, true)) {
      result = BleFriendRequestResult::ProtocolError;
      break;
    }
    const NimBLEAttValue responseValue = responseCharacteristic->readValue();
    const uint8_t* response = responseValue.data();
    if (!response || responseValue.length() != kFriendResponseBytes ||
        memcmp(response, "FMBF", 4) != 0 || response[4] != 1 ||
        memcmp(response + 6, request.requestId,
               kFriendRequestIdBytes) != 0) {
      result = BleFriendRequestResult::ProtocolError;
      break;
    }
    result = response[5] == 0 ? BleFriendRequestResult::Ok
        : response[5] == 1 ? BleFriendRequestResult::Busy
                           : BleFriendRequestResult::ProtocolError;
  } while (false);
  if (client->isConnected()) client->disconnect();
  NimBLEDevice::deleteClient(client);
  return result;
}

bool bleFriendTakeRequest(uint8_t* destination, size_t capacity,
                          size_t& written) {
  written = 0;
  if (!destination || capacity < kFriendRequestEncodedBytes) return false;
  portENTER_CRITICAL(&s_presenceMux);
  if (s_pendingFriendRequestReady &&
      s_pendingFriendRequestLength == kFriendRequestEncodedBytes) {
    memcpy(destination, s_pendingFriendRequest,
           s_pendingFriendRequestLength);
    written = s_pendingFriendRequestLength;
    memset(s_pendingFriendRequest, 0, sizeof(s_pendingFriendRequest));
    s_pendingFriendRequestLength = 0;
    s_pendingFriendRequestReady = false;
  }
  portEXIT_CRITICAL(&s_presenceMux);
  return written != 0;
}

bool bleJoinStartHost(const uint8_t adminPubKey[32], const char* adminName,
                      const char* channelName,
                      const uint8_t channelSecret[16],
                      const uint8_t* joinedPubKeyPrefixes,
                      size_t joinedMemberCount, char codeOut[7]) {
  if (!adminPubKey || !channelSecret || !codeOut ||
      !NimBLEDevice::getInitialized() || !s_joinResponse) return false;
  blePresenceCancelScan();
  s_joinSessionId = esp_random();
  if (s_joinSessionId == 0) s_joinSessionId = 1;
  const uint32_t code = esp_random() % 1000000U;
  snprintf(s_joinCode, sizeof(s_joinCode), "%06u", (unsigned)code);
  memcpy(codeOut, s_joinCode, sizeof(s_joinCode));
  memcpy(s_joinAdminPub, adminPubKey, sizeof(s_joinAdminPub));
  memset(s_joinAdminName, 0, sizeof(s_joinAdminName));
  memset(s_joinChannelName, 0, sizeof(s_joinChannelName));
  if (adminName) strncpy(s_joinAdminName, adminName,
                         sizeof(s_joinAdminName) - 1);
  if (channelName) strncpy(s_joinChannelName, channelName,
                           sizeof(s_joinChannelName) - 1);
  memcpy(s_joinChannelSecret, channelSecret, sizeof(s_joinChannelSecret));
  memset(&s_joinAccepted, 0, sizeof(s_joinAccepted));
  s_joinAcceptedReady = false;
  memset(s_joinMemberPrefixes, 0, sizeof(s_joinMemberPrefixes));
  s_joinMemberPrefixCount =
      joinedMemberCount < kBlePresenceMaxPeers
          ? joinedMemberCount : kBlePresenceMaxPeers;
  for (size_t i = 0; i < s_joinMemberPrefixCount; ++i)
    memcpy(s_joinMemberPrefixes[i],
           joinedPubKeyPrefixes + i * kBlePresencePrefixBytes,
           kBlePresencePrefixBytes);
  s_joinFailedAttempts = 0;
  s_joinExpiresAt = millis() + kJoinSessionLifetimeMs;
  s_joinHostActive = true;
  return blePresenceApplyAdvertising();
}

void bleJoinStopHost() {
  if (!s_joinHostActive) return;
  s_joinHostActive = false;
  s_joinSessionId = 0;
  s_joinExpiresAt = 0;
  memset(s_joinCode, 0, sizeof(s_joinCode));
  memset(s_joinChannelSecret, 0, sizeof(s_joinChannelSecret));
  memset(s_joinMemberPrefixes, 0, sizeof(s_joinMemberPrefixes));
  s_joinMemberPrefixCount = 0;
  blePresenceApplyAdvertising();
}

bool bleJoinHostActive() {
  return s_joinHostActive && (int32_t)(millis() - s_joinExpiresAt) < 0;
}

bool bleJoinTakeAcceptedMember(BleJoinAcceptedMember& member) {
  if (!s_joinAcceptedReady) return false;
  member = s_joinAccepted;
  memset(&s_joinAccepted, 0, sizeof(s_joinAccepted));
  s_joinAcceptedReady = false;
  return true;
}

BleJoinResult bleJoinWithHost(const BlePresencePeer& host, const char code[7],
                              const uint8_t memberPubKey[32],
                              const char* memberName,
                              BleJoinProvision& provision) {
  memset(&provision, 0, sizeof(provision));
  if (!host.joinHost || !host.joinSessionId || !host.address[0] ||
      !validCode(code) || !memberPubKey || !NimBLEDevice::getInitialized())
    return BleJoinResult::Unavailable;
  blePresenceCancelScan();
  NimBLEClient* client = NimBLEDevice::createClient(
      NimBLEAddress(std::string(host.address), host.addressType));
  if (!client) return BleJoinResult::Unavailable;
  client->setConnectTimeout(5);
  BleJoinResult result = BleJoinResult::ConnectFailed;
  uint8_t key[32] = {};
  uint8_t requestPlain[kJoinRequestPlainBytes] = {};
  uint8_t request[kJoinRequestBytes] = {'F','M','B','J',1};
  uint8_t requestTag[16] = {};
  do {
    if (!client->connect(NimBLEAddress(std::string(host.address),
                                      host.addressType))) break;
    NimBLERemoteService* service = client->getService(kJoinService);
    if (!service) { result = BleJoinResult::ProtocolError; break; }
    NimBLERemoteCharacteristic* requestCharacteristic =
        service->getCharacteristic(kJoinRequestCharacteristic);
    NimBLERemoteCharacteristic* responseCharacteristic =
        service->getCharacteristic(kJoinResponseCharacteristic);
    if (!requestCharacteristic || !responseCharacteristic) {
      result = BleJoinResult::ProtocolError;
      break;
    }
    putU32(request + 5, host.joinSessionId);
    esp_fill_random(request + 9, 8);
    request[17] = kJoinRequestPlainBytes;
    memcpy(requestPlain, memberPubKey, 32);
    if (memberName) strncpy(reinterpret_cast<char*>(requestPlain + 32),
                            memberName, 31);
    if (!deriveJoinKey(code, host.joinSessionId, request + 9, key) ||
        !cryptJoin(true, key, host.joinSessionId, request + 9, requestPlain,
                   sizeof(requestPlain), request + 18, requestTag)) {
      result = BleJoinResult::ProtocolError;
      break;
    }
    memcpy(request + 18 + sizeof(requestPlain), requestTag,
           sizeof(requestTag));
    if (!requestCharacteristic->writeValue(request, sizeof(request), true)) {
      result = BleJoinResult::ProtocolError;
      break;
    }
    const NimBLEAttValue responseValue = responseCharacteristic->readValue();
    const uint8_t* response = responseValue.data();
    const size_t responseLength = responseValue.length();
    if (responseLength < kJoinResponseHeaderBytes ||
        memcmp(response, "FMBR", 4) != 0 || response[4] != 1 ||
        getU32(response + 5) != host.joinSessionId ||
        memcmp(response + 9, request + 9, 8) != 0) {
      result = BleJoinResult::ProtocolError;
      break;
    }
    if (response[17] == 1) { result = BleJoinResult::InvalidCode; break; }
    if (response[17] == 2) { result = BleJoinResult::Expired; break; }
    if (response[17] == 4) { result = BleJoinResult::AlreadyMember; break; }
    if (response[17] != 0 || response[18] != kJoinResponsePlainBytes ||
        responseLength != kJoinResponseHeaderBytes +
                          kJoinResponsePlainBytes + 16) {
      result = BleJoinResult::ProtocolError;
      break;
    }
    uint8_t responsePlain[kJoinResponsePlainBytes] = {};
    uint8_t responseTag[16] = {};
    memcpy(responseTag,
           response + kJoinResponseHeaderBytes + kJoinResponsePlainBytes,
           sizeof(responseTag));
    if (!cryptJoin(false, key, host.joinSessionId, request + 9,
                   response + kJoinResponseHeaderBytes,
                   kJoinResponsePlainBytes, responsePlain, responseTag)) {
      result = BleJoinResult::InvalidCode;
      break;
    }
    memcpy(provision.adminPubKey, responsePlain, 32);
    memcpy(provision.adminName, responsePlain + 32, 32);
    memcpy(provision.channelName, responsePlain + 64, 32);
    memcpy(provision.channelSecret, responsePlain + 96, 16);
    provision.adminName[31] = '\0';
    provision.channelName[31] = '\0';
    result = BleJoinResult::Ok;
  } while (false);
  if (client->isConnected()) client->disconnect();
  NimBLEDevice::deleteClient(client);
  memset(key, 0, sizeof(key));
  memset(requestPlain, 0, sizeof(requestPlain));
  memset(request, 0, sizeof(request));
  return result;
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
BleFriendRequestResult bleFriendReadIdentity(
    const BlePresencePeer&, BleFriendIdentity&) {
  return BleFriendRequestResult::Unavailable;
}
BleFriendRequestResult bleFriendSendRequest(
    const BlePresencePeer&, const uint8_t*, size_t) {
  return BleFriendRequestResult::Unavailable;
}
bool bleFriendTakeRequest(uint8_t*, size_t, size_t& written) {
  written = 0;
  return false;
}
bool bleJoinStartHost(const uint8_t[32], const char*, const char*,
                      const uint8_t[16], const uint8_t*, size_t,
                      char[7]) { return false; }
void bleJoinStopHost() {}
bool bleJoinHostActive() { return false; }
bool bleJoinTakeAcceptedMember(BleJoinAcceptedMember&) { return false; }
BleJoinResult bleJoinWithHost(const BlePresencePeer&, const char[7],
                              const uint8_t[32], const char*,
                              BleJoinProvision&) {
  return BleJoinResult::Unavailable;
}
}  // namespace friendmesh

#endif
