#include "FriendMeshChannelRoster.h"

#include <string.h>

namespace friendmesh {
namespace {

constexpr uint8_t kMagic[] = {'F', 'M', 'R', '1'};
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderBytes = 8;
constexpr char kTextPrefix[] = "FMRS1:";

bool prefixIsZero(const uint8_t* prefix) {
  if (!prefix) return true;
  for (size_t i = 0; i < kChannelRosterPrefixBytes; ++i) {
    if (prefix[i] != 0) return false;
  }
  return true;
}

bool validRole(uint8_t role) {
  return role <= static_cast<uint8_t>(ChannelRosterRole::Admin);
}

bool validState(uint8_t state) {
  return state <= static_cast<uint8_t>(ChannelRosterState::Removed);
}

char hexDigit(uint8_t value) {
  return value < 10 ? static_cast<char>('0' + value)
                    : static_cast<char>('A' + value - 10);
}

int hexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'A' && value <= 'F') return 10 + value - 'A';
  if (value >= 'a' && value <= 'f') return 10 + value - 'a';
  return -1;
}

}  // namespace

void clearChannelRoster(ChannelRoster& roster) {
  memset(&roster, 0, sizeof(roster));
}

const ChannelRosterMember* findChannelRosterMember(
    const ChannelRoster& roster, const uint8_t* pubKey) {
  if (!pubKey) return nullptr;
  for (size_t i = 0; i < roster.memberCount; ++i) {
    if (memcmp(roster.members[i].pubKeyPrefix, pubKey,
               kChannelRosterPrefixBytes) == 0) return &roster.members[i];
  }
  return nullptr;
}

ChannelRosterMember* findChannelRosterMember(ChannelRoster& roster,
                                              const uint8_t* pubKey) {
  return const_cast<ChannelRosterMember*>(findChannelRosterMember(
      static_cast<const ChannelRoster&>(roster), pubKey));
}

ResultCode setChannelRosterMember(ChannelRoster& roster,
                                  const uint8_t* pubKey,
                                  ChannelRosterRole role,
                                  ChannelRosterState state) {
  if (!pubKey || prefixIsZero(pubKey)) return ResultCode::InvalidId;
  ChannelRosterMember* member = findChannelRosterMember(roster, pubKey);
  if (!member) {
    if (roster.memberCount >= kChannelRosterMaxMembers)
      return ResultCode::CapacityReached;
    member = &roster.members[roster.memberCount++];
    memset(member, 0, sizeof(*member));
    memcpy(member->pubKeyPrefix, pubKey, kChannelRosterPrefixBytes);
  }
  member->role = role;
  member->state = state;
  return ResultCode::Ok;
}

ResultCode encodeChannelRoster(const ChannelRoster& roster,
                               uint8_t* destination, size_t capacity,
                               size_t& written) {
  written = 0;
  if (!destination) return ResultCode::InvalidArgument;
  if (roster.memberCount > kChannelRosterMaxMembers)
    return ResultCode::CorruptData;
  const size_t required = kHeaderBytes +
      roster.memberCount * (kChannelRosterPrefixBytes + 2);
  if (capacity < required) return ResultCode::CapacityReached;
  memcpy(destination, kMagic, sizeof(kMagic));
  destination[4] = kVersion;
  destination[5] = roster.memberCount;
  destination[6] = roster.rekeyRequired ? 1 : 0;
  destination[7] = 0;
  size_t offset = kHeaderBytes;
  for (size_t i = 0; i < roster.memberCount; ++i) {
    const ChannelRosterMember& member = roster.members[i];
    if (prefixIsZero(member.pubKeyPrefix)) return ResultCode::InvalidId;
    memcpy(destination + offset, member.pubKeyPrefix,
           kChannelRosterPrefixBytes);
    offset += kChannelRosterPrefixBytes;
    destination[offset++] = static_cast<uint8_t>(member.role);
    destination[offset++] = static_cast<uint8_t>(member.state);
  }
  written = offset;
  return ResultCode::Ok;
}

