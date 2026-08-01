#pragma once

#include <stddef.h>
#include <stdint.h>

#include "friendmesh/navigation/FriendMeshGroupCoordination.h"
#include "friendmesh/people/FriendMeshChannelRoster.h"

namespace friendmesh {

constexpr size_t kGroupStorageBindingBytes = 16;
constexpr size_t kGroupStoragePublicKeyBytes = 32;
constexpr size_t kGroupStoragePendingIdBytes = 8;
constexpr size_t kGroupStoragePendingPayloadBytes =
    kGroupCoordinationEventMaxBytes;
constexpr size_t kGroupStorageRecordMaxBytes = 672;

struct GroupStorageMember {
  uint8_t publicKey[kGroupStoragePublicKeyBytes];
  uint8_t publicKeyBytes;
  ChannelRosterRole role;
  ChannelRosterState state;
};

struct GroupStoragePendingMutation {
  bool active;
  uint8_t transactionId[kGroupStoragePendingIdBytes];
  uint16_t dataType;
  uint8_t payloadLength;
  uint8_t payload[kGroupStoragePendingPayloadBytes];
  uint32_t createdAt;
};

struct GroupStorageRecord {
  uint8_t channelBinding[kGroupStorageBindingBytes];
  uint32_t generation;
  GroupStorageMember members[kChannelRosterMaxMembers];
  uint8_t memberCount;
  bool rekeyRequired;
  GroupCoordinationState coordination;
  GroupStoragePendingMutation pending;
};

void clearGroupStorageRecord(GroupStorageRecord& record);
const GroupStorageMember* findGroupStorageMember(
    const GroupStorageRecord& record, const uint8_t* publicKeyOrPrefix);
ResultCode setGroupStorageMember(GroupStorageRecord& record,
                                 const uint8_t* publicKey,
                                 uint8_t publicKeyBytes,
                                 ChannelRosterRole role,
                                 ChannelRosterState state);
ResultCode groupStorageRecordToRoster(const GroupStorageRecord& record,
                                      ChannelRoster& roster);
ResultCode encodeGroupStorageRecord(const GroupStorageRecord& record,
                                    uint8_t* destination, size_t capacity,
                                    size_t& written);
ResultCode decodeGroupStorageRecord(const uint8_t* source, size_t length,
                                    GroupStorageRecord& record);

// Chooses the newest valid two-slot record. Equal-generation records must have
// identical encoded bytes; divergence is recovery-required rather than guessed.
ResultCode selectGroupStorageSlot(bool firstValid, uint32_t firstGeneration,
                                  bool secondValid, uint32_t secondGeneration,
                                  bool equalGenerationBytesMatch,
                                  uint8_t& selectedSlot);

}  // namespace friendmesh
