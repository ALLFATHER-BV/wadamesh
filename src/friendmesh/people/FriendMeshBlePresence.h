#pragma once

#include <stddef.h>
#include <stdint.h>

namespace friendmesh {

constexpr size_t kBlePresencePrefixBytes = 6;
constexpr size_t kBlePresenceMaxPeers = 8;
constexpr size_t kBlePresencePayloadBytes = 5 + kBlePresencePrefixBytes;

struct BlePresencePeer {
  uint8_t pubKeyPrefix[kBlePresencePrefixBytes];
  int8_t rssi;
};

bool encodeBlePresencePayload(const uint8_t* fullPubKey,
                              uint8_t* destination, size_t capacity);
bool decodeBlePresencePayload(const uint8_t* payload, size_t length,
                              uint8_t* pubKeyPrefix);

// BLE is discovery only. The advertised prefix is public identity metadata and
// must never be treated as authentication or as permission to disclose a key.
void blePresenceSetLocalIdentity(const uint8_t* pubKey, const char* bleName);
bool blePresenceApplyAdvertising();

// Starts one bounded asynchronous active scan. Results remain available until
// the next scan. The caller must still use MeshCore's encrypted direct path.
bool blePresenceStartScan(uint32_t durationSeconds = 3);
void blePresenceCancelScan();
bool blePresenceIsScanning();
bool blePresenceScanFinished();
size_t blePresenceCopyPeers(BlePresencePeer* destination, size_t capacity);
bool blePresenceWasSeen(const uint8_t* fullPubKey);

}  // namespace friendmesh
