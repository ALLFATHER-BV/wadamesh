#include "FriendMeshChannelInvite.h"

#include <string.h>

namespace friendmesh {

namespace {

const char kPrefix[] = "FMCH1:";
const char kJoinedPrefix[] = "FMCA1:";
const char kLeftPrefix[] = "FMCL1:";
const char kRemovedPrefix[] = "FMCR1:";
const char kCompassStartedPrefix[] = "FMCP1:";

char hexDigit(uint8_t value) {
  return value < 10 ? (char)('0' + value) : (char)('A' + value - 10);
}

int hexValue(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'A' && value <= 'F') return 10 + value - 'A';
  if (value >= 'a' && value <= 'f') return 10 + value - 'a';
  return -1;
}

void encodeHex(const uint8_t* source, size_t length, char* destination) {
  for (size_t i = 0; i < length; ++i) {
    destination[i * 2] = hexDigit((uint8_t)(source[i] >> 4));
    destination[i * 2 + 1] = hexDigit((uint8_t)(source[i] & 0x0F));
  }
}

bool decodeHex(const char* source, size_t length, uint8_t* destination) {
  if ((length & 1U) != 0) return false;
  for (size_t i = 0; i < length / 2; ++i) {
    const int high = hexValue(source[i * 2]);
    const int low = hexValue(source[i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    destination[i] = (uint8_t)((high << 4) | low);
  }
  return true;
}

}  // namespace

bool isDirectChannelInviteText(const char* text) {
  return text && strncmp(text, kPrefix, sizeof(kPrefix) - 1) == 0;
}

const char* controlPrefix(ChannelControlType type) {
  switch (type) {
    case ChannelControlType::Joined: return kJoinedPrefix;
    case ChannelControlType::Left: return kLeftPrefix;
    case ChannelControlType::Removed: return kRemovedPrefix;
  }
  return nullptr;
}

bool isChannelControlText(const char* text) {
  if (!text) return false;
  return strncmp(text, kJoinedPrefix, sizeof(kJoinedPrefix) - 1) == 0 ||
         strncmp(text, kLeftPrefix, sizeof(kLeftPrefix) - 1) == 0 ||
         strncmp(text, kRemovedPrefix, sizeof(kRemovedPrefix) - 1) == 0;
}

bool isCompassStartedNoticeText(const char* text) {
  return text && strncmp(text, kCompassStartedPrefix,
                         sizeof(kCompassStartedPrefix) - 1) == 0;
}

ResultCode encodeCompassStartedNotice(const CompassStartedNotice& notice,
                                      char* destination, size_t capacity,
                                      size_t& written) {
  written = 0;
  if (!destination || notice.distanceMeters > kCompassStartedMaxDistanceMeters)
    return ResultCode::InvalidArgument;
  const size_t prefixLength = sizeof(kCompassStartedPrefix) - 1;
  const size_t required = prefixLength +
      (kChannelControlTagBytes + sizeof(notice.distanceMeters)) * 2;
  if (capacity < required + 1 || required > kCompassStartedNoticeMaxText)
    return ResultCode::CapacityReached;
  memcpy(destination, kCompassStartedPrefix, prefixLength);
  encodeHex(notice.channelTag, kChannelControlTagBytes,
            destination + prefixLength);
  const uint8_t distance[] = {
      static_cast<uint8_t>(notice.distanceMeters >> 24),
      static_cast<uint8_t>(notice.distanceMeters >> 16),
      static_cast<uint8_t>(notice.distanceMeters >> 8),
      static_cast<uint8_t>(notice.distanceMeters),
  };
  encodeHex(distance, sizeof(distance),
            destination + prefixLength + kChannelControlTagBytes * 2);
  destination[required] = '\0';
  written = required;
  return ResultCode::Ok;
}

ResultCode decodeCompassStartedNotice(const char* text,
                                      CompassStartedNotice& notice) {
  notice = {};
  if (!isCompassStartedNoticeText(text)) return ResultCode::InvalidArgument;
  const size_t prefixLength = sizeof(kCompassStartedPrefix) - 1;
  const size_t required = prefixLength +
      (kChannelControlTagBytes + sizeof(notice.distanceMeters)) * 2;
  uint8_t distance[sizeof(notice.distanceMeters)] = {};
  if (strlen(text) != required ||
      !decodeHex(text + prefixLength, kChannelControlTagBytes * 2,
                 notice.channelTag) ||
      !decodeHex(text + prefixLength + kChannelControlTagBytes * 2,
                 sizeof(distance) * 2, distance)) {
    notice = {};
    return ResultCode::InvalidText;
  }
  notice.distanceMeters =
      (static_cast<uint32_t>(distance[0]) << 24) |
      (static_cast<uint32_t>(distance[1]) << 16) |
      (static_cast<uint32_t>(distance[2]) << 8) |
      static_cast<uint32_t>(distance[3]);
  if (notice.distanceMeters > kCompassStartedMaxDistanceMeters) {
    notice = {};
    return ResultCode::InvalidText;
  }
  return ResultCode::Ok;
}

ResultCode encodeChannelControl(ChannelControlType type,
                                const uint8_t tag[kChannelControlTagBytes],
                                char* destination, size_t capacity,
                                size_t& written) {
  written = 0;
  const char* prefix = controlPrefix(type);
  if (!prefix || !tag || !destination) return ResultCode::InvalidArgument;
  const size_t prefixLength = strlen(prefix);
  const size_t required = prefixLength + kChannelControlTagBytes * 2;
  if (capacity < required + 1 || required > kChannelControlMaxText)
    return ResultCode::CapacityReached;
  memcpy(destination, prefix, prefixLength);
  encodeHex(tag, kChannelControlTagBytes, destination + prefixLength);
  destination[required] = '\0';
  written = required;
  return ResultCode::Ok;
}

ResultCode decodeChannelControl(const char* text, ChannelControlType& type,
                                uint8_t tag[kChannelControlTagBytes]) {
  if (!text || !tag) return ResultCode::InvalidArgument;
  const char* prefix = nullptr;
  if (strncmp(text, kJoinedPrefix, sizeof(kJoinedPrefix) - 1) == 0) {
    type = ChannelControlType::Joined;
    prefix = kJoinedPrefix;
  } else if (strncmp(text, kLeftPrefix, sizeof(kLeftPrefix) - 1) == 0) {
    type = ChannelControlType::Left;
    prefix = kLeftPrefix;
  } else if (strncmp(text, kRemovedPrefix,
                     sizeof(kRemovedPrefix) - 1) == 0) {
    type = ChannelControlType::Removed;
    prefix = kRemovedPrefix;
  } else {
    return ResultCode::InvalidArgument;
  }
  const size_t prefixLength = strlen(prefix);
  if (strlen(text) != prefixLength + kChannelControlTagBytes * 2 ||
      !decodeHex(text + prefixLength, kChannelControlTagBytes * 2, tag)) {
    memset(tag, 0, kChannelControlTagBytes);
    return ResultCode::InvalidText;
  }
  return ResultCode::Ok;
}

ResultCode encodeDirectChannelInvite(const DirectChannelInvite& invite,
                                     char* destination, size_t capacity,
                                     size_t& written) {
  written = 0;
  if (!destination || capacity == 0) return ResultCode::InvalidArgument;
  const size_t nameLength = strnlen(invite.channelName,
                                    sizeof(invite.channelName));
  if (nameLength == 0 || nameLength > kMeshCoreChannelNameBytes) {
    return ResultCode::InvalidText;
  }
  const size_t required = (sizeof(kPrefix) - 1) + 2 + 1 +
                          nameLength * 2 + 1 +
                          kMeshCoreChannelSecretBytes * 2;
  if (required + 1 > capacity || required > kDirectChannelInviteMaxText) {
    return ResultCode::CapacityReached;
  }

  size_t offset = 0;
  memcpy(destination + offset, kPrefix, sizeof(kPrefix) - 1);
  offset += sizeof(kPrefix) - 1;
  destination[offset++] = hexDigit((uint8_t)(nameLength >> 4));
  destination[offset++] = hexDigit((uint8_t)(nameLength & 0x0F));
  destination[offset++] = ':';
  encodeHex(reinterpret_cast<const uint8_t*>(invite.channelName), nameLength,
            destination + offset);
  offset += nameLength * 2;
  destination[offset++] = ':';
  encodeHex(invite.channelSecret, sizeof(invite.channelSecret),
            destination + offset);
  offset += sizeof(invite.channelSecret) * 2;
  destination[offset] = '\0';
  written = offset;
  return ResultCode::Ok;
}

ResultCode decodeDirectChannelInvite(const char* text,
                                     DirectChannelInvite& invite) {
  memset(&invite, 0, sizeof(invite));
  if (!isDirectChannelInviteText(text)) return ResultCode::InvalidArgument;
  const size_t textLength = strnlen(text, kDirectChannelInviteMaxText + 2);
  if (textLength > kDirectChannelInviteMaxText) return ResultCode::InvalidText;

  const size_t prefixLength = sizeof(kPrefix) - 1;
  if (textLength < prefixLength + 2 + 1 + 2 + 1 +
                       kMeshCoreChannelSecretBytes * 2) {
    return ResultCode::InvalidText;
  }
  const int nameHigh = hexValue(text[prefixLength]);
  const int nameLow = hexValue(text[prefixLength + 1]);
  if (nameHigh < 0 || nameLow < 0 || text[prefixLength + 2] != ':') {
    return ResultCode::InvalidText;
  }
  const size_t nameLength = (size_t)((nameHigh << 4) | nameLow);
  if (nameLength == 0 || nameLength > kMeshCoreChannelNameBytes) {
    return ResultCode::InvalidText;
  }
  const size_t nameOffset = prefixLength + 3;
  const size_t secretSeparator = nameOffset + nameLength * 2;
  const size_t expectedLength = secretSeparator + 1 +
                                kMeshCoreChannelSecretBytes * 2;
  if (textLength != expectedLength || text[secretSeparator] != ':') {
    return ResultCode::InvalidText;
  }
  if (!decodeHex(text + nameOffset, nameLength * 2,
                 reinterpret_cast<uint8_t*>(invite.channelName)) ||
      !decodeHex(text + secretSeparator + 1,
                 kMeshCoreChannelSecretBytes * 2, invite.channelSecret)) {
    memset(&invite, 0, sizeof(invite));
    return ResultCode::InvalidText;
  }
  invite.channelName[nameLength] = '\0';
  return ResultCode::Ok;
}

}  // namespace friendmesh
