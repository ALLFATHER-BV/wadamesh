#include "FriendMeshMembership.h"

#include <ctype.h>
#include <string.h>

namespace friendmesh {

DevelopmentIdentityLifecycle::DevelopmentIdentityLifecycle()
    : state_(DevelopmentIdentityState::Empty), identity_{}, generation_(0) {}

ResultCode DevelopmentIdentityLifecycle::create(
    const IdentityReference& identity) {
  if (state_ != DevelopmentIdentityState::Empty) return ResultCode::InvalidState;
  if (idIsZero(identity.meshIdentity) || idIsZero(identity.signingIdentity)) {
    return ResultCode::InvalidId;
  }
  identity_ = identity;
  generation_ = 1;
  state_ = DevelopmentIdentityState::Ready;
  return ResultCode::Ok;
}

ResultCode DevelopmentIdentityLifecycle::replace(
    const IdentityReference& identity) {
  if (state_ == DevelopmentIdentityState::Empty) return ResultCode::InvalidState;
  if (idIsZero(identity.meshIdentity) || idIsZero(identity.signingIdentity)) {
    return ResultCode::InvalidId;
  }
  if (idsEqual(identity_.meshIdentity, identity.meshIdentity) ||
      idsEqual(identity_.signingIdentity, identity.signingIdentity)) {
    return ResultCode::Conflict;
  }
  identity_ = identity;
  if (generation_ != UINT32_MAX) ++generation_;
  state_ = DevelopmentIdentityState::Replaced;
  return ResultCode::Ok;
}

void DevelopmentIdentityLifecycle::clear() {
  memset(&identity_, 0, sizeof(identity_));
  generation_ = 0;
  state_ = DevelopmentIdentityState::Empty;
}

MembershipService::MembershipService(DomainStore& domain) : domain_(domain) {
  memset(invitations_, 0, sizeof(invitations_));
  memset(requests_, 0, sizeof(requests_));
  memset(removals_, 0, sizeof(removals_));
  memset(successions_, 0, sizeof(successions_));
  memset(invitationUsed_, 0, sizeof(invitationUsed_));
  memset(requestUsed_, 0, sizeof(requestUsed_));
  memset(removalUsed_, 0, sizeof(removalUsed_));
  memset(successionUsed_, 0, sizeof(successionUsed_));
}

bool MembershipService::isApprovedMember(const Id128& groupId,
                                         const Id128& friendId) const {
  const GroupMember* member = domain_.groupMember(groupId, friendId);
  const FriendRecord* friendRecord = domain_.friendById(friendId);
  return member && friendRecord && !friendRecord->blockedLocally &&
         member->state == MemberState::Approved;
}

bool MembershipService::isAdmin(const Id128& groupId,
                                const Id128& friendId) const {
  const GroupMember* member = domain_.groupMember(groupId, friendId);
  return member && isApprovedMember(groupId, friendId) &&
         member->role == MemberRole::Admin;
}

bool MembershipService::invitationCodeValid(const char* code) const {
  if (!code || strnlen(code, kInvitationCodeLength + 1) != kInvitationCodeLength) {
    return false;
  }
  for (size_t i = 0; i < kInvitationCodeLength; ++i) {
    const unsigned char value = static_cast<unsigned char>(code[i]);
    if (!(isdigit(value) || (value >= 'A' && value <= 'Z'))) return false;
  }
  return true;
}

const InvitationSession* MembershipService::invitation(const Id128& id) const {
  for (size_t i = 0; i < kMaxOpenInvitations; ++i) {
    if (invitationUsed_[i] && idsEqual(invitations_[i].invitationId, id)) {
      return &invitations_[i];
    }
  }
  return nullptr;
}

InvitationSession* MembershipService::mutableInvitation(const Id128& id) {
  for (size_t i = 0; i < kMaxOpenInvitations; ++i) {
    if (invitationUsed_[i] && idsEqual(invitations_[i].invitationId, id)) {
      return &invitations_[i];
    }
  }
  return nullptr;
}

const InvitationSession* MembershipService::invitationByCode(
    const Id128& groupId, const char* code) const {
  if (!invitationCodeValid(code)) return nullptr;
  for (size_t i = 0; i < kMaxOpenInvitations; ++i) {
    if (invitationUsed_[i] && invitations_[i].state == InvitationState::Open &&
        idsEqual(invitations_[i].groupId, groupId) &&
        strcmp(invitations_[i].code, code) == 0) {
      return &invitations_[i];
    }
  }
  return nullptr;
}

const JoinRequest* MembershipService::joinRequest(const Id128& id) const {
  for (size_t i = 0; i < kMaxJoinRequests; ++i) {
    if (requestUsed_[i] && idsEqual(requests_[i].requestId, id)) {
      return &requests_[i];
    }
  }
  return nullptr;
}

JoinRequest* MembershipService::mutableJoinRequest(const Id128& id) {
  for (size_t i = 0; i < kMaxJoinRequests; ++i) {
    if (requestUsed_[i] && idsEqual(requests_[i].requestId, id)) {
      return &requests_[i];
    }
  }
  return nullptr;
}

const RemovalOperation* MembershipService::removal(const Id128& id) const {
  for (size_t i = 0; i < kMaxRemovalOperations; ++i) {
    if (removalUsed_[i] && idsEqual(removals_[i].operationId, id)) {
      return &removals_[i];
    }
  }
  return nullptr;
}

RemovalOperation* MembershipService::mutableRemoval(const Id128& id) {
  for (size_t i = 0; i < kMaxRemovalOperations; ++i) {
    if (removalUsed_[i] && idsEqual(removals_[i].operationId, id)) {
      return &removals_[i];
    }
  }
  return nullptr;
}

const SuccessionOperation* MembershipService::succession(const Id128& id) const {
  for (size_t i = 0; i < kMaxSuccessionOperations; ++i) {
    if (successionUsed_[i] && idsEqual(successions_[i].operationId, id)) {
      return &successions_[i];
    }
  }
  return nullptr;
}

SuccessionOperation* MembershipService::mutableSuccession(const Id128& id) {
  for (size_t i = 0; i < kMaxSuccessionOperations; ++i) {
    if (successionUsed_[i] && idsEqual(successions_[i].operationId, id)) {
      return &successions_[i];
    }
  }
  return nullptr;
}

const GroupMember* MembershipService::earliestEligibleSuccessor(
    const Id128& groupId) const {
  const GroupRecord* group = domain_.groupById(groupId);
  if (!group) return nullptr;
  const GroupMember* earliest = nullptr;
  for (size_t i = 0; i < group->memberCount; ++i) {
    const GroupMember& member = group->members[i];
    if (member.role == MemberRole::Admin ||
        !isApprovedMember(groupId, member.friendId)) {
      continue;
    }
    if (!earliest || member.joinOrder < earliest->joinOrder) earliest = &member;
  }
  return earliest;
}

ResultCode MembershipService::openInvitation(
    const Id128& invitationId, const Id128& groupId, const Id128& inviterId,
    const char* code, uint32_t openedAt, uint32_t expiresAt,
    JoinPathPolicy pathPolicy, uint32_t maxDirectObservationAge) {
  if (idIsZero(invitationId) || idIsZero(groupId) || idIsZero(inviterId)) {
    return ResultCode::InvalidId;
  }
  if (!invitationCodeValid(code) || expiresAt <= openedAt ||
      (pathPolicy == JoinPathPolicy::DirectOnly &&
       maxDirectObservationAge == 0)) {
    return ResultCode::InvalidArgument;
  }
  if (!domain_.groupById(groupId)) return ResultCode::NotFound;
  if (!isApprovedMember(groupId, inviterId)) return ResultCode::Unauthorized;
  if (invitation(invitationId)) return ResultCode::Duplicate;

  size_t freeIndex = kMaxOpenInvitations;
  for (size_t i = 0; i < kMaxOpenInvitations; ++i) {
    if (!invitationUsed_[i] && freeIndex == kMaxOpenInvitations) freeIndex = i;
    if (invitationUsed_[i] && invitations_[i].state == InvitationState::Open &&
        idsEqual(invitations_[i].groupId, groupId) &&
        strcmp(invitations_[i].code, code) == 0) {
      return ResultCode::Duplicate;
    }
  }
  if (freeIndex == kMaxOpenInvitations) return ResultCode::CapacityReached;

  InvitationSession next = {};
  next.invitationId = invitationId;
  next.groupId = groupId;
  next.inviterId = inviterId;
  memcpy(next.code, code, kInvitationCodeLength);
  next.code[kInvitationCodeLength] = '\0';
  next.state = InvitationState::Open;
  next.pathPolicy = pathPolicy;
  next.openedAt = openedAt;
  next.expiresAt = expiresAt;
  next.maxDirectObservationAge = maxDirectObservationAge;
  invitations_[freeIndex] = next;
  invitationUsed_[freeIndex] = true;
  return ResultCode::Ok;
}

bool MembershipService::directPathValid(
    const InvitationSession& invitation,
    const DirectPathObservation& observation, uint32_t requestedAt) const {
  if (observation.state != DirectPathState::Observed ||
      observation.observedHopCount != 0 || !observation.forwardingDisabled ||
      idIsZero(observation.exchangeId) ||
      idIsZero(observation.ephemeralPublicReference) ||
      observation.observedAt < invitation.openedAt ||
      observation.observedAt > requestedAt) {
    return false;
  }
  return requestedAt - observation.observedAt <=
         invitation.maxDirectObservationAge;
}

ResultCode MembershipService::closeInvitation(const Id128& invitationId,
                                              const Id128& actorId) {
  InvitationSession* session = mutableInvitation(invitationId);
  if (!session) return ResultCode::NotFound;
  if (session->state != InvitationState::Open) return ResultCode::InvalidState;
  if (!idsEqual(session->inviterId, actorId) &&
      !isAdmin(session->groupId, actorId)) {
    return ResultCode::Unauthorized;
  }
  session->state = InvitationState::Closed;
  return ResultCode::Ok;
}

ResultCode MembershipService::submitJoinRequest(
    const Id128& requestId, const Id128& invitationId,
    const Id128& candidateFriendId, const char* alias,
    const VerificationTranscript& transcript, uint32_t requestedAt,
    uint32_t expiresAt, const DirectPathObservation* directPath) {
  if (idIsZero(requestId) || idIsZero(candidateFriendId)) {
    return ResultCode::InvalidId;
  }
  InvitationSession* session = mutableInvitation(invitationId);
  if (!session) return ResultCode::NotFound;
  if (session->state != InvitationState::Open || requestedAt >= session->expiresAt) {
    return ResultCode::InvalidState;
  }
  if (requestedAt < session->openedAt || expiresAt <= requestedAt ||
      expiresAt > session->expiresAt || transcript.comparisonNumber == 0) {
    return ResultCode::InvalidArgument;
  }
  if (joinRequest(requestId)) return ResultCode::Duplicate;
  if (session->pathPolicy == JoinPathPolicy::DirectOnly &&
      (!directPath || !directPathValid(*session, *directPath, requestedAt))) {
    return ResultCode::Unauthorized;
  }
  if (directPath && !directPathValid(*session, *directPath, requestedAt)) {
    return ResultCode::InvalidArgument;
  }

  const FriendRecord* candidate = domain_.friendById(candidateFriendId);
  if (!candidate) return ResultCode::NotFound;
  if (candidate->blockedLocally) return ResultCode::Unauthorized;
  if (domain_.groupMember(session->groupId, candidateFriendId)) {
    return ResultCode::Duplicate;
  }
  if (!domain_.groupAliasAvailable(session->groupId, alias)) {
    return ResultCode::Conflict;
  }
  if (transcript.proof != DevelopmentProofState::ReferencesMatched ||
      !idsEqual(transcript.candidateIdentity.meshIdentity,
                candidate->identity.meshIdentity) ||
      !idsEqual(transcript.candidateIdentity.signingIdentity,
                candidate->identity.signingIdentity)) {
    return ResultCode::Unauthorized;
  }

  size_t freeIndex = kMaxJoinRequests;
  for (size_t i = 0; i < kMaxJoinRequests; ++i) {
    if (!requestUsed_[i] && freeIndex == kMaxJoinRequests) freeIndex = i;
    if (requestUsed_[i] && requests_[i].state == JoinRequestState::Pending &&
        idsEqual(requests_[i].groupId, session->groupId) &&
        idsEqual(requests_[i].candidateFriendId, candidateFriendId)) {
      return ResultCode::Duplicate;
    }
  }
  if (freeIndex == kMaxJoinRequests) return ResultCode::CapacityReached;

  JoinRequest next = {};
  next.requestId = requestId;
  next.invitationId = invitationId;
  next.groupId = session->groupId;
  next.candidateFriendId = candidateFriendId;
  if (!copyBoundedText(next.alias, sizeof(next.alias), alias, kMaxAliasBytes)) {
    return ResultCode::InvalidText;
  }
  next.transcript = transcript;
  if (directPath) next.directPath = *directPath;
  next.state = JoinRequestState::Pending;
  next.requestedAt = requestedAt;
  next.expiresAt = expiresAt;
  requests_[freeIndex] = next;
  requestUsed_[freeIndex] = true;
  return ResultCode::Ok;
}

ResultCode MembershipService::approveJoinRequest(const Id128& requestId,
                                                 const Id128& adminId,
                                                 uint32_t reviewedAt) {
  JoinRequest* request = mutableJoinRequest(requestId);
  if (!request) return ResultCode::NotFound;
  if (!isAdmin(request->groupId, adminId)) return ResultCode::Unauthorized;
  if (request->state != JoinRequestState::Pending ||
      reviewedAt >= request->expiresAt) {
    return ResultCode::InvalidState;
  }
  const GroupRecord* group = domain_.groupById(request->groupId);
  if (!group) return ResultCode::NotFound;
  if (group->security != GroupSecurityState::ReadyForDevelopment) {
    return ResultCode::InvalidState;
  }
  const ResultCode added = domain_.addMember(
      request->groupId, request->candidateFriendId, request->alias,
      MemberRole::Member, MemberState::Approved, group->membershipEpoch, reviewedAt);
  if (added != ResultCode::Ok) return added;
  const ResultCode verified = domain_.setFriendVerification(
      request->candidateFriendId, VerificationState::Verified, reviewedAt);
  if (verified != ResultCode::Ok) return verified;
  request->state = JoinRequestState::Approved;
  request->reviewedBy = adminId;
  request->reviewedAt = reviewedAt;
  return ResultCode::Ok;
}

ResultCode MembershipService::rejectJoinRequest(const Id128& requestId,
                                                const Id128& adminId,
                                                uint32_t reviewedAt) {
  JoinRequest* request = mutableJoinRequest(requestId);
  if (!request) return ResultCode::NotFound;
  if (!isAdmin(request->groupId, adminId)) return ResultCode::Unauthorized;
  if (request->state != JoinRequestState::Pending) return ResultCode::InvalidState;
  request->state = JoinRequestState::Rejected;
  request->reviewedBy = adminId;
  request->reviewedAt = reviewedAt;
  return ResultCode::Ok;
}

ResultCode MembershipService::cancelJoinRequest(
    const Id128& requestId, const Id128& candidateFriendId) {
  JoinRequest* request = mutableJoinRequest(requestId);
  if (!request) return ResultCode::NotFound;
  if (!idsEqual(request->candidateFriendId, candidateFriendId)) {
    return ResultCode::Unauthorized;
  }
  if (request->state != JoinRequestState::Pending) return ResultCode::InvalidState;
  request->state = JoinRequestState::Cancelled;
  return ResultCode::Ok;
}

void MembershipService::expireInvitationsAndRequests(uint32_t now) {
  for (size_t i = 0; i < kMaxOpenInvitations; ++i) {
    if (invitationUsed_[i] && invitations_[i].state == InvitationState::Open &&
        now >= invitations_[i].expiresAt) {
      invitations_[i].state = InvitationState::Expired;
    }
  }
  for (size_t i = 0; i < kMaxJoinRequests; ++i) {
    if (requestUsed_[i] && requests_[i].state == JoinRequestState::Pending &&
        now >= requests_[i].expiresAt) {
      requests_[i].state = JoinRequestState::Expired;
    }
  }
}

ResultCode MembershipService::requestLeave(const Id128& operationId,
                                            const Id128& groupId,
                                            const Id128& memberId,
                                            uint32_t requestedAt,
                                            uint32_t reversibleUntil) {
  if (idIsZero(operationId) || idIsZero(groupId) || idIsZero(memberId)) {
    return ResultCode::InvalidId;
  }
  if (reversibleUntil <= requestedAt) return ResultCode::InvalidArgument;
  if (removal(operationId)) return ResultCode::Duplicate;
  if (!isApprovedMember(groupId, memberId)) return ResultCode::InvalidState;
  const GroupMember* leaving = domain_.groupMember(groupId, memberId);
  if (leaving->role == MemberRole::Admin) return ResultCode::Conflict;

  size_t freeIndex = kMaxRemovalOperations;
  for (size_t i = 0; i < kMaxRemovalOperations; ++i) {
    if (!removalUsed_[i] && freeIndex == kMaxRemovalOperations) freeIndex = i;
    if (removalUsed_[i] && removals_[i].state == RemovalState::Pending &&
        idsEqual(removals_[i].groupId, groupId) &&
        idsEqual(removals_[i].targetFriendId, memberId)) {
      return ResultCode::Duplicate;
    }
  }
  if (freeIndex == kMaxRemovalOperations) return ResultCode::CapacityReached;
  const GroupRecord* group = domain_.groupById(groupId);
  if (!group) return ResultCode::NotFound;
  const ResultCode updated = domain_.setMemberState(
      groupId, memberId, MemberState::LeavePending, group->membershipEpoch,
      requestedAt);
  if (updated != ResultCode::Ok) return updated;

  RemovalOperation next = {};
  next.operationId = operationId;
  next.groupId = groupId;
  next.targetFriendId = memberId;
  next.requestedBy = memberId;
  next.kind = RemovalKind::Leave;
  next.state = RemovalState::Pending;
  next.requestedAt = requestedAt;
  next.reversibleUntil = reversibleUntil;
  removals_[freeIndex] = next;
  removalUsed_[freeIndex] = true;
  return ResultCode::Ok;
}

ResultCode MembershipService::requestKick(
    const Id128& operationId, const Id128& groupId, const Id128& adminId,
    const Id128& targetId, uint32_t requestedAt, uint32_t reversibleUntil) {
  if (idIsZero(operationId) || idIsZero(groupId) || idIsZero(adminId) ||
      idIsZero(targetId)) {
    return ResultCode::InvalidId;
  }
  if (reversibleUntil <= requestedAt) return ResultCode::InvalidArgument;
  if (removal(operationId)) return ResultCode::Duplicate;
  if (!isAdmin(groupId, adminId)) return ResultCode::Unauthorized;
  const GroupMember* target = domain_.groupMember(groupId, targetId);
  if (!target || target->state != MemberState::Approved) {
    return ResultCode::InvalidState;
  }
  if (target->role == MemberRole::Admin) return ResultCode::Conflict;

  size_t freeIndex = kMaxRemovalOperations;
  for (size_t i = 0; i < kMaxRemovalOperations; ++i) {
    if (!removalUsed_[i] && freeIndex == kMaxRemovalOperations) freeIndex = i;
    if (removalUsed_[i] && removals_[i].state == RemovalState::Pending &&
        idsEqual(removals_[i].groupId, groupId) &&
        idsEqual(removals_[i].targetFriendId, targetId)) {
      return ResultCode::Duplicate;
    }
  }
  if (freeIndex == kMaxRemovalOperations) return ResultCode::CapacityReached;
  const GroupRecord* group = domain_.groupById(groupId);
  const ResultCode updated = domain_.setMemberState(
      groupId, targetId, MemberState::KickPending, group->membershipEpoch,
      requestedAt);
  if (updated != ResultCode::Ok) return updated;

  RemovalOperation next = {};
  next.operationId = operationId;
  next.groupId = groupId;
  next.targetFriendId = targetId;
  next.requestedBy = adminId;
  next.kind = RemovalKind::Kick;
  next.state = RemovalState::Pending;
  next.requestedAt = requestedAt;
  next.reversibleUntil = reversibleUntil;
  removals_[freeIndex] = next;
  removalUsed_[freeIndex] = true;
  return ResultCode::Ok;
}

ResultCode MembershipService::cancelRemoval(const Id128& operationId,
                                            const Id128& actorId,
                                            uint32_t now) {
  RemovalOperation* operation = mutableRemoval(operationId);
  if (!operation) return ResultCode::NotFound;
  if (operation->state != RemovalState::Pending ||
      now > operation->reversibleUntil) {
    return ResultCode::InvalidState;
  }
  const bool actorAllowed =
      operation->kind == RemovalKind::Leave
          ? (idsEqual(actorId, operation->targetFriendId) ||
             isAdmin(operation->groupId, actorId))
          : (idsEqual(actorId, operation->requestedBy) ||
             isAdmin(operation->groupId, actorId));
  if (!actorAllowed) return ResultCode::Unauthorized;
  const GroupRecord* group = domain_.groupById(operation->groupId);
  if (!group) return ResultCode::NotFound;
  const ResultCode updated = domain_.setMemberState(
      operation->groupId, operation->targetFriendId, MemberState::Approved,
      group->membershipEpoch, now);
  if (updated != ResultCode::Ok) return updated;
  operation->state = RemovalState::Cancelled;
  return ResultCode::Ok;
}

ResultCode MembershipService::advanceRemoval(const Id128& operationId,
                                             uint32_t now) {
  RemovalOperation* operation = mutableRemoval(operationId);
  if (!operation) return ResultCode::NotFound;
  if (operation->state != RemovalState::Pending ||
      now < operation->reversibleUntil) {
    return ResultCode::InvalidState;
  }
  const GroupRecord* group = domain_.groupById(operation->groupId);
  if (!group) return ResultCode::NotFound;
  operation->nextEpoch = group->membershipEpoch + 1;
  const ResultCode rekeyStarted = domain_.beginRekey(
      operation->groupId, operation->targetFriendId, operation->nextEpoch, now);
  if (rekeyStarted != ResultCode::Ok) return rekeyStarted;
  const ResultCode memberUpdated = domain_.setMemberState(
      operation->groupId, operation->targetFriendId, MemberState::RekeyPending,
      operation->nextEpoch, now);
  if (memberUpdated != ResultCode::Ok) return memberUpdated;
  operation->state = RemovalState::RekeyRequired;
  return ResultCode::Ok;
}

ResultCode MembershipService::commitRemoval(const Id128& operationId,
                                            uint32_t committedAt) {
  RemovalOperation* operation = mutableRemoval(operationId);
  if (!operation) return ResultCode::NotFound;
  if (operation->state != RemovalState::RekeyRequired) {
    return ResultCode::InvalidState;
  }
  const ResultCode memberUpdated = domain_.setMemberState(
      operation->groupId, operation->targetFriendId, MemberState::Removed,
      operation->nextEpoch, committedAt);
  if (memberUpdated != ResultCode::Ok) return memberUpdated;
  operation->state = RemovalState::Committed;
  return ResultCode::Ok;
}

ResultCode MembershipService::activateGrant(const Id128& groupId,
                                            const Id128& adminId,
                                            const Id128& memberId,
                                            uint32_t epoch,
                                            uint32_t activatedAt) {
  if (!isAdmin(groupId, adminId)) return ResultCode::Unauthorized;
  return domain_.activateMemberGrant(groupId, memberId, epoch, activatedAt);
}

ResultCode MembershipService::completeRekey(const Id128& groupId,
                                            const Id128& adminId,
                                            uint32_t epoch,
                                            uint32_t completedAt) {
  if (!isAdmin(groupId, adminId)) return ResultCode::Unauthorized;
  return domain_.completeRekey(groupId, epoch, completedAt);
}

ResultCode MembershipService::replaceMemberIdentity(
    const Id128& groupId, const Id128& actorId, const Id128& memberId,
    const IdentityReference& replacement, uint32_t replacedAt) {
  if (!idsEqual(actorId, memberId) && !isAdmin(groupId, actorId)) {
    return ResultCode::Unauthorized;
  }
  if (!isApprovedMember(groupId, memberId)) return ResultCode::InvalidState;
  const GroupMember* replacing = domain_.groupMember(groupId, memberId);
  if (replacing->role == MemberRole::Admin) return ResultCode::Conflict;
  const GroupRecord* group = domain_.groupById(groupId);
  if (group->security != GroupSecurityState::ReadyForDevelopment) {
    return ResultCode::InvalidState;
  }
  if (group->membershipEpoch == UINT32_MAX) return ResultCode::Conflict;
  const ResultCode replaced =
      domain_.replaceFriendIdentity(memberId, replacement, replacedAt);
  if (replaced != ResultCode::Ok) return replaced;
  const uint32_t nextEpoch = group->membershipEpoch + 1;
  const ResultCode rekeyStarted =
      domain_.beginRekey(groupId, memberId, nextEpoch, replacedAt);
  if (rekeyStarted != ResultCode::Ok) return rekeyStarted;
  const ResultCode memberUpdated = domain_.setMemberState(
      groupId, memberId, MemberState::Replaced, nextEpoch, replacedAt);
  if (memberUpdated != ResultCode::Ok) return memberUpdated;
  return ResultCode::Ok;
}

ResultCode MembershipService::approveReplacement(const Id128& groupId,
                                                 const Id128& adminId,
                                                 const Id128& memberId,
                                                 uint32_t approvedAt) {
  if (!isAdmin(groupId, adminId)) return ResultCode::Unauthorized;
  const GroupMember* member = domain_.groupMember(groupId, memberId);
  if (!member || member->state != MemberState::Replaced) {
    return ResultCode::InvalidState;
  }
  const ResultCode verified = domain_.setFriendVerification(
      memberId, VerificationState::Verified, approvedAt);
  if (verified != ResultCode::Ok) return verified;
  const GroupRecord* group = domain_.groupById(groupId);
  const ResultCode approved = domain_.setMemberState(
      groupId, memberId, MemberState::Approved, group->membershipEpoch, approvedAt);
  if (approved != ResultCode::Ok) return approved;
  return domain_.requireMemberGrant(groupId, memberId, group->membershipEpoch,
                                    approvedAt);
}

ResultCode MembershipService::transferAdministration(
    const Id128& groupId, const Id128& adminId, const Id128& nextAdminId,
    uint32_t at) {
  if (!isAdmin(groupId, adminId)) return ResultCode::Unauthorized;
  if (idsEqual(adminId, nextAdminId)) return ResultCode::Conflict;
  if (!isApprovedMember(groupId, nextAdminId)) return ResultCode::InvalidState;
  const GroupRecord* group = domain_.groupById(groupId);
  return domain_.transferAdmin(groupId, nextAdminId,
                               group->administrativeSequence + 1, at);
}

ResultCode MembershipService::proposeSuccession(
    const Id128& operationId, const Id128& groupId, const Id128& proposerId,
    const Id128& candidateId, uint32_t proposedAt, uint32_t expiresAt) {
  if (idIsZero(operationId) || idIsZero(groupId) || idIsZero(proposerId) ||
      idIsZero(candidateId)) {
    return ResultCode::InvalidId;
  }
  if (expiresAt <= proposedAt) return ResultCode::InvalidArgument;
  if (succession(operationId)) return ResultCode::Duplicate;
  if (!isApprovedMember(groupId, proposerId)) return ResultCode::Unauthorized;
  const GroupMember* earliest = earliestEligibleSuccessor(groupId);
  if (!earliest || !idsEqual(earliest->friendId, candidateId)) {
    return ResultCode::Conflict;
  }
  size_t freeIndex = kMaxSuccessionOperations;
  for (size_t i = 0; i < kMaxSuccessionOperations; ++i) {
    if (!successionUsed_[i] && freeIndex == kMaxSuccessionOperations) freeIndex = i;
    if (successionUsed_[i] && successions_[i].state == SuccessionState::Pending &&
        idsEqual(successions_[i].groupId, groupId)) {
      return ResultCode::Duplicate;
    }
  }
  if (freeIndex == kMaxSuccessionOperations) return ResultCode::CapacityReached;

  SuccessionOperation next = {};
  next.operationId = operationId;
  next.groupId = groupId;
  next.candidateId = candidateId;
  next.proposedBy = proposerId;
  next.approvals[0] = proposerId;
  next.approvalCount = 1;
  next.state = SuccessionState::Pending;
  next.proposedAt = proposedAt;
  next.expiresAt = expiresAt;
  successions_[freeIndex] = next;
  successionUsed_[freeIndex] = true;
  return ResultCode::Ok;
}

ResultCode MembershipService::approveSuccession(const Id128& operationId,
                                                const Id128& approverId,
                                                uint32_t approvedAt) {
  SuccessionOperation* operation = mutableSuccession(operationId);
  if (!operation) return ResultCode::NotFound;
  if (operation->state != SuccessionState::Pending ||
      approvedAt >= operation->expiresAt) {
    return ResultCode::InvalidState;
  }
  if (!isApprovedMember(operation->groupId, approverId)) {
    return ResultCode::Unauthorized;
  }
  for (size_t i = 0; i < operation->approvalCount; ++i) {
    if (idsEqual(operation->approvals[i], approverId)) return ResultCode::Duplicate;
  }
  if (operation->approvalCount >= kMaxGroupMembers) {
    return ResultCode::CapacityReached;
  }
  operation->approvals[operation->approvalCount++] = approverId;
  return ResultCode::Ok;
}

ResultCode MembershipService::cancelSuccession(const Id128& operationId,
                                               const Id128& actorId) {
  SuccessionOperation* operation = mutableSuccession(operationId);
  if (!operation) return ResultCode::NotFound;
  if (operation->state != SuccessionState::Pending) return ResultCode::InvalidState;
  if (!idsEqual(operation->proposedBy, actorId) &&
      !isAdmin(operation->groupId, actorId)) {
    return ResultCode::Unauthorized;
  }
  operation->state = SuccessionState::Cancelled;
  return ResultCode::Ok;
}

ResultCode MembershipService::executeSuccession(const Id128& operationId,
                                                uint32_t at) {
  SuccessionOperation* operation = mutableSuccession(operationId);
  if (!operation) return ResultCode::NotFound;
  if (operation->state != SuccessionState::Pending) return ResultCode::InvalidState;
  if (at >= operation->expiresAt) {
    operation->state = SuccessionState::Expired;
    return ResultCode::InvalidState;
  }
  const Id128 groupId = operation->groupId;
  const Id128 candidateId = operation->candidateId;
  const GroupRecord* group = domain_.groupById(groupId);
  if (!group) return ResultCode::NotFound;
  const GroupMember* earliest = earliestEligibleSuccessor(groupId);
  if (!earliest || !idsEqual(earliest->friendId, candidateId)) {
    return ResultCode::Conflict;
  }
  uint8_t approvedCount = 0;
  for (size_t i = 0; i < group->memberCount; ++i) {
    const GroupMember& member = group->members[i];
    if (!isApprovedMember(groupId, member.friendId)) continue;
    ++approvedCount;
  }
  uint8_t validApprovalCount = 0;
  for (size_t i = 0; i < operation->approvalCount; ++i) {
    if (isApprovedMember(groupId, operation->approvals[i])) {
      ++validApprovalCount;
    }
  }
  if (validApprovalCount <= approvedCount / 2) {
    return ResultCode::Unauthorized;
  }
  const ResultCode transferred = domain_.transferAdmin(
      groupId, candidateId, group->administrativeSequence + 1, at);
  if (transferred != ResultCode::Ok) return transferred;
  operation->state = SuccessionState::Executed;
  operation->executedAt = at;
  return ResultCode::Ok;
}

ResultCode MembershipService::disband(const Id128& groupId,
                                      const Id128& adminId, uint32_t at) {
  if (!isAdmin(groupId, adminId)) return ResultCode::Unauthorized;
  const GroupRecord* group = domain_.groupById(groupId);
  return domain_.disbandGroup(groupId, group->membershipEpoch + 1, at);
}

}  // namespace friendmesh
