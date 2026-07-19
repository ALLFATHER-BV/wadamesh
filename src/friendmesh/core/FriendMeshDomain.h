#pragma once

#include "FriendMeshCoreTypes.h"

namespace friendmesh {

struct IdentityReference {
  Id128 meshIdentity;
  Id128 signingIdentity;
};

struct FriendRecord {
  Id128 id;
  IdentityReference identity;
  IdentityReference previousIdentity;
  char label[kMaxFriendLabelBytes + 1];
  VerificationState verification;
  bool blockedLocally;
  bool emergencyContactAllowed;
  uint32_t firstVerifiedAt;
  uint32_t lastAuthenticatedAt;
  uint8_t replacementCount;
};

struct GroupMember {
  Id128 friendId;
  char alias[kMaxAliasBytes + 1];
  MemberRole role;
  MemberState state;
  uint16_t joinOrder;
  uint32_t admittedEpoch;
  uint32_t removedEpoch;
  uint32_t grantedEpoch;
  GrantState grantState;
  uint32_t lastAuthenticatedAt;
};

struct GroupRecord {
  Id128 id;
  char name[kMaxGroupNameBytes + 1];
  uint32_t membershipEpoch;
  uint32_t administrativeSequence;
  GroupSecurityState security;
  LocationVisibility locationVisibility;
  GroupMember members[kMaxGroupMembers];
  uint8_t memberCount;
  uint32_t createdAt;
  uint32_t lastActivityAt;
};

class DomainStore {
 public:
  DomainStore();

  size_t friendCount() const { return friendCount_; }
  size_t groupCount() const { return groupCount_; }

  const FriendRecord* friendById(const Id128& id) const;
  FriendRecord* mutableFriendById(const Id128& id);
  ResultCode addFriend(const FriendRecord& record);
  ResultCode setFriendBlocked(const Id128& id, bool blocked);
  ResultCode setFriendVerification(const Id128& id, VerificationState verification,
                                   uint32_t authenticatedAt);
  ResultCode replaceFriendIdentity(const Id128& id,
                                   const IdentityReference& replacement,
                                   uint32_t replacedAt);

  const GroupRecord* groupById(const Id128& id) const;
  GroupRecord* mutableGroupById(const Id128& id);
  const GroupMember* groupMember(const Id128& groupId,
                                 const Id128& friendId) const;
  GroupMember* mutableGroupMember(const Id128& groupId,
                                  const Id128& friendId);
  bool groupAliasAvailable(const Id128& groupId, const char* alias) const;
  ResultCode createGroup(const Id128& groupId, const char* name,
                         const Id128& adminFriendId, const char* adminAlias,
                         uint32_t createdAt);
  ResultCode renameGroup(const Id128& groupId, const char* name,
                         uint32_t activityAt);
  ResultCode addMember(const Id128& groupId, const Id128& friendId,
                       const char* alias, MemberRole role, MemberState state,
                       uint32_t admittedEpoch, uint32_t activityAt);
  ResultCode setMemberState(const Id128& groupId, const Id128& friendId,
                            MemberState state, uint32_t epoch,
                            uint32_t activityAt);
  ResultCode transferAdmin(const Id128& groupId, const Id128& nextAdminId,
                           uint32_t nextAdministrativeSequence,
                           uint32_t activityAt);
  ResultCode setGroupSecurity(const Id128& groupId, GroupSecurityState security,
                              uint32_t activityAt);
  ResultCode beginRekey(const Id128& groupId, const Id128& excludedFriendId,
                        uint32_t nextEpoch, uint32_t activityAt);
  ResultCode requireMemberGrant(const Id128& groupId, const Id128& friendId,
                                uint32_t epoch, uint32_t activityAt);
  ResultCode activateMemberGrant(const Id128& groupId, const Id128& friendId,
                                 uint32_t epoch, uint32_t activityAt);
  ResultCode completeRekey(const Id128& groupId, uint32_t epoch,
                           uint32_t activityAt);
  ResultCode disbandGroup(const Id128& groupId, uint32_t nextEpoch,
                          uint32_t activityAt);

 private:
  FriendRecord friends_[kMaxFriends];
  GroupRecord groups_[kMaxGroups];
  size_t friendCount_;
  size_t groupCount_;

  GroupMember* memberByFriendId(GroupRecord& group, const Id128& friendId);
  const GroupMember* memberByFriendId(const GroupRecord& group,
                                      const Id128& friendId) const;
  bool aliasExists(const GroupRecord& group, const char* alias) const;
};

}  // namespace friendmesh
