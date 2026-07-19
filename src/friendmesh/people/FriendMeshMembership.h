#pragma once

#include "friendmesh/core/FriendMeshDomain.h"

namespace friendmesh {

constexpr size_t kInvitationCodeLength = 6;
constexpr size_t kMaxOpenInvitations = 8;
constexpr size_t kMaxJoinRequests = 16;
constexpr size_t kMaxRemovalOperations = 16;
constexpr size_t kMaxSuccessionOperations = 8;
constexpr uint32_t kDefaultDirectObservationAgeSeconds = 120;

enum class DevelopmentIdentityState : uint8_t {
  Empty = 0,
  Ready,
  Replaced,
};

// Contains public identity references only. It exists to exercise complete
// feature behavior before the production security provider is implemented.
class DevelopmentIdentityLifecycle {
 public:
  DevelopmentIdentityLifecycle();
  DevelopmentIdentityState state() const { return state_; }
  const IdentityReference& identity() const { return identity_; }
  uint32_t generation() const { return generation_; }

  ResultCode create(const IdentityReference& identity);
  ResultCode replace(const IdentityReference& identity);
  void clear();

 private:
  DevelopmentIdentityState state_;
  IdentityReference identity_;
  uint32_t generation_;
};

enum class InvitationState : uint8_t {
  Open = 0,
  Closed,
  Expired,
};

enum class JoinPathPolicy : uint8_t {
  DirectOnly = 0,
  AnyPathWithAdminApproval,
};

enum class DirectPathState : uint8_t {
  Unchecked = 0,
  Observed,
  Rejected,
};

// Evidence about transport behavior, not proof of physical proximity or
// confidentiality. Production code must obtain this from the reviewed MeshCore
// receive path rather than trusting a caller-controlled packet field.
struct DirectPathObservation {
  Id128 exchangeId;
  Id128 ephemeralPublicReference;
  DirectPathState state;
  uint8_t observedHopCount;
  bool forwardingDisabled;
  uint32_t observedAt;
};

struct InvitationSession {
  Id128 invitationId;
  Id128 groupId;
  Id128 inviterId;
  char code[kInvitationCodeLength + 1];
  InvitationState state;
  JoinPathPolicy pathPolicy;
  uint32_t openedAt;
  uint32_t expiresAt;
  uint32_t maxDirectObservationAge;
};

enum class DevelopmentProofState : uint8_t {
  Unchecked = 0,
  ReferencesMatched,
  Rejected,
};

struct VerificationTranscript {
  IdentityReference candidateIdentity;
  uint32_t comparisonNumber;
  DevelopmentProofState proof;
};

enum class JoinRequestState : uint8_t {
  Pending = 0,
  Approved,
  Rejected,
  Expired,
  Cancelled,
};

struct JoinRequest {
  Id128 requestId;
  Id128 invitationId;
  Id128 groupId;
  Id128 candidateFriendId;
  char alias[kMaxAliasBytes + 1];
  VerificationTranscript transcript;
  DirectPathObservation directPath;
  JoinRequestState state;
  uint32_t requestedAt;
  uint32_t expiresAt;
  Id128 reviewedBy;
  uint32_t reviewedAt;
};

enum class RemovalKind : uint8_t {
  Leave = 0,
  Kick,
};

enum class RemovalState : uint8_t {
  Pending = 0,
  Cancelled,
  RekeyRequired,
  Committed,
};

struct RemovalOperation {
  Id128 operationId;
  Id128 groupId;
  Id128 targetFriendId;
  Id128 requestedBy;
  RemovalKind kind;
  RemovalState state;
  uint32_t requestedAt;
  uint32_t reversibleUntil;
  uint32_t nextEpoch;
};

enum class SuccessionState : uint8_t {
  Pending = 0,
  Executed,
  Cancelled,
  Expired,
};

struct SuccessionOperation {
  Id128 operationId;
  Id128 groupId;
  Id128 candidateId;
  Id128 proposedBy;
  Id128 approvals[kMaxGroupMembers];
  uint8_t approvalCount;
  SuccessionState state;
  uint32_t proposedAt;
  uint32_t expiresAt;
  uint32_t executedAt;
};

class MembershipService {
 public:
  explicit MembershipService(DomainStore& domain);

