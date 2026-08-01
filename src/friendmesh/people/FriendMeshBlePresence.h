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
  bool joinHost;
  uint32_t joinSessionId;
  uint8_t addressType;
  char address[18];
  char name[16];
};

struct BleJoinProvision {
  uint8_t adminPubKey[32];
  char adminName[32];
  char channelName[32];
  uint8_t channelSecret[16];
};

struct BleJoinAcceptedMember {
  uint8_t pubKey[32];
  char name[32];
};

struct BleFriendIdentity {
  uint8_t pubKey[32];
  char name[32];
};

enum class BleFriendRequestResult : uint8_t {
  Ok,
  Unavailable,
  ConnectFailed,
  IdentityMismatch,
  Busy,
  ProtocolError,
};

enum class BleJoinResult : uint8_t {
  Ok,
  Unavailable,
  InvalidCode,
  AlreadyMember,
  Expired,
  ConnectFailed,
  ProtocolError,
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

// Nearby friendship is consent based. Discovery never mutates either Friends
// list. The client first reads the peer's full identity (and verifies that it
// matches the advertised prefix), then writes a signed FriendRequestEnvelope.
BleFriendRequestResult bleFriendReadIdentity(
    const BlePresencePeer& peer, BleFriendIdentity& identity);
BleFriendRequestResult bleFriendSendRequest(
    const BlePresencePeer& peer, const uint8_t* encodedRequest,
    size_t encodedLength);
bool bleFriendTakeRequest(uint8_t* destination, size_t capacity,
                          size_t& written);

// A host session exists only while the admin's invite screen is open. The
// six-digit code is never advertised; it derives the key for the final GATT
// exchange. The caller must stop the session when the screen closes.
bool bleJoinStartHost(const uint8_t adminPubKey[32], const char* adminName,
                      const char* channelName, const uint8_t channelSecret[16],
                      const uint8_t* joinedPubKeyPrefixes,
                      size_t joinedMemberCount, char codeOut[7]);
void bleJoinStopHost();
bool bleJoinHostActive();
bool bleJoinTakeAcceptedMember(BleJoinAcceptedMember& member);

// Connects to a host returned by blePresenceCopyPeers(), verifies the code,
// and returns the existing MeshCore channel material. This call is bounded by
// the NimBLE connection timeout and should only run after explicit user input.
BleJoinResult bleJoinWithHost(const BlePresencePeer& host, const char code[7],
                              const uint8_t memberPubKey[32],
                              const char* memberName,
                              BleJoinProvision& provision);

}  // namespace friendmesh
