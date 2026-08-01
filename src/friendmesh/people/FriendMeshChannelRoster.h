#pragma once

#include <stddef.h>
#include <stdint.h>

#include "friendmesh/core/FriendMeshCoreTypes.h"

namespace friendmesh {

constexpr size_t kChannelRosterPrefixBytes = 6;
constexpr size_t kChannelRosterMaxMembers = 8;
constexpr size_t kChannelRosterEncodedBytes =
    8 + kChannelRosterMaxMembers * (kChannelRosterPrefixBytes + 2);
constexpr size_t kChannelRosterMaxText = 6 + kChannelRosterEncodedBytes * 2;
// MeshCore's unregistered development range is 0xFF00-0xFFFE. Roster
// snapshots are control datagrams, never user-visible channel text.
constexpr uint16_t kChannelRosterDataType = 0xFF01;

enum class ChannelRosterRole : uint8_t {
  Member = 0,
  Admin = 1,
};

enum class ChannelRosterState : uint8_t {
  Invited = 0,
  Joined = 1,
  InviteFailed = 2,
  Left = 3,
  Removed = 4,
};

struct ChannelRosterMember {
  uint8_t pubKeyPrefix[kChannelRosterPrefixBytes];
  ChannelRosterRole role;
  ChannelRosterState state;
};

struct ChannelRoster {
  ChannelRosterMember members[kChannelRosterMaxMembers];
  uint8_t memberCount;
  bool rekeyRequired;
};

void clearChannelRoster(ChannelRoster& roster);
const ChannelRosterMember* findChannelRosterMember(
    const ChannelRoster& roster, const uint8_t* pubKey);
ChannelRosterMember* findChannelRosterMember(
    ChannelRoster& roster, const uint8_t* pubKey);
ResultCode setChannelRosterMember(ChannelRoster& roster,
                                  const uint8_t* pubKey,
                                  ChannelRosterRole role,
                                  ChannelRosterState state);
ResultCode encodeChannelRoster(const ChannelRoster& roster,
                               uint8_t* destination, size_t capacity,
                               size_t& written);
ResultCode decodeChannelRoster(const uint8_t* source, size_t length,
                               ChannelRoster& roster);
ResultCode encodeChannelRosterText(const ChannelRoster& roster,
                                   char* destination, size_t capacity,
                                   size_t& written);
ResultCode decodeChannelRosterText(const char* text, ChannelRoster& roster);
bool isChannelRosterText(const char* text);
// Compatibility parser for the obsolete raw and sender-prefixed text forms.
// Upgraded nodes consume these without exposing them as chat messages.
const char* legacyChannelRosterTextPayload(const char* text);

}  // namespace friendmesh