ResultCode decodeChannelRoster(const uint8_t* source, size_t length,
                               ChannelRoster& roster) {
  clearChannelRoster(roster);
  if (!source || length < kHeaderBytes ||
      memcmp(source, kMagic, sizeof(kMagic)) != 0 ||
      source[4] != kVersion || source[5] > kChannelRosterMaxMembers ||
      source[6] > 1 || source[7] != 0) return ResultCode::CorruptData;
  const uint8_t count = source[5];
  const size_t expected = kHeaderBytes +
      count * (kChannelRosterPrefixBytes + 2);
  if (length != expected) return ResultCode::CorruptData;
  size_t offset = kHeaderBytes;
  for (uint8_t i = 0; i < count; ++i) {
    const uint8_t* prefix = source + offset;
    offset += kChannelRosterPrefixBytes;
    const uint8_t role = source[offset++];
    const uint8_t state = source[offset++];
    if (prefixIsZero(prefix) || !validRole(role) || !validState(state)) {
      clearChannelRoster(roster);
      return ResultCode::CorruptData;
    }
    if (setChannelRosterMember(
            roster, prefix, static_cast<ChannelRosterRole>(role),
            static_cast<ChannelRosterState>(state)) != ResultCode::Ok ||
        roster.memberCount != i + 1) {
      clearChannelRoster(roster);
      return ResultCode::CorruptData;
    }
  }
  roster.rekeyRequired = source[6] != 0;
  return ResultCode::Ok;
}

bool isChannelRosterText(const char* text) {
  return text && strncmp(text, kTextPrefix, sizeof(kTextPrefix) - 1) == 0;
}

const char* legacyChannelRosterTextPayload(const char* text) {
  if (!text) return nullptr;
  if (isChannelRosterText(text)) return text;
  const char* candidate = text;
  while ((candidate = strstr(candidate, kTextPrefix)) != nullptr) {
    if (candidate >= text + 2 && candidate[-2] == ':' &&
        candidate[-1] == ' ') return candidate;
    candidate += sizeof(kTextPrefix) - 1;
  }
  return nullptr;
}

ResultCode encodeChannelRosterText(const ChannelRoster& roster,
                                   char* destination, size_t capacity,
                                   size_t& written) {
  written = 0;
  if (!destination) return ResultCode::InvalidArgument;
  uint8_t binary[kChannelRosterEncodedBytes] = {};
  size_t binaryLength = 0;
  const ResultCode encoded = encodeChannelRoster(
      roster, binary, sizeof(binary), binaryLength);
  if (encoded != ResultCode::Ok) return encoded;
  const size_t prefixLength = sizeof(kTextPrefix) - 1;
  const size_t required = prefixLength + binaryLength * 2;
  if (capacity < required + 1 || required > kChannelRosterMaxText)
    return ResultCode::CapacityReached;
  memcpy(destination, kTextPrefix, prefixLength);
  for (size_t i = 0; i < binaryLength; ++i) {
    destination[prefixLength + i * 2] = hexDigit(binary[i] >> 4);
    destination[prefixLength + i * 2 + 1] = hexDigit(binary[i] & 0x0F);
  }
  destination[required] = '\0';
  written = required;
  return ResultCode::Ok;
}

ResultCode decodeChannelRosterText(const char* text, ChannelRoster& roster) {
  clearChannelRoster(roster);
  if (!isChannelRosterText(text)) return ResultCode::InvalidArgument;
  const size_t textLength = strnlen(text, kChannelRosterMaxText + 2);
  const size_t prefixLength = sizeof(kTextPrefix) - 1;
  if (textLength > kChannelRosterMaxText || textLength <= prefixLength ||
      ((textLength - prefixLength) & 1U) != 0)
    return ResultCode::InvalidText;
  const size_t binaryLength = (textLength - prefixLength) / 2;
  if (binaryLength > kChannelRosterEncodedBytes) return ResultCode::InvalidText;
  uint8_t binary[kChannelRosterEncodedBytes] = {};
  for (size_t i = 0; i < binaryLength; ++i) {
    const int high = hexValue(text[prefixLength + i * 2]);
    const int low = hexValue(text[prefixLength + i * 2 + 1]);
    if (high < 0 || low < 0) return ResultCode::InvalidText;
    binary[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return decodeChannelRoster(binary, binaryLength, roster);
}

}  // namespace friendmesh
