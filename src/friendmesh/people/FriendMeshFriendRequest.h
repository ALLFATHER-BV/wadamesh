#pragma once

#include <stddef.h>
#include <stdint.h>

#include "friendmesh/core/FriendMeshCoreTypes.h"
#include "friendmesh/core/FriendMeshTransaction.h"

namespace friendmesh {

// Friend requests ride inside MeshCore's existing group-data packet.  The
// 0xFFxx range is reserved by this firmware for application-level extensions;
// stock MeshCore nodes relay the packet and ignore the unknown data type.
constexpr uint16_t kFriendRequestDataType = 0xFF03;
constexpr size_t kFriendRequestIdBytes = 8;
constexpr size_t kFriendRequestTargetHashBytes = 8;
constexpr size_t kFriendRequestPublicKeyBytes = 32;
constexpr size_t kFriendRequestNameBytes = 24;
constexpr size_t kFriendRequestSignatureBytes = 64;
constexpr size_t kFriendRequestReturnPathMaxBytes = 14;
constexpr uint8_t kFriendRequestReturnPathUnknown = 0xFF;
constexpr uint32_t kFriendRequestLifetimeSeconds = 24UL * 60UL * 60UL;
constexpr uint8_t kFriendRequestFlagNearbyBle = 0x01;
constexpr uint8_t kFriendRequestKnownFlags = kFriendRequestFlagNearbyBle;

// Fixed encoding keeps parsing bounded on the radio/UI task. The signature is
// always the final field and covers every preceding byte.
constexpr size_t kFriendRequestV1SignedBytes =
    4 + 1 + 1 + kFriendRequestIdBytes + kFriendRequestTargetHashBytes +
    4 + 4 + kFriendRequestPublicKeyBytes + kFriendRequestNameBytes;
constexpr size_t kFriendRequestSignedBytes =
    kFriendRequestV1SignedBytes + 1 + kFriendRequestReturnPathMaxBytes;
constexpr size_t kFriendRequestEncodedBytes =
    kFriendRequestSignedBytes + kFriendRequestSignatureBytes;
constexpr size_t kFriendRequestV1EncodedBytes =
    kFriendRequestV1SignedBytes + kFriendRequestSignatureBytes;

struct FriendRequestEnvelope {
  uint8_t flags;
  uint8_t requestId[kFriendRequestIdBytes];
  uint8_t targetMessageHash[kFriendRequestTargetHashBytes];
  uint32_t createdAt;
  uint32_t expiresAt;
  uint8_t requesterPublicKey[kFriendRequestPublicKeyBytes];
  char requesterName[kFriendRequestNameBytes];
  uint8_t returnPathLength;
  uint8_t returnPath[kFriendRequestReturnPathMaxBytes];
  uint8_t signature[kFriendRequestSignatureBytes];
};

ResultCode encodeFriendRequest(const FriendRequestEnvelope& request,
                               uint8_t* destination, size_t capacity,
                               size_t& written);
ResultCode decodeFriendRequest(const uint8_t* source, size_t length,
                               FriendRequestEnvelope& request);
bool friendRequestUnsignedFieldsValid(const FriendRequestEnvelope& request);
void makeNearbyFriendTargetHash(
    const uint8_t targetPublicKey[kFriendRequestPublicKeyBytes],
    uint8_t destination[kFriendRequestTargetHashBytes]);

// Acceptances use MeshCore's existing authenticated ANON_REQ carrier. Its
// sender key and ECDH/MAC already authenticate the responder, so this compact
// application payload only has to correlate the response to an outgoing ID.
constexpr size_t kFriendAcceptEncodedBytes =
    4 + 1 + kFriendRequestIdBytes + kFriendRequestNameBytes;
struct FriendAcceptEnvelope {
  uint8_t requestId[kFriendRequestIdBytes];
  char responderName[kFriendRequestNameBytes];
};
ResultCode encodeFriendAccept(const FriendAcceptEnvelope& response,
                              uint8_t* destination, size_t capacity,
                              size_t& written);
ResultCode decodeFriendAccept(const uint8_t* source, size_t length,
                              FriendAcceptEnvelope& response);

// Authenticated relationship controls ride in the same MeshCore ANON_REQ
// carrier. Acceptance is user-visible and must correlate to an outgoing
// request. Removal is deliberately quiet and idempotent; the authenticated
// carrier identity is the relationship being removed. Decline is explicit but
// creates no relationship state and therefore needs no ACK-of-the-decline.
enum class FriendLinkAction : uint8_t {
  Accepted = 1,
  Removed = 2,
  Acknowledged = 3,
  Declined = 4,
};
constexpr size_t kFriendLinkV1EncodedBytes =
    4 + 1 + 1 + kFriendRequestIdBytes + kFriendRequestNameBytes;
constexpr size_t kFriendLinkEncodedBytes =
    kFriendLinkV1EncodedBytes + 1 + kFriendRequestReturnPathMaxBytes;
struct FriendLinkEnvelope {
  FriendLinkAction action;
  uint8_t requestId[kFriendRequestIdBytes];
  char peerName[kFriendRequestNameBytes];
  uint8_t returnPathLength;
  uint8_t returnPath[kFriendRequestReturnPathMaxBytes];
};
ResultCode encodeFriendLink(const FriendLinkEnvelope& link,
                            uint8_t* destination, size_t capacity,
                            size_t& written);
ResultCode decodeFriendLink(const uint8_t* source, size_t length,
                            FriendLinkEnvelope& link);

// User-visible transaction progress. Radio/domain code reports these states;
// the UI renders them without inferring delivery from a successful queue call.
// Reverse a MeshCore encoded direct path entry-by-entry. encodedPathLength is
// the normal MeshCore byte: upper bits select hash size and lower 6 bits hold
// the number of repeaters. Returns zero on malformed/capacity failure.
size_t reverseMeshPath(const uint8_t* source, uint8_t encodedPathLength,
                       uint8_t* destination, size_t capacity);

}  // namespace friendmesh
