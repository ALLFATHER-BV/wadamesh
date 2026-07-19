#pragma once

#include "friendmesh/core/FriendMeshCoreTypes.h"

namespace friendmesh {

constexpr size_t kMeshCoreChannelSecretBytes = 16;
constexpr size_t kMeshCoreChannelNameBytes = 31;
constexpr size_t kDirectChannelInviteMaxText = 105;
constexpr size_t kChannelControlTagBytes = 8;
constexpr size_t kChannelControlMaxText = 22;
constexpr size_t kCompassStartedNoticeMaxText = 30;
constexpr uint32_t kCompassStartedMaxDistanceMeters = 21000000;

struct DirectChannelInvite {
  char channelName[kMeshCoreChannelNameBytes + 1];
  uint8_t channelSecret[kMeshCoreChannelSecretBytes];
};

// Compact text envelope carried inside MeshCore's existing authenticated,
// encrypted contact-message packet. This codec provides framing only; callers
// must separately enforce a zero-length direct route at send and receive.
ResultCode encodeDirectChannelInvite(const DirectChannelInvite& invite,
                                     char* destination, size_t capacity,
                                     size_t& written);
ResultCode decodeDirectChannelInvite(const char* text,
                                     DirectChannelInvite& invite);
bool isDirectChannelInviteText(const char* text);

enum class ChannelControlType : uint8_t {
  Joined = 0,
  Left,
  Removed,
};

ResultCode encodeChannelControl(ChannelControlType type,
                                const uint8_t tag[kChannelControlTagBytes],
                                char* destination, size_t capacity,
                                size_t& written);
ResultCode decodeChannelControl(const char* text, ChannelControlType& type,
                                uint8_t tag[kChannelControlTagBytes]);
bool isChannelControlText(const char* text);

// Targeted system notice carried by MeshCore's authenticated, encrypted
// contact-message protocol. The group tag lets the receiver verify that the
// authenticated sender is joined locally; the envelope is consumed before
// chat surfaces.
struct CompassStartedNotice {
  uint8_t channelTag[kChannelControlTagBytes];
  uint32_t distanceMeters;
};

ResultCode encodeCompassStartedNotice(const CompassStartedNotice& notice,
                                      char* destination, size_t capacity,
                                      size_t& written);
ResultCode decodeCompassStartedNotice(const char* text,
                                      CompassStartedNotice& notice);
bool isCompassStartedNoticeText(const char* text);

}  // namespace friendmesh
