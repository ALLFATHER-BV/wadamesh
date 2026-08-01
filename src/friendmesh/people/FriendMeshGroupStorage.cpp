#include "FriendMeshGroupStorage.h"

#include <string.h>

namespace friendmesh {
namespace {

constexpr uint8_t kMagic[] = {'F', 'M', 'G', '2'};
constexpr uint8_t kVersion = 2;
constexpr size_t kHeaderBytes = 52;
constexpr size_t kChecksumOffset = 48;
constexpr size_t kMemberBytes = 35;
constexpr uint8_t kFlagRekeyRequired = 1U << 0;
constexpr uint8_t kFlagCoordination = 1U << 1;
constexpr uint8_t kFlagPending = 1U << 2;

bool allZero(const uint8_t* value, size_t length) {
  if (!value) return true;
  for (size_t i = 0; i < length; ++i)
    if (value[i] != 0) return false;
  return true;
}

bool validRole(ChannelRosterRole role) {
  return static_cast<uint8_t>(role) <=
      static_cast<uint8_t>(ChannelRosterRole::Admin);
}

bool validState(ChannelRosterState state) {
  return static_cast<uint8_t>(state) <=
      static_cast<uint8_t>(ChannelRosterState::Removed);
}

void put16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
}

uint16_t get16(const uint8_t* source) {
  return static_cast<uint16_t>(source[0]) |
      static_cast<uint16_t>(source[1] << 8);
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

uint32_t checksumUpdate(uint32_t checksum, const uint8_t* source,
                        size_t length) {
  for (size_t i = 0; i < length; ++i) {
    checksum ^= source[i];
    checksum *= 16777619UL;
  }
  return checksum;
}

uint32_t recordChecksum(const uint8_t* source, size_t length) {
  uint32_t result = 2166136261UL;
  result = checksumUpdate(result, source, kChecksumOffset);
  const uint8_t zero[4] = {};
  result = checksumUpdate(result, zero, sizeof(zero));
  return checksumUpdate(result, source + kChecksumOffset + 4,
                        length - kChecksumOffset - 4);
}

bool coordinationPresent(const GroupCoordinationState& state) {
  return !allZero(state.meetup.objectId, sizeof(state.meetup.objectId)) ||
      !allZero(state.incident.objectId, sizeof(state.incident.objectId)) ||
      state.meetupResponseCount != 0 || state.incidentResponseCount != 0;
}

bool memberValid(const GroupStorageMember& member) {
  return (member.publicKeyBytes == kChannelRosterPrefixBytes ||
          member.publicKeyBytes == kGroupStoragePublicKeyBytes) &&
      !allZero(member.publicKey, member.publicKeyBytes) &&
      (member.publicKeyBytes == kGroupStoragePublicKeyBytes ||
       allZero(member.publicKey + kChannelRosterPrefixBytes,
               kGroupStoragePublicKeyBytes - kChannelRosterPrefixBytes)) &&
      validRole(member.role) && validState(member.state);
}

}  // namespace

void clearGroupStorageRecord(GroupStorageRecord& record) {
  memset(&record, 0, sizeof(record));
}

const GroupStorageMember* findGroupStorageMember(
    const GroupStorageRecord& record, const uint8_t* publicKeyOrPrefix) {
  if (!publicKeyOrPrefix) return nullptr;
  for (size_t i = 0; i < record.memberCount; ++i) {
    if (memcmp(record.members[i].publicKey, publicKeyOrPrefix,
               kChannelRosterPrefixBytes) == 0) return &record.members[i];
  }
  return nullptr;
}

ResultCode setGroupStorageMember(GroupStorageRecord& record,
                                 const uint8_t* publicKey,
                                 uint8_t publicKeyBytes,
                                 ChannelRosterRole role,
                                 ChannelRosterState state) {
  if (!publicKey ||
      (publicKeyBytes != kChannelRosterPrefixBytes &&
       publicKeyBytes != kGroupStoragePublicKeyBytes) ||
      allZero(publicKey, publicKeyBytes) || !validRole(role) ||
      !validState(state)) return ResultCode::InvalidArgument;
  GroupStorageMember* member = const_cast<GroupStorageMember*>(
      findGroupStorageMember(record, publicKey));
  if (!member) {
    if (record.memberCount >= kChannelRosterMaxMembers)
      return ResultCode::CapacityReached;
    member = &record.members[record.memberCount++];
    memset(member, 0, sizeof(*member));
  } else if (member->publicKeyBytes == kGroupStoragePublicKeyBytes &&
             publicKeyBytes == kGroupStoragePublicKeyBytes &&
             memcmp(member->publicKey, publicKey,
                    kGroupStoragePublicKeyBytes) != 0) {
    return ResultCode::Conflict;
  }
  if (publicKeyBytes == kGroupStoragePublicKeyBytes ||
      member->publicKeyBytes != kGroupStoragePublicKeyBytes) {
    memset(member->publicKey, 0, sizeof(member->publicKey));
    memcpy(member->publicKey, publicKey, publicKeyBytes);
    member->publicKeyBytes = publicKeyBytes;
  }
  member->role = role;
  member->state = state;
  return ResultCode::Ok;
}

ResultCode groupStorageRecordToRoster(const GroupStorageRecord& record,
                                      ChannelRoster& roster) {
  clearChannelRoster(roster);
  for (size_t i = 0; i < record.memberCount; ++i) {
    const GroupStorageMember& member = record.members[i];
    if (!memberValid(member) ||
        setChannelRosterMember(roster, member.publicKey, member.role,
                               member.state) != ResultCode::Ok)
      return ResultCode::CorruptData;
  }
  roster.rekeyRequired = record.rekeyRequired;
  return ResultCode::Ok;
}

ResultCode encodeGroupStorageRecord(const GroupStorageRecord& record,
                                    uint8_t* destination, size_t capacity,
                                    size_t& written) {
  written = 0;
  if (!destination || allZero(record.channelBinding,
                              sizeof(record.channelBinding)) ||
      record.memberCount > kChannelRosterMaxMembers)
    return ResultCode::InvalidArgument;
  for (size_t i = 0; i < record.memberCount; ++i) {
    if (!memberValid(record.members[i])) return ResultCode::CorruptData;
    for (size_t j = 0; j < i; ++j)
      if (memcmp(record.members[i].publicKey, record.members[j].publicKey,
                 kChannelRosterPrefixBytes) == 0)
        return ResultCode::Conflict;
  }

  uint8_t coordination[kGroupCoordinationStateMaxBytes] = {};
  size_t coordinationLength = 0;
  const bool hasCoordination = coordinationPresent(record.coordination);
  if (hasCoordination &&
      encodeGroupCoordinationState(record.coordination, coordination,
                                   sizeof(coordination),
                                   coordinationLength) != ResultCode::Ok)
    return ResultCode::CorruptData;
  if (record.pending.active &&
      (allZero(record.pending.transactionId,
               sizeof(record.pending.transactionId)) ||
       record.pending.payloadLength == 0 ||
       record.pending.payloadLength > sizeof(record.pending.payload)))
    return ResultCode::InvalidArgument;

  const size_t required = kHeaderBytes + record.memberCount * kMemberBytes +
      coordinationLength +
      (record.pending.active ? record.pending.payloadLength : 0);
  if (required > capacity || required > kGroupStorageRecordMaxBytes)
    return ResultCode::CapacityReached;
  memset(destination, 0, required);
  memcpy(destination, kMagic, sizeof(kMagic));
  destination[4] = kVersion;
  destination[5] = (record.rekeyRequired ? kFlagRekeyRequired : 0) |
      (hasCoordination ? kFlagCoordination : 0) |
      (record.pending.active ? kFlagPending : 0);
  destination[6] = record.memberCount;
  put32(destination + 8, record.generation);
  memcpy(destination + 12, record.channelBinding,
         sizeof(record.channelBinding));
  put16(destination + 28, static_cast<uint16_t>(coordinationLength));
  if (record.pending.active) {
    put16(destination + 30, record.pending.dataType);
    destination[32] = record.pending.payloadLength;
    memcpy(destination + 36, record.pending.transactionId,
           sizeof(record.pending.transactionId));
    put32(destination + 44, record.pending.createdAt);
  }
  size_t offset = kHeaderBytes;
  for (size_t i = 0; i < record.memberCount; ++i) {
    const GroupStorageMember& member = record.members[i];
    destination[offset++] = member.publicKeyBytes;
    memcpy(destination + offset, member.publicKey, sizeof(member.publicKey));
    offset += sizeof(member.publicKey);
    destination[offset++] = static_cast<uint8_t>(member.role);
    destination[offset++] = static_cast<uint8_t>(member.state);
  }
  if (coordinationLength) {
    memcpy(destination + offset, coordination, coordinationLength);
    offset += coordinationLength;
  }
  if (record.pending.active) {
    memcpy(destination + offset, record.pending.payload,
           record.pending.payloadLength);
    offset += record.pending.payloadLength;
  }
  put32(destination + kChecksumOffset, recordChecksum(destination, required));
  written = offset;
  return ResultCode::Ok;
}

ResultCode decodeGroupStorageRecord(const uint8_t* source, size_t length,
                                    GroupStorageRecord& record) {
  clearGroupStorageRecord(record);
  if (!source || length < kHeaderBytes ||
      length > kGroupStorageRecordMaxBytes ||
      memcmp(source, kMagic, sizeof(kMagic)) != 0 ||
      source[4] != kVersion || (source[5] & ~7U) != 0 ||
      source[6] > kChannelRosterMaxMembers || source[7] != 0 ||
      source[33] != 0 || source[34] != 0 || source[35] != 0 ||
      get32(source + kChecksumOffset) != recordChecksum(source, length))
    return ResultCode::CorruptData;
  const bool hasCoordination = (source[5] & kFlagCoordination) != 0;
  const bool hasPending = (source[5] & kFlagPending) != 0;
  const uint16_t coordinationLength = get16(source + 28);
  const uint8_t pendingLength = source[32];
  if ((!hasCoordination && coordinationLength != 0) ||
      (hasCoordination && coordinationLength == 0) ||
      (!hasPending && (pendingLength != 0 || get16(source + 30) != 0 ||
                       get32(source + 44) != 0 ||
                       !allZero(source + 36, 8))) ||
      (hasPending && (pendingLength == 0 ||
                      pendingLength > kGroupStoragePendingPayloadBytes ||
                      allZero(source + 36, 8))))
    return ResultCode::CorruptData;
  const size_t expected = kHeaderBytes + source[6] * kMemberBytes +
      coordinationLength + pendingLength;
  if (length != expected) return ResultCode::CorruptData;

  record.generation = get32(source + 8);
  memcpy(record.channelBinding, source + 12,
         sizeof(record.channelBinding));
  if (allZero(record.channelBinding, sizeof(record.channelBinding)))
    return ResultCode::CorruptData;
  record.rekeyRequired = (source[5] & kFlagRekeyRequired) != 0;
  size_t offset = kHeaderBytes;
  for (uint8_t i = 0; i < source[6]; ++i) {
    const uint8_t keyBytes = source[offset++];
    if (setGroupStorageMember(
            record, source + offset, keyBytes,
            static_cast<ChannelRosterRole>(source[offset + 32]),
            static_cast<ChannelRosterState>(source[offset + 33])) !=
        ResultCode::Ok)
      return ResultCode::CorruptData;
    offset += 34;
  }
  if (hasCoordination) {
    if (decodeGroupCoordinationState(source + offset, coordinationLength,
                                     record.coordination) != ResultCode::Ok)
      return ResultCode::CorruptData;
    offset += coordinationLength;
  }
  if (hasPending) {
    record.pending.active = true;
    record.pending.dataType = get16(source + 30);
    record.pending.payloadLength = pendingLength;
    memcpy(record.pending.transactionId, source + 36,
           sizeof(record.pending.transactionId));
    record.pending.createdAt = get32(source + 44);
    memcpy(record.pending.payload, source + offset, pendingLength);
  }
  return ResultCode::Ok;
}

ResultCode selectGroupStorageSlot(bool firstValid, uint32_t firstGeneration,
                                  bool secondValid, uint32_t secondGeneration,
                                  bool equalGenerationBytesMatch,
                                  uint8_t& selectedSlot) {
  selectedSlot = 0;
  if (!firstValid && !secondValid) return ResultCode::NotFound;
  if (firstValid && !secondValid) return ResultCode::Ok;
  if (!firstValid && secondValid) {
    selectedSlot = 1;
    return ResultCode::Ok;
  }
  if (firstGeneration == secondGeneration) {
    if (!equalGenerationBytesMatch) return ResultCode::Conflict;
    return ResultCode::Ok;
  }
  if (static_cast<int32_t>(secondGeneration - firstGeneration) > 0)
    selectedSlot = 1;
  return ResultCode::Ok;
}

}  // namespace friendmesh
