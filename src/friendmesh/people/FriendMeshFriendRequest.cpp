#include "FriendMeshFriendRequest.h"

#include <string.h>

namespace friendmesh {
namespace {

constexpr uint8_t kRequestMagic[] = {'F', 'M', 'R', '1'};
constexpr uint8_t kAcceptMagic[] = {'F', 'M', 'A', '1'};
constexpr uint8_t kLinkMagic[] = {'F', 'M', 'L', '1'};
constexpr uint8_t kVersion = 1;
constexpr uint8_t kRequestVersion = 2;

bool allZero(const uint8_t* value, size_t length) {
  if (!value) return true;
  for (size_t i = 0; i < length; ++i) if (value[i] != 0) return false;
  return true;
}

void put32(uint8_t* destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t get32(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) |
      (static_cast<uint32_t>(source[1]) << 8) |
      (static_cast<uint32_t>(source[2]) << 16) |
      (static_cast<uint32_t>(source[3]) << 24);
}

bool validName(const char* name) {
  if (!name || !name[0]) return false;
  return strnlen(name, kFriendRequestNameBytes) < kFriendRequestNameBytes;
}

bool validReturnPath(uint8_t encodedLength) {
  const size_t count = encodedLength & 0x3Fu;
  const size_t hashSize = (encodedLength >> 6) + 1u;
  return hashSize <= 3 &&
      count * hashSize <= kFriendRequestReturnPathMaxBytes;
}

}  // namespace

bool friendRequestUnsignedFieldsValid(const FriendRequestEnvelope& request) {
  if ((request.flags & ~kFriendRequestKnownFlags) != 0 ||
      allZero(request.requestId, sizeof(request.requestId)) ||
      allZero(request.targetMessageHash, sizeof(request.targetMessageHash)) ||
      allZero(request.requesterPublicKey, sizeof(request.requesterPublicKey)) ||
      !validName(request.requesterName) ||
      (request.returnPathLength != kFriendRequestReturnPathUnknown &&
       !validReturnPath(request.returnPathLength))) return false;
  if (request.createdAt >= 1700000000UL) {
    if (request.expiresAt <= request.createdAt ||
        request.expiresAt - request.createdAt > kFriendRequestLifetimeSeconds)
      return false;
  } else if (request.expiresAt != 0) {
    return false;
  }
  return true;
}

void makeNearbyFriendTargetHash(
    const uint8_t targetPublicKey[kFriendRequestPublicKeyBytes],
    uint8_t destination[kFriendRequestTargetHashBytes]) {
  if (!destination) return;
  memset(destination, 0, kFriendRequestTargetHashBytes);
  if (!targetPublicKey) return;
  // MeshCore identities are randomly generated public keys. Eight bytes bind
  // the signed request to the BLE peer while keeping the existing fixed wire
  // layout; the full key is still checked against BLE's advertised prefix.
  memcpy(destination, targetPublicKey, kFriendRequestTargetHashBytes);
}

ResultCode encodeFriendRequest(const FriendRequestEnvelope& request,
                               uint8_t* destination, size_t capacity,
                               size_t& written) {
  written = 0;
  if (!destination || capacity < kFriendRequestEncodedBytes ||
      !friendRequestUnsignedFieldsValid(request))
    return ResultCode::InvalidArgument;
  size_t offset = 0;
  memcpy(destination + offset, kRequestMagic, sizeof(kRequestMagic));
  offset += sizeof(kRequestMagic);
  destination[offset++] = kRequestVersion;
  destination[offset++] = request.flags;
  memcpy(destination + offset, request.requestId, sizeof(request.requestId));
  offset += sizeof(request.requestId);
  memcpy(destination + offset, request.targetMessageHash,
         sizeof(request.targetMessageHash));
  offset += sizeof(request.targetMessageHash);
  put32(destination + offset, request.createdAt); offset += 4;
  put32(destination + offset, request.expiresAt); offset += 4;
  memcpy(destination + offset, request.requesterPublicKey,
         sizeof(request.requesterPublicKey));
  offset += sizeof(request.requesterPublicKey);
  memset(destination + offset, 0, kFriendRequestNameBytes);
  const size_t nameLength = strnlen(request.requesterName,
                                    kFriendRequestNameBytes - 1);
  memcpy(destination + offset, request.requesterName, nameLength);
  offset += kFriendRequestNameBytes;
  destination[offset++] = request.returnPathLength;
  memset(destination + offset, 0, kFriendRequestReturnPathMaxBytes);
  const size_t pathBytes = request.returnPathLength ==
          kFriendRequestReturnPathUnknown
      ? 0
      : static_cast<size_t>(request.returnPathLength & 0x3F) *
        static_cast<size_t>((request.returnPathLength >> 6) + 1);
  if (pathBytes) memcpy(destination + offset, request.returnPath, pathBytes);
  offset += kFriendRequestReturnPathMaxBytes;
  memcpy(destination + offset, request.signature, sizeof(request.signature));
  offset += sizeof(request.signature);
  written = offset;
  return ResultCode::Ok;
}

ResultCode decodeFriendRequest(const uint8_t* source, size_t length,
                               FriendRequestEnvelope& request) {
  request = {};
  const bool legacy = length == kFriendRequestV1EncodedBytes;
  if (!source || (!legacy && length != kFriendRequestEncodedBytes) ||
      memcmp(source, kRequestMagic, sizeof(kRequestMagic)) != 0 ||
      source[4] != (legacy ? kVersion : kRequestVersion) ||
      (source[5] & ~kFriendRequestKnownFlags) != 0)
    return ResultCode::CorruptData;
  size_t offset = 6;
  request.flags = source[5];
  memcpy(request.requestId, source + offset, sizeof(request.requestId));
  offset += sizeof(request.requestId);
  memcpy(request.targetMessageHash, source + offset,
         sizeof(request.targetMessageHash));
  offset += sizeof(request.targetMessageHash);
  request.createdAt = get32(source + offset); offset += 4;
  request.expiresAt = get32(source + offset); offset += 4;
  memcpy(request.requesterPublicKey, source + offset,
         sizeof(request.requesterPublicKey));
  offset += sizeof(request.requesterPublicKey);
  memcpy(request.requesterName, source + offset, kFriendRequestNameBytes);
  request.requesterName[kFriendRequestNameBytes - 1] = '\0';
  offset += kFriendRequestNameBytes;
  if (!legacy) {
    request.returnPathLength = source[offset++];
    memcpy(request.returnPath, source + offset,
           kFriendRequestReturnPathMaxBytes);
    offset += kFriendRequestReturnPathMaxBytes;
  } else {
    request.returnPathLength = kFriendRequestReturnPathUnknown;
  }
  memcpy(request.signature, source + offset, sizeof(request.signature));
  return friendRequestUnsignedFieldsValid(request)
      ? ResultCode::Ok : ResultCode::CorruptData;
}

ResultCode encodeFriendAccept(const FriendAcceptEnvelope& response,
                              uint8_t* destination, size_t capacity,
                              size_t& written) {
  written = 0;
  if (!destination || capacity < kFriendAcceptEncodedBytes ||
      allZero(response.requestId, sizeof(response.requestId)) ||
      !validName(response.responderName)) return ResultCode::InvalidArgument;
  size_t offset = 0;
  memcpy(destination + offset, kAcceptMagic, sizeof(kAcceptMagic));
  offset += sizeof(kAcceptMagic);
  destination[offset++] = kVersion;
  memcpy(destination + offset, response.requestId, sizeof(response.requestId));
  offset += sizeof(response.requestId);
  memset(destination + offset, 0, kFriendRequestNameBytes);
  const size_t nameLength = strnlen(response.responderName,
                                    kFriendRequestNameBytes - 1);
  memcpy(destination + offset, response.responderName, nameLength);
  offset += kFriendRequestNameBytes;
  written = offset;
  return ResultCode::Ok;
}

ResultCode decodeFriendAccept(const uint8_t* source, size_t length,
                              FriendAcceptEnvelope& response) {
  response = {};
  if (!source || length != kFriendAcceptEncodedBytes ||
      memcmp(source, kAcceptMagic, sizeof(kAcceptMagic)) != 0 ||
      source[4] != kVersion) return ResultCode::CorruptData;
  size_t offset = 5;
  memcpy(response.requestId, source + offset, sizeof(response.requestId));
  offset += sizeof(response.requestId);
  memcpy(response.responderName, source + offset, kFriendRequestNameBytes);
  response.responderName[kFriendRequestNameBytes - 1] = '\0';
  return (!allZero(response.requestId, sizeof(response.requestId)) &&
          validName(response.responderName))
      ? ResultCode::Ok : ResultCode::CorruptData;
}

ResultCode encodeFriendLink(const FriendLinkEnvelope& link,
                            uint8_t* destination, size_t capacity,
                            size_t& written) {
  written = 0;
  const bool accepted = link.action == FriendLinkAction::Accepted;
  const bool removed = link.action == FriendLinkAction::Removed;
  if (!destination || capacity < kFriendLinkEncodedBytes ||
      (!accepted && !removed) || !validName(link.peerName) ||
      (accepted && allZero(link.requestId, sizeof(link.requestId))) ||
      (removed && !allZero(link.requestId, sizeof(link.requestId))))
    return ResultCode::InvalidArgument;
  size_t offset = 0;
  memcpy(destination + offset, kLinkMagic, sizeof(kLinkMagic));
  offset += sizeof(kLinkMagic);
  destination[offset++] = kVersion;
  destination[offset++] = static_cast<uint8_t>(link.action);
  memcpy(destination + offset, link.requestId, sizeof(link.requestId));
  offset += sizeof(link.requestId);
  memset(destination + offset, 0, kFriendRequestNameBytes);
  const size_t nameLength = strnlen(link.peerName,
                                    kFriendRequestNameBytes - 1);
  memcpy(destination + offset, link.peerName, nameLength);
  offset += kFriendRequestNameBytes;
  written = offset;
  return ResultCode::Ok;
}

ResultCode decodeFriendLink(const uint8_t* source, size_t length,
                            FriendLinkEnvelope& link) {
  link = {};
  if (!source || length != kFriendLinkEncodedBytes ||
      memcmp(source, kLinkMagic, sizeof(kLinkMagic)) != 0 ||
      source[4] != kVersion) return ResultCode::CorruptData;
  link.action = static_cast<FriendLinkAction>(source[5]);
  memcpy(link.requestId, source + 6, sizeof(link.requestId));
  memcpy(link.peerName, source + 6 + sizeof(link.requestId),
         kFriendRequestNameBytes);
  link.peerName[kFriendRequestNameBytes - 1] = '\0';
  const bool accepted = link.action == FriendLinkAction::Accepted;
  const bool removed = link.action == FriendLinkAction::Removed;
  return (validName(link.peerName) &&
          ((accepted && !allZero(link.requestId, sizeof(link.requestId))) ||
           (removed && allZero(link.requestId, sizeof(link.requestId)))))
      ? ResultCode::Ok : ResultCode::CorruptData;
}

size_t reverseMeshPath(const uint8_t* source, uint8_t encodedPathLength,
                       uint8_t* destination, size_t capacity) {
  const size_t count = encodedPathLength & 0x3Fu;
  const size_t hashSize = (encodedPathLength >> 6) + 1u;
  if (hashSize > 3 || count * hashSize > capacity ||
      (count != 0 && (!source || !destination))) return 0;
  for (size_t i = 0; i < count; ++i) {
    memcpy(destination + i * hashSize,
           source + (count - 1 - i) * hashSize, hashSize);
  }
  return count * hashSize;
}

}  // namespace friendmesh