  ResultCode openInvitation(const Id128& invitationId, const Id128& groupId,
                            const Id128& inviterId, const char* code,
                            uint32_t openedAt, uint32_t expiresAt,
                            JoinPathPolicy pathPolicy = JoinPathPolicy::DirectOnly,
                            uint32_t maxDirectObservationAge =
                                kDefaultDirectObservationAgeSeconds);
  ResultCode closeInvitation(const Id128& invitationId, const Id128& actorId);
  const InvitationSession* invitationByCode(const Id128& groupId,
                                            const char* code) const;
  ResultCode submitJoinRequest(const Id128& requestId, const Id128& invitationId,
                               const Id128& candidateFriendId, const char* alias,
                               const VerificationTranscript& transcript,
                               uint32_t requestedAt, uint32_t expiresAt,
                               const DirectPathObservation* directPath = nullptr);
  ResultCode approveJoinRequest(const Id128& requestId, const Id128& adminId,
                                uint32_t reviewedAt);
  ResultCode rejectJoinRequest(const Id128& requestId, const Id128& adminId,
                               uint32_t reviewedAt);
  ResultCode cancelJoinRequest(const Id128& requestId,
                               const Id128& candidateFriendId);
  void expireInvitationsAndRequests(uint32_t now);

  ResultCode requestLeave(const Id128& operationId, const Id128& groupId,
                          const Id128& memberId, uint32_t requestedAt,
                          uint32_t reversibleUntil);
  ResultCode requestKick(const Id128& operationId, const Id128& groupId,
                         const Id128& adminId, const Id128& targetId,
                         uint32_t requestedAt, uint32_t reversibleUntil);
  ResultCode cancelRemoval(const Id128& operationId, const Id128& actorId,
                           uint32_t now);
  ResultCode advanceRemoval(const Id128& operationId, uint32_t now);
  ResultCode commitRemoval(const Id128& operationId, uint32_t committedAt);
  ResultCode activateGrant(const Id128& groupId, const Id128& adminId,
                           const Id128& memberId, uint32_t epoch,
                           uint32_t activatedAt);
  ResultCode completeRekey(const Id128& groupId, const Id128& adminId,
                           uint32_t epoch, uint32_t completedAt);

  ResultCode replaceMemberIdentity(const Id128& groupId, const Id128& actorId,
                                   const Id128& memberId,
                                   const IdentityReference& replacement,
                                   uint32_t replacedAt);
  ResultCode approveReplacement(const Id128& groupId, const Id128& adminId,
                                const Id128& memberId, uint32_t approvedAt);
  ResultCode transferAdministration(const Id128& groupId, const Id128& adminId,
                                    const Id128& nextAdminId, uint32_t at);
  ResultCode proposeSuccession(const Id128& operationId, const Id128& groupId,
                               const Id128& proposerId,
                               const Id128& candidateId, uint32_t proposedAt,
                               uint32_t expiresAt);
  ResultCode approveSuccession(const Id128& operationId,
                               const Id128& approverId, uint32_t approvedAt);
  ResultCode cancelSuccession(const Id128& operationId, const Id128& actorId);
  ResultCode executeSuccession(const Id128& operationId, uint32_t at);
  ResultCode disband(const Id128& groupId, const Id128& adminId, uint32_t at);

  const InvitationSession* invitation(const Id128& id) const;
  const JoinRequest* joinRequest(const Id128& id) const;
  const RemovalOperation* removal(const Id128& id) const;
  const SuccessionOperation* succession(const Id128& id) const;

 private:
  DomainStore& domain_;
  InvitationSession invitations_[kMaxOpenInvitations];
  JoinRequest requests_[kMaxJoinRequests];
  RemovalOperation removals_[kMaxRemovalOperations];
  SuccessionOperation successions_[kMaxSuccessionOperations];
  bool invitationUsed_[kMaxOpenInvitations];
  bool requestUsed_[kMaxJoinRequests];
  bool removalUsed_[kMaxRemovalOperations];
  bool successionUsed_[kMaxSuccessionOperations];

  bool isApprovedMember(const Id128& groupId, const Id128& friendId) const;
  bool isAdmin(const Id128& groupId, const Id128& friendId) const;
  bool invitationCodeValid(const char* code) const;
  bool directPathValid(const InvitationSession& invitation,
                       const DirectPathObservation& observation,
                       uint32_t requestedAt) const;
  InvitationSession* mutableInvitation(const Id128& id);
  JoinRequest* mutableJoinRequest(const Id128& id);
  RemovalOperation* mutableRemoval(const Id128& id);
  SuccessionOperation* mutableSuccession(const Id128& id);
  const GroupMember* earliestEligibleSuccessor(const Id128& groupId) const;
};

}  // namespace friendmesh
