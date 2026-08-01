#pragma once

#include <stddef.h>
#include <stdint.h>

namespace friendmesh {

constexpr size_t kIdSize = 16;
constexpr size_t kMaxFriendLabelBytes = 32;
constexpr size_t kMaxGroupNameBytes = 40;
constexpr size_t kMaxAliasBytes = 32;
constexpr size_t kMaxFriends = 64;
constexpr size_t kMaxGroups = 8;
constexpr size_t kMaxGroupMembers = 8;
constexpr size_t kMaxOutboxEntries = 32;
constexpr size_t kMaxHistoryEntries = 96;
constexpr size_t kMaxEventPayloadBytes = 1024;

struct Id128 {
  uint8_t bytes[kIdSize];
};

bool idIsZero(const Id128& id);
bool idsEqual(const Id128& lhs, const Id128& rhs);
int compareIds(const Id128& lhs, const Id128& rhs);

// Copies a bounded UTF-8 byte string without splitting validation into the
// storage/domain layer. Input validation can become stricter without changing
// record layouts. Returns false for null, empty, or oversized input.
bool copyBoundedText(char* destination, size_t destinationSize,
                     const char* source, size_t maxBytes);
bool boundedTextEqualFolded(const char* lhs, const char* rhs);

enum class ResultCode : uint8_t {
  Ok = 0,
  InvalidArgument,
  InvalidId,
  InvalidText,
  Duplicate,
  NotFound,
  CapacityReached,
  Conflict,
  Unauthorized,
  InvalidState,
  StorageUnavailable,
  ReadOnly,
  CorruptData,
  Incomplete,
  Quarantined,
};

enum class VerificationState : uint8_t {
  Unverified = 0,
  Pending,
  Verified,
  Replaced,
  Revoked,
};

enum class MemberRole : uint8_t {
  Member = 0,
  Admin,
};

enum class MemberState : uint8_t {
  Pending = 0,
  Approved,
  LeavePending,
  KickPending,
  RekeyPending,
  Replaced,
  Removed,
  BlockedLocal,
  Disbanded,
};

enum class GroupSecurityState : uint8_t {
  NotConfigured = 0,
  Locked,
  ReadyForDevelopment,
  RekeyPending,
  UnsafeConfiguration,
  Degraded,
  RecoveryRequired,
};

enum class GrantState : uint8_t {
  None = 0,
  Pending,
  Active,
  Revoked,
};

enum class LocationVisibility : uint8_t {
  Hidden = 0,
  Precise,
};

}  // namespace friendmesh
