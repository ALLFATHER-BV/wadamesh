#include "FriendMeshDomain.h"

#include <string.h>

namespace friendmesh {

DomainStore::DomainStore() : friendCount_(0), groupCount_(0) {
  memset(friends_, 0, sizeof(friends_));
  memset(groups_, 0, sizeof(groups_));
}

const FriendRecord* DomainStore::friendById(const Id128& id) const {
  for (size_t i = 0; i < friendCount_; ++i) {
    if (idsEqual(friends_[i].id, id)) return &friends_[i];
  }
  return nullptr;
}

FriendRecord* DomainStore::mutableFriendById(const Id128& id) {
  for (size_t i = 0; i < friendCount_; ++i) {
    if (idsEqual(friends_[i].id, id)) return &friends_[i];
  }
  return nullptr;
}

ResultCode DomainStore::addFriend(const FriendRecord& record) {
  if (idIsZero(record.id) || idIsZero(record.identity.meshIdentity) ||
      idIsZero(record.identity.signingIdentity)) return ResultCode::InvalidId;
  if (record.label[0] == '\0' ||
      strnlen(record.label, sizeof(record.label)) > kMaxFriendLabelBytes) {
    return ResultCode::InvalidText;
  }
  if (friendById(record.id)) return ResultCode::Duplicate;
  for (size_t i = 0; i < friendCount_; ++i) {
    if (idsEqual(friends_[i].identity.meshIdentity, record.identity.meshIdentity) ||
        idsEqual(friends_[i].identity.signingIdentity, record.identity.signingIdentity)) {
      return ResultCode::Conflict;
    }
  }
  if (friendCount_ >= kMaxFriends) return ResultCode::CapacityReached;
  friends_[friendCount_++] = record;
  return ResultCode::Ok;
}

ResultCode DomainStore::setFriendBlocked(const Id128& id, bool blocked) {
  FriendRecord* record = mutableFriendById(id);
  if (!record) return ResultCode::NotFound;
  record->blockedLocally = blocked;
  return ResultCode::Ok;
}

ResultCode DomainStore::setFriendVerification(const Id128& id,
                                              VerificationState verification,
                                              uint32_t authenticatedAt) {
  FriendRecord* record = mutableFriendById(id);
  if (!record) return ResultCode::NotFound;
  record->verification = verification;
  record->lastAuthenticatedAt = authenticatedAt;
  if (verification == VerificationState::Verified && record->firstVerifiedAt == 0) {
    record->firstVerifiedAt = authenticatedAt;
  }
  return ResultCode::Ok;
}

ResultCode DomainStore::replaceFriendIdentity(const Id128& id,
                                              const IdentityReference& replacement,
                                              uint32_t replacedAt) {
  FriendRecord* record = mutableFriendById(id);
  if (!record) return ResultCode::NotFound;
  if (idIsZero(replacement.meshIdentity) || idIsZero(replacement.signingIdentity)) {
    return ResultCode::InvalidId;
  }
  for (size_t i = 0; i < friendCount_; ++i) {
    if (idsEqual(friends_[i].id, id)) continue;
    if (idsEqual(friends_[i].identity.meshIdentity, replacement.meshIdentity) ||
        idsEqual(friends_[i].identity.signingIdentity, replacement.signingIdentity)) {
      return ResultCode::Conflict;
    }
  }
  record->previousIdentity = record->identity;
  record->identity = replacement;
  record->verification = VerificationState::Replaced;
  record->lastAuthenticatedAt = replacedAt;
  if (record->replacementCount != UINT8_MAX) ++record->replacementCount;
  return ResultCode::Ok;
}

const GroupRecord* DomainStore::groupById(const Id128& id) const {
  for (size_t i = 0; i < groupCount_; ++i) {
    if (idsEqual(groups_[i].id, id)) return &groups_[i];
  }
  return nullptr;
}

GroupRecord* DomainStore::mutableGroupById(const Id128& id) {
  for (size_t i = 0; i < groupCount_; ++i) {
    if (idsEqual(groups_[i].id, id)) return &groups_[i];
  }
  return nullptr;
}

const GroupMember* DomainStore::groupMember(const Id128& groupId,
                                             const Id128& friendId) const {
  const GroupRecord* group = groupById(groupId);
  return group ? memberByFriendId(*group, friendId) : nullptr;
}

GroupMember* DomainStore::mutableGroupMember(const Id128& groupId,
                                              const Id128& friendId) {
  GroupRecord* group = mutableGroupById(groupId);
  return group ? memberByFriendId(*group, friendId) : nullptr;
}

bool DomainStore::groupAliasAvailable(const Id128& groupId, const char* alias) const {
  const GroupRecord* group = groupById(groupId);
  return group && alias && alias[0] != '\0' && !aliasExists(*group, alias);
}

GroupMember* DomainStore::memberByFriendId(GroupRecord& group, const Id128& friendId) {
  for (size_t i = 0; i < group.memberCount; ++i) {
    if (idsEqual(group.members[i].friendId, friendId)) return &group.members[i];
  }
  return nullptr;
}

const GroupMember* DomainStore::memberByFriendId(const GroupRecord& group,
                                                  const Id128& friendId) const {
  for (size_t i = 0; i < group.memberCount; ++i) {
    if (idsEqual(group.members[i].friendId, friendId)) return &group.members[i];
  }
  return nullptr;
}

bool DomainStore::aliasExists(const GroupRecord& group, const char* alias) const {
  for (size_t i = 0; i < group.memberCount; ++i) {
    if (boundedTextEqualFolded(group.members[i].alias, alias)) return true;
  }
  return false;
}

ResultCode DomainStore::createGroup(const Id128& groupId, const char* name,
                                    const Id128& adminFriendId, const char* adminAlias,
                                    uint32_t createdAt) {
  if (idIsZero(groupId) || idIsZero(adminFriendId)) return ResultCode::InvalidId;
  if (groupById(groupId)) return ResultCode::Duplicate;
  if (!friendById(adminFriendId)) return ResultCode::NotFound;
  if (groupCount_ >= kMaxGroups) return ResultCode::CapacityReached;

  GroupRecord next = {};
  next.id = groupId;
  if (!copyBoundedText(next.name, sizeof(next.name), name, kMaxGroupNameBytes)) {
    return ResultCode::InvalidText;
  }
  next.membershipEpoch = 1;
  next.administrativeSequence = 1;
  next.security = GroupSecurityState::ReadyForDevelopment;
  next.locationVisibility = LocationVisibility::Hidden;
  next.createdAt = createdAt;
  next.lastActivityAt = createdAt;

  GroupMember& admin = next.members[0];
  admin.friendId = adminFriendId;
  if (!copyBoundedText(admin.alias, sizeof(admin.alias), adminAlias, kMaxAliasBytes)) {
    return ResultCode::InvalidText;
  }
  admin.role = MemberRole::Admin;
  admin.state = MemberState::Approved;
  admin.joinOrder = 1;
  admin.admittedEpoch = 1;
  admin.grantedEpoch = 1;
  admin.grantState = GrantState::Active;
  admin.lastAuthenticatedAt = createdAt;
  next.memberCount = 1;

  groups_[groupCount_++] = next;
  return ResultCode::Ok;
}

ResultCode DomainStore::renameGroup(const Id128& groupId, const char* name,
                                    uint32_t activityAt) {
  GroupRecord* group = mutableGroupById(groupId);
  if (!group) return ResultCode::NotFound;
  char nextName[kMaxGroupNameBytes + 1] = {};
  if (!copyBoundedText(nextName, sizeof(nextName), name, kMaxGroupNameBytes)) {
    return ResultCode::InvalidText;
  }
  memcpy(group->name, nextName, sizeof(group->name));
  ++group->administrativeSequence;
  group->lastActivityAt = activityAt;
  return ResultCode::Ok;
}

ResultCode DomainStore::addMember(const Id128& groupId, const Id128& friendId,
                                  const char* alias, MemberRole role, MemberState state,
                                  uint32_t admittedEpoch, uint32_t activityAt) {
  GroupRecord* group = mutableGroupById(groupId);
  if (!group || !friendById(friendId)) return ResultCode::NotFound;
  if (memberByFriendId(*group, friendId)) return ResultCode::Duplicate;
  if (group->memberCount >= kMaxGroupMembers) return ResultCode::CapacityReached;
  if (!alias || aliasExists(*group, alias)) return ResultCode::Duplicate;
  if (admittedEpoch != group->membershipEpoch) return ResultCode::Conflict;
  if (role == MemberRole::Admin) return ResultCode::Conflict;

  GroupMember member = {};
  member.friendId = friendId;
  if (!copyBoundedText(member.alias, sizeof(member.alias), alias, kMaxAliasBytes)) {
    return ResultCode::InvalidText;
  }
  member.role = role;
  member.state = state;
  member.joinOrder = static_cast<uint16_t>(group->memberCount + 1);
  member.admittedEpoch = admittedEpoch;
  member.grantedEpoch = admittedEpoch;
  member.grantState = state == MemberState::Approved ? GrantState::Active
                                                     : GrantState::None;
  member.lastAuthenticatedAt = activityAt;
  group->members[group->memberCount++] = member;
  group->lastActivityAt = activityAt;
  return ResultCode::Ok;
}

ResultCode DomainStore::setMemberState(const Id128& groupId, const Id128& friendId,
                                       MemberState state, uint32_t epoch,
                                       uint32_t activityAt) {
  GroupRecord* group = mutableGroupById(groupId);
  if (!group) return ResultCode::NotFound;
  GroupMember* member = memberByFriendId(*group, friendId);
  if (!member) return ResultCode::NotFound;
  if (epoch < group->membershipEpoch) return ResultCode::Conflict;
  if (epoch > group->membershipEpoch + 1) return ResultCode::Conflict;
  if (epoch > group->membershipEpoch) group->membershipEpoch = epoch;
  member->state = state;
  if (state == MemberState::Removed || state == MemberState::Disbanded) {
    member->removedEpoch = epoch;
    member->grantState = GrantState::Revoked;
  }
  group->lastActivityAt = activityAt;
  return ResultCode::Ok;
}

ResultCode DomainStore::transferAdmin(const Id128& groupId, const Id128& nextAdminId,
                                      uint32_t nextAdministrativeSequence,
                                      uint32_t activityAt) {
  GroupRecord* group = mutableGroupById(groupId);
  if (!group) return ResultCode::NotFound;
  if (nextAdministrativeSequence != group->administrativeSequence + 1) {
    return ResultCode::Conflict;
  }
  if (group->administrativeSequence == UINT32_MAX) return ResultCode::Conflict;
  GroupMember* next = memberByFriendId(*group, nextAdminId);
  if (!next || next->state != MemberState::Approved) return ResultCode::InvalidState;

  for (size_t i = 0; i < group->memberCount; ++i) {
    if (group->members[i].role == MemberRole::Admin) {
      group->members[i].role = MemberRole::Member;
    }
  }
  next->role = MemberRole::Admin;
  group->administrativeSequence = nextAdministrativeSequence;
  group->lastActivityAt = activityAt;
  return ResultCode::Ok;
}

ResultCode DomainStore::setGroupSecurity(const Id128& groupId,
                                         GroupSecurityState security,
                                         uint32_t activityAt) {
  GroupRecord* group = mutableGroupById(groupId);
  if (!group) return ResultCode::NotFound;
  group->security = security;
  group->lastActivityAt = activityAt;
  return ResultCode::Ok;
}

ResultCode DomainStore::beginRekey(const Id128& groupId,
                                   const Id128& excludedFriendId,
                                   uint32_t nextEpoch, uint32_t activityAt) {
  GroupRecord* group = mutableGroupById(groupId);
  if (!group) return ResultCode::NotFound;
  if (group->membershipEpoch == UINT32_MAX) return ResultCode::Conflict;
  if (group->security != GroupSecurityState::ReadyForDevelopment) {
    return ResultCode::InvalidState;
  }
  if (nextEpoch != group->membershipEpoch + 1) return ResultCode::Conflict;
  if (!memberByFriendId(*group, excludedFriendId)) return ResultCode::NotFound;

  group->membershipEpoch = nextEpoch;
  group->security = GroupSecurityState::RekeyPending;
  group->lastActivityAt = activityAt;
  for (size_t i = 0; i < group->memberCount; ++i) {
    GroupMember& member = group->members[i];
    member.grantedEpoch = nextEpoch;
    if (idsEqual(member.friendId, excludedFriendId) ||
        member.state == MemberState::Removed ||
        member.state == MemberState::Disbanded) {
      member.grantState = GrantState::Revoked;
    } else if (member.state == MemberState::Approved) {
      member.grantState = GrantState::Pending;
    }
  }
  return ResultCode::Ok;
}

ResultCode DomainStore::requireMemberGrant(const Id128& groupId,
                                           const Id128& friendId,
                                           uint32_t epoch,
                                           uint32_t activityAt) {
  GroupRecord* group = mutableGroupById(groupId);
  if (!group) return ResultCode::NotFound;
  GroupMember* member = memberByFriendId(*group, friendId);
  if (!member) return ResultCode::NotFound;
  if (group->security != GroupSecurityState::RekeyPending ||
      epoch != group->membershipEpoch || member->state != MemberState::Approved) {
    return ResultCode::InvalidState;
  }
  member->grantedEpoch = epoch;
  member->grantState = GrantState::Pending;
  group->lastActivityAt = activityAt;
  return ResultCode::Ok;
}

ResultCode DomainStore::activateMemberGrant(const Id128& groupId,
                                            const Id128& friendId,
                                            uint32_t epoch,
                                            uint32_t activityAt) {
  GroupRecord* group = mutableGroupById(groupId);
  if (!group) return ResultCode::NotFound;
  GroupMember* member = memberByFriendId(*group, friendId);
  if (!member) return ResultCode::NotFound;
  if (group->security != GroupSecurityState::RekeyPending ||
      epoch != group->membershipEpoch || member->state != MemberState::Approved ||
      member->grantedEpoch != epoch || member->grantState != GrantState::Pending) {
    return ResultCode::InvalidState;
  }
  member->grantState = GrantState::Active;
  group->lastActivityAt = activityAt;
  return ResultCode::Ok;
}

ResultCode DomainStore::completeRekey(const Id128& groupId, uint32_t epoch,
                                      uint32_t activityAt) {
  GroupRecord* group = mutableGroupById(groupId);
  if (!group) return ResultCode::NotFound;
  if (group->security != GroupSecurityState::RekeyPending ||
      epoch != group->membershipEpoch) {
    return ResultCode::InvalidState;
  }
  for (size_t i = 0; i < group->memberCount; ++i) {
    const GroupMember& member = group->members[i];
    if (member.state == MemberState::Approved &&
        (member.grantedEpoch != epoch || member.grantState != GrantState::Active)) {
      return ResultCode::Conflict;
    }
  }
  group->security = GroupSecurityState::ReadyForDevelopment;
  group->lastActivityAt = activityAt;
  return ResultCode::Ok;
}

ResultCode DomainStore::disbandGroup(const Id128& groupId, uint32_t nextEpoch,
                                     uint32_t activityAt) {
  GroupRecord* group = mutableGroupById(groupId);
  if (!group) return ResultCode::NotFound;
  if (group->membershipEpoch == UINT32_MAX) return ResultCode::Conflict;
  if (group->administrativeSequence == UINT32_MAX) return ResultCode::Conflict;
  if (nextEpoch != group->membershipEpoch + 1) return ResultCode::Conflict;
  group->membershipEpoch = nextEpoch;
  group->security = GroupSecurityState::NotConfigured;
  ++group->administrativeSequence;
  group->lastActivityAt = activityAt;
  for (size_t i = 0; i < group->memberCount; ++i) {
    group->members[i].state = MemberState::Disbanded;
    group->members[i].removedEpoch = nextEpoch;
    group->members[i].grantedEpoch = nextEpoch;
    group->members[i].grantState = GrantState::Revoked;
  }
  return ResultCode::Ok;
}

}  // namespace friendmesh
