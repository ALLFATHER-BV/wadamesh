#include <stdio.h>
#include <string.h>

#include "friendmesh/FriendMeshFeatureService.h"
#include "friendmesh/app/FriendMeshDevelopmentRuntime.h"
#include "friendmesh/chat/FriendMeshChat.h"
#include "friendmesh/chat/FriendMeshDevelopmentStorage.h"
#include "friendmesh/chat/FriendMeshFragmentation.h"
#include "friendmesh/chat/FriendMeshSync.h"
#include "friendmesh/core/FriendMeshDomain.h"
#include "friendmesh/core/FriendMeshCoordinator.h"
#include "friendmesh/core/FriendMeshEventHistory.h"
#include "friendmesh/core/FriendMeshFeatureModels.h"
#include "friendmesh/core/FriendMeshOutbox.h"
#include "friendmesh/navigation/FriendMeshNavigation.h"
#include "friendmesh/navigation/FriendMeshGroupCoordination.h"
#include "friendmesh/navigation/FriendMeshMeshCorePositionAdapter.h"
#include "friendmesh/people/FriendMeshBlePresence.h"
#include "friendmesh/people/FriendMeshChannelInvite.h"
#include "friendmesh/people/FriendMeshFriendRequest.h"
#include "friendmesh/people/FriendMeshChannelRoster.h"
#include "friendmesh/people/FriendMeshMembership.h"
#include "friendmesh/people/FriendMeshTrustedContacts.h"
#include "friendmesh/safety/FriendMeshSafety.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);     \
      ++failures;                                                               \
    }                                                                           \
  } while (0)

friendmesh::Id128 makeId(uint8_t seed) {
  friendmesh::Id128 id = {};
  for (size_t i = 0; i < friendmesh::kIdSize; ++i) {
    id.bytes[i] = static_cast<uint8_t>(seed + i);
  }
  return id;
}

friendmesh::FriendRecord makeFriend(uint8_t seed, const char* label) {
  friendmesh::FriendRecord record = {};
  record.id = makeId(seed);
  record.identity.meshIdentity = makeId(static_cast<uint8_t>(seed + 40));
  record.identity.signingIdentity = makeId(static_cast<uint8_t>(seed + 80));
  friendmesh::copyBoundedText(record.label, sizeof(record.label), label,
                              friendmesh::kMaxFriendLabelBytes);
  record.verification = friendmesh::VerificationState::Verified;
  return record;
}

friendmesh::EventHeader makeEvent(uint8_t seed, friendmesh::EventType type,
                                  uint32_t createdAt, uint32_t expiresAt = 0) {
  friendmesh::EventHeader event = {};
  event.eventId = makeId(seed);
  event.groupId = makeId(120);
  event.senderId = makeId(static_cast<uint8_t>(seed + 30));
  event.type = type;
  event.priority = friendmesh::policyForEvent(type).priority;
  event.membershipEpoch = 1;
  event.senderSequence = seed;
  event.createdAt = createdAt;
  event.expiresAt = expiresAt;
  return event;
}

friendmesh::EventHeader makeScopedEvent(
    uint8_t eventSeed, friendmesh::EventType type,
    const friendmesh::Id128& groupId, const friendmesh::Id128& senderId,
    uint32_t sequence, uint32_t payloadHandle, uint32_t payloadLength,
    uint32_t createdAt, uint32_t expiresAt = 0) {
  friendmesh::EventHeader event = {};
  event.eventId = makeId(eventSeed);
  event.groupId = groupId;
  event.senderId = senderId;
  event.type = type;
  event.priority = friendmesh::policyForEvent(type).priority;
  event.membershipEpoch = 1;
  event.senderSequence = sequence;
  event.createdAt = createdAt;
  event.expiresAt = expiresAt;
  event.payloadHandle = payloadHandle;
  event.payloadLength = payloadLength;
  for (size_t i = 0; i < sizeof(event.payloadHash); ++i) {
    event.payloadHash[i] = static_cast<uint8_t>(eventSeed + i);
  }
  return event;
}

friendmesh::PositionRecord makePosition(const friendmesh::Id128& subject,
                                        int32_t latitudeE7,
                                        int32_t longitudeE7,
                                        uint32_t capturedAt) {
  friendmesh::PositionRecord position = {};
  position.subjectId = subject;
  position.latitudeE7 = latitudeE7;
  position.longitudeE7 = longitudeE7;
  position.capturedAt = capturedAt;
  position.receivedAt = capturedAt;
  position.accuracyMeters = 5;
  position.source = friendmesh::PositionSource::OnDeviceGps;
  position.valid = true;
  return position;
}

class FakeContactDirectory : public friendmesh::ExistingContactDirectory {
 public:
  FakeContactDirectory(const friendmesh::Id128& identity, const char* label,
                       bool available)
      : snapshot_{} {
    snapshot_.meshIdentity = identity;
    friendmesh::copyBoundedText(snapshot_.label, sizeof(snapshot_.label), label,
                                friendmesh::kMaxFriendLabelBytes);
    snapshot_.lastAuthenticatedAt = 25;
    snapshot_.available = available;
  }

  friendmesh::ResultCode lookup(
      const friendmesh::Id128& meshIdentity,
      friendmesh::ExistingContactSnapshot& snapshot) const override {
    if (!friendmesh::idsEqual(meshIdentity, snapshot_.meshIdentity)) {
      return friendmesh::ResultCode::NotFound;
    }
    snapshot = snapshot_;
    return friendmesh::ResultCode::Ok;
  }

 private:
  friendmesh::ExistingContactSnapshot snapshot_;
};

void testCoreTypes() {
  friendmesh::Id128 zero = {};
  CHECK(friendmesh::idIsZero(zero));
  CHECK(!friendmesh::idIsZero(makeId(1)));
  CHECK(friendmesh::idsEqual(makeId(7), makeId(7)));
  CHECK(friendmesh::compareIds(makeId(7), makeId(8)) < 0);

  char text[8] = {};
  CHECK(friendmesh::copyBoundedText(text, sizeof(text), "Friend", 7));
  CHECK(strcmp(text, "Friend") == 0);
  CHECK(!friendmesh::copyBoundedText(text, sizeof(text), "TooLong!", 7));
  CHECK(friendmesh::boundedTextEqualFolded("ALICE", "alice"));
  CHECK(!friendmesh::boundedTextEqualFolded("alice", "alicia"));
}

void testEveryFeatureFamilyHasPolicy() {
  bool families[8] = {};
  for (uint8_t raw = 0; raw < static_cast<uint8_t>(friendmesh::EventType::Count); ++raw) {
    const friendmesh::EventType type = static_cast<friendmesh::EventType>(raw);
    CHECK(friendmesh::eventTypeIsValid(type));
    const friendmesh::EventPolicy policy = friendmesh::policyForEvent(type);
    families[static_cast<uint8_t>(policy.family)] = true;
    CHECK(policy.requiresSignature);
    if (policy.requiresGroup) {
      friendmesh::EventHeader event = makeEvent(static_cast<uint8_t>(raw + 1), type, 10);
      event.groupId = {};
      CHECK(friendmesh::validateEventHeader(event) == friendmesh::ResultCode::InvalidId);
    }
  }
  for (size_t i = 0; i < sizeof(families); ++i) CHECK(families[i]);

  const friendmesh::EventPolicy sos =
      friendmesh::policyForEvent(friendmesh::EventType::SosOpened);
  CHECK(sos.family == friendmesh::FeatureFamily::Safety);
  CHECK(sos.priority == friendmesh::EventPriority::Safety);
  CHECK(sos.durability == friendmesh::DurabilityRule::SafetyMayBypassPersistence);

  const friendmesh::EventPolicy chat =
      friendmesh::policyForEvent(friendmesh::EventType::ChatMessage);
  CHECK(chat.family == friendmesh::FeatureFamily::ChatAndHistory);
  CHECK(chat.durability == friendmesh::DurabilityRule::PersistBeforeTransmit);
  CHECK(chat.requiresApprovedMember);
}

void testSharedFeatureModels() {
  friendmesh::ChatRecord chat = {};
  chat.eventId = makeId(1);
  chat.delivery = friendmesh::DeliveryState::Queued;
  CHECK(chat.delivery == friendmesh::DeliveryState::Queued);

  friendmesh::PositionRecord position = {};
  position.subjectId = makeId(2);
  position.latitudeE7 = 377749000;
  position.longitudeE7 = -1224194000;
  position.source = friendmesh::PositionSource::OnDeviceGps;
  position.valid = true;

  friendmesh::NavigationState navigation = {};
  navigation.targetId = position.subjectId;
  navigation.targetPosition = position;
  navigation.headingSource = friendmesh::HeadingSource::NorthUp;
  navigation.active = true;
  CHECK(navigation.targetPosition.valid);

  friendmesh::MarkerRecord marker = {};
  marker.markerId = makeId(3);
  marker.type = friendmesh::MarkerType::Resource;
  marker.position = position;
  CHECK(marker.position.latitudeE7 == position.latitudeE7);

  friendmesh::MeetupRecord meetup = {};
  meetup.meetupId = makeId(4);
  meetup.state = friendmesh::MeetupState::Proposed;
  meetup.yesVotes = 1;
  CHECK(meetup.yesVotes == 1);

  friendmesh::IncidentRecord incident = {};
  incident.incidentId = makeId(5);
  incident.kind = friendmesh::IncidentKind::Sos;
  incident.state = friendmesh::IncidentState::Active;
  incident.persistenceFailed = true;
  CHECK(incident.persistenceFailed);
}

void testDomainStore() {
  friendmesh::DomainStore store;
  const friendmesh::FriendRecord alice = makeFriend(1, "Alice");
  const friendmesh::FriendRecord bob = makeFriend(2, "Bob");
  const friendmesh::FriendRecord charlie = makeFriend(3, "Charlie");

  CHECK(store.addFriend(alice) == friendmesh::ResultCode::Ok);
  CHECK(store.addFriend(bob) == friendmesh::ResultCode::Ok);
  CHECK(store.addFriend(charlie) == friendmesh::ResultCode::Ok);
  CHECK(store.addFriend(alice) == friendmesh::ResultCode::Duplicate);
  CHECK(store.friendCount() == 3);

  const friendmesh::Id128 groupId = makeId(120);
  CHECK(store.createGroup(groupId, "Trail Friends", alice.id, "Alice", 100) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.renameGroup(groupId, "Trail Crew", 105) ==
        friendmesh::ResultCode::Ok);
  CHECK(strcmp(store.groupById(groupId)->name, "Trail Crew") == 0);
  CHECK(store.groupCount() == 1);
  CHECK(store.addMember(groupId, bob.id, "Bob", friendmesh::MemberRole::Member,
                        friendmesh::MemberState::Approved, 1, 110) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.addMember(groupId, charlie.id, "bOb", friendmesh::MemberRole::Member,
                        friendmesh::MemberState::Approved, 1, 111) ==
        friendmesh::ResultCode::Duplicate);
  CHECK(store.addMember(groupId, charlie.id, "Charlie", friendmesh::MemberRole::Admin,
                        friendmesh::MemberState::Approved, 1, 112) ==
        friendmesh::ResultCode::Conflict);
  CHECK(store.addMember(groupId, charlie.id, "Charlie", friendmesh::MemberRole::Member,
                        friendmesh::MemberState::Approved, 1, 112) ==
        friendmesh::ResultCode::Ok);

  CHECK(store.transferAdmin(groupId, bob.id, 3, 120) == friendmesh::ResultCode::Ok);
  CHECK(store.transferAdmin(groupId, charlie.id, 5, 121) ==
        friendmesh::ResultCode::Conflict);
  CHECK(store.setMemberState(groupId, bob.id, friendmesh::MemberState::RekeyPending,
                             2, 130) == friendmesh::ResultCode::Ok);
  const friendmesh::GroupRecord* group = store.groupById(groupId);
  CHECK(group != nullptr);
  CHECK(group && group->membershipEpoch == 2);
  CHECK(group && group->memberCount == 3);
  CHECK(store.setFriendBlocked(bob.id, true) == friendmesh::ResultCode::Ok);
  CHECK(store.friendById(bob.id)->blockedLocally);
}

void testDevelopmentIdentityLifecycle() {
  friendmesh::DevelopmentIdentityLifecycle identity;
  friendmesh::IdentityReference first = {};
  first.meshIdentity = makeId(40);
  first.signingIdentity = makeId(80);
  friendmesh::IdentityReference replacement = {};
  replacement.meshIdentity = makeId(41);
  replacement.signingIdentity = makeId(81);

  CHECK(identity.state() == friendmesh::DevelopmentIdentityState::Empty);
  CHECK(identity.replace(first) == friendmesh::ResultCode::InvalidState);
  CHECK(identity.create(first) == friendmesh::ResultCode::Ok);
  CHECK(identity.generation() == 1);
  CHECK(identity.create(first) == friendmesh::ResultCode::InvalidState);
  CHECK(identity.replace(first) == friendmesh::ResultCode::Conflict);
  CHECK(identity.replace(replacement) == friendmesh::ResultCode::Ok);
  CHECK(identity.state() == friendmesh::DevelopmentIdentityState::Replaced);
  CHECK(identity.generation() == 2);
  identity.clear();
  CHECK(identity.state() == friendmesh::DevelopmentIdentityState::Empty);
  CHECK(friendmesh::idIsZero(identity.identity().meshIdentity));
}

void testTrustedContactRepository() {
  friendmesh::DomainStore store;
  const friendmesh::Id128 meshIdentity = makeId(60);
  FakeContactDirectory directory(meshIdentity, "Existing MeshCore Friend", true);
  friendmesh::TrustedContactRepository trusted(store, directory);
  const friendmesh::Id128 friendId = makeId(10);
  const friendmesh::Id128 signingIdentity = makeId(100);

  CHECK(trusted.importCandidate(friendId, makeId(61), signingIdentity, false) ==
        friendmesh::ResultCode::NotFound);
  CHECK(trusted.importCandidate(friendId, meshIdentity, signingIdentity, true) ==
        friendmesh::ResultCode::Ok);
  CHECK(trusted.byId(friendId) != nullptr);
  CHECK(trusted.byId(friendId)->verification ==
        friendmesh::VerificationState::Pending);
  CHECK(trusted.byId(friendId)->emergencyContactAllowed);
  CHECK(strcmp(trusted.byId(friendId)->label, "Existing MeshCore Friend") == 0);
  CHECK(trusted.verify(friendId, 30) == friendmesh::ResultCode::Ok);
  CHECK(trusted.byId(friendId)->firstVerifiedAt == 30);
  CHECK(trusted.setBlocked(friendId, true) == friendmesh::ResultCode::Ok);
  CHECK(trusted.byId(friendId)->blockedLocally);
  CHECK(trusted.importCandidate(friendId, meshIdentity, signingIdentity, true) ==
        friendmesh::ResultCode::Duplicate);
}

friendmesh::VerificationTranscript transcriptFor(
    const friendmesh::FriendRecord& candidate) {
  friendmesh::VerificationTranscript transcript = {};
  transcript.candidateIdentity = candidate.identity;
  transcript.comparisonNumber = 412773;
  transcript.proof = friendmesh::DevelopmentProofState::ReferencesMatched;
  return transcript;
}

friendmesh::DirectPathObservation directObservation(uint8_t seed,
                                                    uint32_t observedAt,
                                                    uint8_t hopCount = 0,
                                                    bool forwardingDisabled = true) {
  friendmesh::DirectPathObservation observation = {};
  observation.exchangeId = makeId(seed);
  observation.ephemeralPublicReference = makeId(static_cast<uint8_t>(seed + 1));
  observation.state = friendmesh::DirectPathState::Observed;
  observation.observedHopCount = hopCount;
  observation.forwardingDisabled = forwardingDisabled;
  observation.observedAt = observedAt;
  return observation;
}

void testMembershipLifecycle() {
  friendmesh::DomainStore store;
  const friendmesh::FriendRecord alice = makeFriend(1, "Alice");
  const friendmesh::FriendRecord bob = makeFriend(2, "Bob");
  const friendmesh::FriendRecord charlie = makeFriend(3, "Charlie");
  const friendmesh::FriendRecord dana = makeFriend(4, "Dana");
  CHECK(store.addFriend(alice) == friendmesh::ResultCode::Ok);
  CHECK(store.addFriend(bob) == friendmesh::ResultCode::Ok);
  CHECK(store.addFriend(charlie) == friendmesh::ResultCode::Ok);
  CHECK(store.addFriend(dana) == friendmesh::ResultCode::Ok);

  const friendmesh::Id128 trailGroup = makeId(120);
  CHECK(store.createGroup(trailGroup, "Trail Friends", alice.id, "Alice", 10) ==
        friendmesh::ResultCode::Ok);
  friendmesh::MembershipService membership(store);
  const friendmesh::JoinPathPolicy anyPath =
      friendmesh::JoinPathPolicy::AnyPathWithAdminApproval;

  const friendmesh::Id128 invitation1 = makeId(130);
  CHECK(membership.openInvitation(invitation1, trailGroup, alice.id, "abc123", 10,
                                  100, anyPath) == friendmesh::ResultCode::InvalidArgument);
  CHECK(membership.openInvitation(invitation1, trailGroup, bob.id, "ABC123", 10,
                                  100, anyPath) == friendmesh::ResultCode::Unauthorized);
  CHECK(membership.openInvitation(invitation1, trailGroup, alice.id, "ABC123", 10,
                                  100, anyPath) == friendmesh::ResultCode::Ok);
  CHECK(membership.invitationByCode(trailGroup, "ABC123") != nullptr);
  CHECK(membership.invitationByCode(trailGroup, "abc123") == nullptr);
  CHECK(membership.openInvitation(makeId(131), trailGroup, alice.id, "ABC123", 11,
                                  100, anyPath) == friendmesh::ResultCode::Duplicate);

  friendmesh::VerificationTranscript bobTranscript = transcriptFor(bob);
  friendmesh::VerificationTranscript rejectedTranscript = bobTranscript;
  rejectedTranscript.proof = friendmesh::DevelopmentProofState::Rejected;
  const friendmesh::Id128 bobRequest = makeId(140);
  CHECK(membership.submitJoinRequest(bobRequest, invitation1, bob.id, "Bob",
                                     rejectedTranscript, 20, 90) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.submitJoinRequest(bobRequest, invitation1, bob.id, "Bob",
                                     bobTranscript, 20, 110) ==
        friendmesh::ResultCode::InvalidArgument);
  CHECK(store.setFriendBlocked(bob.id, true) == friendmesh::ResultCode::Ok);
  CHECK(membership.submitJoinRequest(bobRequest, invitation1, bob.id, "Bob",
                                     bobTranscript, 20, 90) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(store.setFriendBlocked(bob.id, false) == friendmesh::ResultCode::Ok);
  CHECK(membership.submitJoinRequest(bobRequest, invitation1, bob.id, "Bob",
                                     bobTranscript, 20, 90) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.approveJoinRequest(bobRequest, bob.id, 30) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.approveJoinRequest(bobRequest, alice.id, 30) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.joinRequest(bobRequest)->state ==
        friendmesh::JoinRequestState::Approved);
  CHECK(store.groupMember(trailGroup, bob.id)->state ==
        friendmesh::MemberState::Approved);
  CHECK(store.friendById(bob.id)->verification ==
        friendmesh::VerificationState::Verified);

  CHECK(membership.closeInvitation(invitation1, dana.id) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.closeInvitation(invitation1, bob.id) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.closeInvitation(invitation1, alice.id) ==
        friendmesh::ResultCode::Ok);

  const friendmesh::Id128 expiringInvitation = makeId(132);
  const friendmesh::Id128 expiringRequest = makeId(141);
  CHECK(membership.openInvitation(expiringInvitation, trailGroup, bob.id, "EXP123",
                                  40, 60, anyPath) == friendmesh::ResultCode::Ok);
  CHECK(membership.submitJoinRequest(expiringRequest, expiringInvitation, charlie.id,
                                     "Charlie", transcriptFor(charlie), 41, 59) ==
        friendmesh::ResultCode::Ok);
  membership.expireInvitationsAndRequests(60);
  CHECK(membership.invitation(expiringInvitation)->state ==
        friendmesh::InvitationState::Expired);
  CHECK(membership.joinRequest(expiringRequest)->state ==
        friendmesh::JoinRequestState::Expired);

  const friendmesh::Id128 activeInvitation = makeId(133);
  CHECK(membership.openInvitation(activeInvitation, trailGroup, alice.id, "JOIN99",
                                  61, 200, anyPath) == friendmesh::ResultCode::Ok);
  const friendmesh::Id128 rejectedRequest = makeId(142);
  CHECK(membership.submitJoinRequest(rejectedRequest, activeInvitation, charlie.id,
                                     "Charlie", transcriptFor(charlie), 62, 190) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.rejectJoinRequest(rejectedRequest, alice.id, 63) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.joinRequest(rejectedRequest)->state ==
        friendmesh::JoinRequestState::Rejected);
  const friendmesh::Id128 cancelledRequest = makeId(143);
  CHECK(membership.submitJoinRequest(cancelledRequest, activeInvitation, charlie.id,
                                     "Charlie", transcriptFor(charlie), 64, 190) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.cancelJoinRequest(cancelledRequest, dana.id) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.cancelJoinRequest(cancelledRequest, charlie.id) ==
        friendmesh::ResultCode::Ok);
  const friendmesh::Id128 charlieRequest = makeId(144);
  CHECK(membership.submitJoinRequest(charlieRequest, activeInvitation, charlie.id,
                                     "Charlie", transcriptFor(charlie), 65, 190) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.approveJoinRequest(charlieRequest, alice.id, 66) ==
        friendmesh::ResultCode::Ok);

  const friendmesh::Id128 directInvitation = makeId(134);
  const friendmesh::Id128 directRequest = makeId(145);
  CHECK(membership.openInvitation(directInvitation, trailGroup, alice.id,
                                  "NEAR01", 70, 300) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.submitJoinRequest(directRequest, directInvitation, dana.id,
                                     "Dana", transcriptFor(dana), 190, 250) ==
        friendmesh::ResultCode::Unauthorized);
  friendmesh::DirectPathObservation relayed = directObservation(170, 180, 1);
  CHECK(membership.submitJoinRequest(directRequest, directInvitation, dana.id,
                                     "Dana", transcriptFor(dana), 190, 250,
                                     &relayed) == friendmesh::ResultCode::Unauthorized);
  friendmesh::DirectPathObservation forwardingAllowed =
      directObservation(172, 180, 0, false);
  CHECK(membership.submitJoinRequest(directRequest, directInvitation, dana.id,
                                     "Dana", transcriptFor(dana), 190, 250,
                                     &forwardingAllowed) ==
        friendmesh::ResultCode::Unauthorized);
  friendmesh::DirectPathObservation stale = directObservation(174, 69);
  CHECK(membership.submitJoinRequest(directRequest, directInvitation, dana.id,
                                     "Dana", transcriptFor(dana), 190, 250,
                                     &stale) == friendmesh::ResultCode::Unauthorized);
  friendmesh::DirectPathObservation direct = directObservation(176, 180);
  CHECK(membership.submitJoinRequest(directRequest, directInvitation, dana.id,
                                     "Dana", transcriptFor(dana), 190, 250,
                                     &direct) == friendmesh::ResultCode::Ok);
  CHECK(membership.joinRequest(directRequest)->directPath.observedHopCount == 0);
  CHECK(membership.rejectJoinRequest(directRequest, alice.id, 191) ==
        friendmesh::ResultCode::Ok);

  const friendmesh::Id128 leaveOperation = makeId(150);
  CHECK(membership.requestLeave(makeId(149), trailGroup, alice.id, 70, 80) ==
        friendmesh::ResultCode::Conflict);
  CHECK(membership.requestLeave(leaveOperation, trailGroup, bob.id, 70, 80) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.groupMember(trailGroup, bob.id)->state ==
        friendmesh::MemberState::LeavePending);
  CHECK(membership.cancelRemoval(leaveOperation, dana.id, 75) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.cancelRemoval(leaveOperation, bob.id, 75) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.groupMember(trailGroup, bob.id)->state ==
        friendmesh::MemberState::Approved);

  const friendmesh::Id128 kickOperation = makeId(151);
  CHECK(membership.requestKick(kickOperation, trailGroup, bob.id, charlie.id, 81,
                               90) == friendmesh::ResultCode::Unauthorized);
  CHECK(membership.requestKick(kickOperation, trailGroup, alice.id, bob.id, 81,
                               90) == friendmesh::ResultCode::Ok);
  CHECK(membership.cancelRemoval(kickOperation, bob.id, 85) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.advanceRemoval(kickOperation, 89) ==
        friendmesh::ResultCode::InvalidState);
  CHECK(membership.advanceRemoval(kickOperation, 90) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.removal(kickOperation)->state ==
        friendmesh::RemovalState::RekeyRequired);
  CHECK(store.groupById(trailGroup)->security ==
        friendmesh::GroupSecurityState::RekeyPending);
  const friendmesh::Id128 overlappingKick = makeId(152);
  CHECK(membership.requestKick(overlappingKick, trailGroup, alice.id, charlie.id,
                               90, 95) == friendmesh::ResultCode::Ok);
  CHECK(membership.advanceRemoval(overlappingKick, 95) ==
        friendmesh::ResultCode::InvalidState);
  CHECK(membership.cancelRemoval(overlappingKick, alice.id, 95) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.commitRemoval(kickOperation, 91) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.groupMember(trailGroup, bob.id)->state ==
        friendmesh::MemberState::Removed);
  const uint32_t removalEpoch = store.groupById(trailGroup)->membershipEpoch;
  friendmesh::IdentityReference replacement = {};
  replacement.meshIdentity = makeId(200);
  replacement.signingIdentity = makeId(220);
  CHECK(membership.replaceMemberIdentity(trailGroup, charlie.id, charlie.id,
                                         replacement, 92) ==
        friendmesh::ResultCode::InvalidState);
  CHECK(store.friendById(charlie.id)->replacementCount == 0);
  CHECK(store.groupMember(trailGroup, bob.id)->grantState ==
        friendmesh::GrantState::Revoked);
  CHECK(store.groupMember(trailGroup, alice.id)->grantState ==
        friendmesh::GrantState::Pending);
  CHECK(membership.completeRekey(trailGroup, alice.id, removalEpoch, 92) ==
        friendmesh::ResultCode::Conflict);
  CHECK(membership.activateGrant(trailGroup, alice.id, alice.id, removalEpoch, 92) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.activateGrant(trailGroup, alice.id, charlie.id, removalEpoch,
                                 93) == friendmesh::ResultCode::Ok);
  CHECK(membership.completeRekey(trailGroup, alice.id, removalEpoch, 94) ==
        friendmesh::ResultCode::Ok);

  CHECK(membership.replaceMemberIdentity(trailGroup, alice.id, alice.id,
                                         replacement, 100) ==
        friendmesh::ResultCode::Conflict);
  CHECK(membership.replaceMemberIdentity(trailGroup, bob.id, charlie.id,
                                         replacement, 100) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.replaceMemberIdentity(trailGroup, charlie.id, charlie.id,
                                         replacement, 100) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.friendById(charlie.id)->replacementCount == 1);
  CHECK(store.groupMember(trailGroup, charlie.id)->state ==
        friendmesh::MemberState::Replaced);
  CHECK(membership.approveReplacement(trailGroup, alice.id, charlie.id, 101) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.groupMember(trailGroup, charlie.id)->state ==
        friendmesh::MemberState::Approved);
  const uint32_t replacementEpoch = store.groupById(trailGroup)->membershipEpoch;
  CHECK(store.groupMember(trailGroup, charlie.id)->grantState ==
        friendmesh::GrantState::Pending);
  CHECK(membership.activateGrant(trailGroup, alice.id, alice.id, replacementEpoch,
                                 102) == friendmesh::ResultCode::Ok);
  CHECK(membership.activateGrant(trailGroup, alice.id, charlie.id,
                                 replacementEpoch, 103) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.completeRekey(trailGroup, alice.id, replacementEpoch, 104) ==
        friendmesh::ResultCode::Ok);

  CHECK(membership.transferAdministration(trailGroup, bob.id, charlie.id, 110) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.transferAdministration(trailGroup, alice.id, charlie.id, 110) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.groupMember(trailGroup, charlie.id)->role ==
        friendmesh::MemberRole::Admin);
  CHECK(membership.disband(trailGroup, alice.id, 120) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.disband(trailGroup, charlie.id, 120) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.groupMember(trailGroup, alice.id)->state ==
        friendmesh::MemberState::Disbanded);

  const friendmesh::Id128 campGroup = makeId(121);
  CHECK(store.createGroup(campGroup, "Camp Friends", alice.id, "Alice", 130) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.addMember(campGroup, bob.id, "Bob", friendmesh::MemberRole::Member,
                        friendmesh::MemberState::Approved, 1, 131) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.addMember(campGroup, dana.id, "Dana", friendmesh::MemberRole::Member,
                        friendmesh::MemberState::Approved, 1, 132) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.proposeSuccession(makeId(160), campGroup, dana.id, dana.id,
                                     140, 150) ==
        friendmesh::ResultCode::Conflict);
  const friendmesh::Id128 cancelledSuccession = makeId(161);
  CHECK(membership.proposeSuccession(cancelledSuccession, campGroup, dana.id,
                                     bob.id, 140, 150) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.cancelSuccession(cancelledSuccession, makeId(250)) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.cancelSuccession(cancelledSuccession, dana.id) ==
        friendmesh::ResultCode::Ok);
  const friendmesh::Id128 expiredSuccession = makeId(162);
  CHECK(membership.proposeSuccession(expiredSuccession, campGroup, dana.id,
                                     bob.id, 141, 145) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.executeSuccession(expiredSuccession, 145) ==
        friendmesh::ResultCode::InvalidState);
  CHECK(membership.succession(expiredSuccession)->state ==
        friendmesh::SuccessionState::Expired);
  const friendmesh::Id128 succession = makeId(163);
  CHECK(membership.proposeSuccession(succession, campGroup, dana.id, bob.id, 146,
                                     160) == friendmesh::ResultCode::Ok);
  CHECK(membership.approveSuccession(succession, dana.id, 147) ==
        friendmesh::ResultCode::Duplicate);
  CHECK(membership.executeSuccession(succession, 148) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(membership.approveSuccession(succession, alice.id, 149) ==
        friendmesh::ResultCode::Ok);
  CHECK(store.setFriendBlocked(alice.id, true) == friendmesh::ResultCode::Ok);
  CHECK(membership.executeSuccession(succession, 150) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(store.setFriendBlocked(alice.id, false) == friendmesh::ResultCode::Ok);
  CHECK(membership.executeSuccession(succession, 150) ==
        friendmesh::ResultCode::Ok);
  CHECK(membership.succession(succession)->state ==
        friendmesh::SuccessionState::Executed);
  CHECK(store.groupMember(campGroup, bob.id)->role ==
        friendmesh::MemberRole::Admin);
}

void testMembershipBounds() {
  friendmesh::DomainStore groups;
  friendmesh::FriendRecord friends[10] = {};
  for (size_t i = 0; i < 10; ++i) {
    char label[12] = {};
    snprintf(label, sizeof(label), "Friend%u", static_cast<unsigned>(i));
    friends[i] = makeFriend(static_cast<uint8_t>(10 + i), label);
    CHECK(groups.addFriend(friends[i]) == friendmesh::ResultCode::Ok);
  }
  for (size_t i = 0; i < friendmesh::kMaxGroups; ++i) {
    CHECK(groups.createGroup(makeId(static_cast<uint8_t>(180 + i)), "Bounded",
                             friends[0].id, "Admin", 10) ==
          friendmesh::ResultCode::Ok);
  }
  CHECK(groups.createGroup(makeId(199), "Too Many", friends[0].id, "Admin", 10) ==
        friendmesh::ResultCode::CapacityReached);

  const friendmesh::Id128 firstGroup = makeId(180);
  for (size_t i = 1; i < friendmesh::kMaxGroupMembers; ++i) {
    char alias[12] = {};
    snprintf(alias, sizeof(alias), "Member%u", static_cast<unsigned>(i));
    CHECK(groups.addMember(firstGroup, friends[i].id, alias,
                           friendmesh::MemberRole::Member,
                           friendmesh::MemberState::Approved, 1, 20) ==
          friendmesh::ResultCode::Ok);
  }
  CHECK(groups.addMember(firstGroup, friends[8].id, "Overflow",
                         friendmesh::MemberRole::Member,
                         friendmesh::MemberState::Approved, 1, 21) ==
        friendmesh::ResultCode::CapacityReached);

  friendmesh::DomainStore invitationDomain;
  const friendmesh::FriendRecord admin = makeFriend(30, "Admin");
  CHECK(invitationDomain.addFriend(admin) == friendmesh::ResultCode::Ok);
  const friendmesh::Id128 invitationGroup = makeId(210);
  CHECK(invitationDomain.createGroup(invitationGroup, "Invitations", admin.id,
                                     "Admin", 1) == friendmesh::ResultCode::Ok);
  friendmesh::MembershipService invitations(invitationDomain);
  const friendmesh::JoinPathPolicy anyPath =
      friendmesh::JoinPathPolicy::AnyPathWithAdminApproval;
  for (size_t i = 0; i < friendmesh::kMaxOpenInvitations; ++i) {
    char code[friendmesh::kInvitationCodeLength + 1] = {};
    snprintf(code, sizeof(code), "I%05u", static_cast<unsigned>(i));
    CHECK(invitations.openInvitation(makeId(static_cast<uint8_t>(220 + i)),
                                     invitationGroup, admin.id, code, 10, 100,
                                     anyPath) ==
          friendmesh::ResultCode::Ok);
  }
  CHECK(invitations.openInvitation(makeId(240), invitationGroup, admin.id,
                                   "FULL99", 10, 100, anyPath) ==
        friendmesh::ResultCode::CapacityReached);

  friendmesh::DomainStore requestDomain;
  const friendmesh::FriendRecord requestAdmin = makeFriend(50, "RequestAdmin");
  CHECK(requestDomain.addFriend(requestAdmin) == friendmesh::ResultCode::Ok);
  friendmesh::FriendRecord candidates[friendmesh::kMaxJoinRequests + 1] = {};
  for (size_t i = 0; i < friendmesh::kMaxJoinRequests + 1; ++i) {
    char label[12] = {};
    snprintf(label, sizeof(label), "Join%02u", static_cast<unsigned>(i));
    candidates[i] = makeFriend(static_cast<uint8_t>(70 + i), label);
    CHECK(requestDomain.addFriend(candidates[i]) == friendmesh::ResultCode::Ok);
  }
  const friendmesh::Id128 requestGroup = makeId(200);
  CHECK(requestDomain.createGroup(requestGroup, "Requests", requestAdmin.id,
                                  "Admin", 1) == friendmesh::ResultCode::Ok);
  friendmesh::MembershipService requests(requestDomain);
  const friendmesh::Id128 requestInvitation = makeId(201);
  CHECK(requests.openInvitation(requestInvitation, requestGroup, requestAdmin.id,
                                "REQ123", 2, 100, anyPath) ==
        friendmesh::ResultCode::Ok);
  for (size_t i = 0; i < friendmesh::kMaxJoinRequests; ++i) {
    char alias[12] = {};
    snprintf(alias, sizeof(alias), "Join%02u", static_cast<unsigned>(i));
    CHECK(requests.submitJoinRequest(
              makeId(static_cast<uint8_t>(100 + i)), requestInvitation,
              candidates[i].id, alias, transcriptFor(candidates[i]), 3, 90) ==
          friendmesh::ResultCode::Ok);
  }
  CHECK(requests.submitJoinRequest(makeId(119), requestInvitation,
                                   candidates[friendmesh::kMaxJoinRequests].id,
                                   "Overflow", transcriptFor(candidates[16]), 3,
                                   90) == friendmesh::ResultCode::CapacityReached);

  friendmesh::DomainStore removalDomain;
  const friendmesh::FriendRecord removalAdmin = makeFriend(90, "RemovalAdmin");
  const friendmesh::FriendRecord removalMember = makeFriend(91, "RemovalMember");
  CHECK(removalDomain.addFriend(removalAdmin) == friendmesh::ResultCode::Ok);
  CHECK(removalDomain.addFriend(removalMember) == friendmesh::ResultCode::Ok);
  const friendmesh::Id128 removalGroup = makeId(190);
  CHECK(removalDomain.createGroup(removalGroup, "Removals", removalAdmin.id,
                                  "Admin", 1) == friendmesh::ResultCode::Ok);
  CHECK(removalDomain.addMember(removalGroup, removalMember.id, "Member",
                                friendmesh::MemberRole::Member,
                                friendmesh::MemberState::Approved, 1, 2) ==
        friendmesh::ResultCode::Ok);
  friendmesh::MembershipService removals(removalDomain);
  for (size_t i = 0; i < friendmesh::kMaxRemovalOperations; ++i) {
    const friendmesh::Id128 operation = makeId(static_cast<uint8_t>(130 + i));
    CHECK(removals.requestLeave(operation, removalGroup, removalMember.id, 10,
                                20) == friendmesh::ResultCode::Ok);
    CHECK(removals.cancelRemoval(operation, removalMember.id, 11) ==
          friendmesh::ResultCode::Ok);
  }
  CHECK(removals.requestLeave(makeId(170), removalGroup, removalMember.id, 10,
                              20) == friendmesh::ResultCode::CapacityReached);
}

void testDevelopmentChatStorage() {
  friendmesh::DevelopmentPayloadStorage storage;
  const uint8_t payload[] = {1, 2, 3, 4};
  for (uint32_t handle = 1;
       handle <= friendmesh::kDevelopmentInternalPayloadRecords; ++handle) {
    CHECK(storage.put(handle, payload, sizeof(payload)) ==
          friendmesh::ResultCode::Ok);
    CHECK(storage.placement(handle) == friendmesh::PayloadPlacement::Internal);
  }
  CHECK(storage.put(5, payload, sizeof(payload)) == friendmesh::ResultCode::Ok);
  CHECK(storage.placement(5) == friendmesh::PayloadPlacement::Expansion);
  uint8_t loaded[8] = {};
  size_t loadedLength = 0;
  CHECK(storage.get(5, loaded, sizeof(loaded), loadedLength) ==
        friendmesh::ResultCode::Ok);
  CHECK(loadedLength == sizeof(payload));
  CHECK(memcmp(loaded, payload, sizeof(payload)) == 0);
  CHECK(storage.put(5, payload, sizeof(payload)) ==
        friendmesh::ResultCode::Duplicate);
  storage.corrupt(5);
  CHECK(storage.get(5, loaded, sizeof(loaded), loadedLength) ==
        friendmesh::ResultCode::CorruptData);
  storage.setExpansionReadOnly(true);
  CHECK(storage.remove(5) == friendmesh::ResultCode::ReadOnly);

  friendmesh::DevelopmentPayloadStorage noExpansion;
  noExpansion.setInternalReadOnly(true);
  noExpansion.setExpansionAvailable(false);
  CHECK(noExpansion.put(1, payload, sizeof(payload)) ==
        friendmesh::ResultCode::ReadOnly);
  noExpansion.setFailWrites(true);
  CHECK(noExpansion.put(2, payload, sizeof(payload)) ==
        friendmesh::ResultCode::StorageUnavailable);

  friendmesh::DevelopmentEventJournal journal;
  for (size_t i = 0; i < friendmesh::kDevelopmentJournalRecords; ++i) {
    friendmesh::EventHeader event = makeEvent(
        static_cast<uint8_t>(i + 1), friendmesh::EventType::SyncReceipt,
        static_cast<uint32_t>(i + 1));
    event.payloadHandle = 0;
    event.payloadLength = 0;
    CHECK(journal.append(event) == friendmesh::ResultCode::Ok);
  }
  CHECK(journal.append(makeEvent(250, friendmesh::EventType::SyncReceipt, 200)) ==
        friendmesh::ResultCode::CapacityReached);

  friendmesh::DevelopmentOutboxStore outboxStore;
  for (size_t i = 0; i < friendmesh::kMaxOutboxEntries; ++i) {
    friendmesh::OutboxEntry entry = {};
    entry.event = makeEvent(static_cast<uint8_t>(i + 1),
                            friendmesh::EventType::SyncInventory,
                            static_cast<uint32_t>(i + 1));
    entry.state = friendmesh::OutboxState::Queued;
    entry.persistenceConfirmed = true;
    CHECK(outboxStore.upsert(entry) == friendmesh::ResultCode::Ok);
  }
  friendmesh::OutboxEntry overflow = {};
  overflow.event = makeEvent(200, friendmesh::EventType::SyncInventory, 200);
  overflow.state = friendmesh::OutboxState::Queued;
  CHECK(outboxStore.upsert(overflow) == friendmesh::ResultCode::CapacityReached);
}

void testFragmentation() {
  uint8_t source[500] = {};
  for (size_t i = 0; i < sizeof(source); ++i) {
    source[i] = static_cast<uint8_t>(i & 0xff);
  }
  friendmesh::FragmentAssembler assembler;
  CHECK(assembler.begin(makeId(1), sizeof(source), 3) ==
        friendmesh::ResultCode::Ok);
  CHECK(assembler.accept(1, source + friendmesh::kFragmentPayloadBytes,
                         friendmesh::kFragmentPayloadBytes) ==
        friendmesh::ResultCode::Ok);
  CHECK(assembler.accept(1, source + friendmesh::kFragmentPayloadBytes,
                         friendmesh::kFragmentPayloadBytes) ==
        friendmesh::ResultCode::Duplicate);
  uint8_t conflicting[friendmesh::kFragmentPayloadBytes] = {};
  CHECK(assembler.accept(1, conflicting, sizeof(conflicting)) ==
        friendmesh::ResultCode::Conflict);
  CHECK(assembler.accept(0, source, friendmesh::kFragmentPayloadBytes) ==
        friendmesh::ResultCode::Ok);
  uint8_t assembled[500] = {};
  size_t assembledLength = 0;
  CHECK(assembler.copy(assembled, sizeof(assembled), assembledLength) ==
        friendmesh::ResultCode::Incomplete);
  CHECK(assembler.accept(2, source + 2 * friendmesh::kFragmentPayloadBytes,
                         sizeof(source) - 2 * friendmesh::kFragmentPayloadBytes) ==
        friendmesh::ResultCode::Ok);
  CHECK(assembler.complete());
  CHECK(assembler.copy(assembled, sizeof(assembled), assembledLength) ==
        friendmesh::ResultCode::Ok);
  CHECK(assembledLength == sizeof(source));
  CHECK(memcmp(source, assembled, sizeof(source)) == 0);
}

void testChatLifecycleAndOutbox() {
  friendmesh::DomainStore domain;
  const friendmesh::FriendRecord alice = makeFriend(1, "Alice");
  const friendmesh::FriendRecord bob = makeFriend(2, "Bob");
  CHECK(domain.addFriend(alice) == friendmesh::ResultCode::Ok);
  CHECK(domain.addFriend(bob) == friendmesh::ResultCode::Ok);
  const friendmesh::Id128 groupId = makeId(120);
  CHECK(domain.createGroup(groupId, "Chat", alice.id, "Alice", 1) ==
        friendmesh::ResultCode::Ok);
  CHECK(domain.addMember(groupId, bob.id, "Bob", friendmesh::MemberRole::Member,
                         friendmesh::MemberState::Approved, 1, 2) ==
        friendmesh::ResultCode::Ok);

  friendmesh::DevelopmentPayloadStorage payloads;
  friendmesh::DevelopmentEventJournal journal;
  friendmesh::DevelopmentOutboxStore outboxStore;
  friendmesh::ChatService chat(domain, payloads, journal, outboxStore);

  const uint8_t hello[] = "hello group";
  friendmesh::EventHeader outgoing = makeScopedEvent(
      10, friendmesh::EventType::ChatMessage, groupId, alice.id, 1, 1,
      sizeof(hello) - 1 + friendmesh::kChatEnvelopeHeaderBytes, 100, 1000);
  CHECK(chat.createMessage(outgoing, hello, sizeof(hello) - 1) ==
        friendmesh::ResultCode::Ok);
  CHECK(chat.messageCount() == 1);
  CHECK(chat.message(outgoing.eventId)->record.delivery ==
        friendmesh::DeliveryState::Queued);
  CHECK(chat.outbox().size() == 1);
  CHECK(journal.size() == 1);
  uint8_t readback[32] = {};
  size_t readbackLength = 0;
  CHECK(chat.readMessageText(outgoing.eventId, readback, sizeof(readback),
                             readbackLength) == friendmesh::ResultCode::Ok);
  CHECK(readbackLength == sizeof(hello) - 1);
  CHECK(memcmp(readback, hello, readbackLength) == 0);

  CHECK(chat.beginTransmit(outgoing.eventId) == friendmesh::ResultCode::Ok);
  CHECK(chat.scheduleRetry(outgoing.eventId, 150,
                           friendmesh::ResultCode::StorageUnavailable) ==
        friendmesh::ResultCode::Ok);
  CHECK(chat.nextReady(149) == nullptr);
  CHECK(chat.nextReady(150)->event.type == friendmesh::EventType::ChatMessage);
  CHECK(chat.beginTransmit(outgoing.eventId) == friendmesh::ResultCode::Ok);
  CHECK(chat.markRelayedOrObserved(outgoing.eventId) ==
        friendmesh::ResultCode::Ok);
  CHECK(chat.message(outgoing.eventId)->record.delivery ==
        friendmesh::DeliveryState::RelayedOrObserved);
  CHECK(chat.markDelivered(outgoing.eventId) == friendmesh::ResultCode::Ok);
  CHECK(chat.message(outgoing.eventId)->record.delivery ==
        friendmesh::DeliveryState::Delivered);

  const uint8_t delayed[] = "offline message";
  friendmesh::EventHeader incoming = makeScopedEvent(
      20, friendmesh::EventType::ChatMessage, groupId, bob.id, 1, 2,
      sizeof(delayed) - 1 + friendmesh::kChatEnvelopeHeaderBytes, 100);
  CHECK(chat.receiveMessage(incoming, delayed, sizeof(delayed) - 1, 500, false) ==
        friendmesh::ResultCode::Ok);
  CHECK(chat.message(incoming.eventId)->record.delayed);
  CHECK(chat.message(incoming.eventId)->record.senderClockUntrusted);
  CHECK(chat.conversation(groupId)->unreadCount == 1);
  CHECK(chat.receiveMessage(incoming, delayed, sizeof(delayed) - 1, 501, false) ==
        friendmesh::ResultCode::Duplicate);
  uint8_t changed[sizeof(delayed) - 1] = {};
  memcpy(changed, delayed, sizeof(changed));
  changed[0] ^= 1;
  CHECK(chat.receiveMessage(incoming, changed, sizeof(changed), 502, false) ==
        friendmesh::ResultCode::Quarantined);
  CHECK(chat.conflictCount() == 1);

  CHECK(chat.setMuted(groupId, true) == friendmesh::ResultCode::Ok);
  const uint8_t mutedText[] = "muted";
  friendmesh::EventHeader muted = makeScopedEvent(
      21, friendmesh::EventType::ChatMessage, groupId, bob.id, 2, 3,
      sizeof(mutedText) - 1 + friendmesh::kChatEnvelopeHeaderBytes, 510);
  CHECK(chat.receiveMessage(muted, mutedText, sizeof(mutedText) - 1, 511, true) ==
        friendmesh::ResultCode::Ok);
  CHECK(chat.message(muted.eventId)->mutedAtReceipt);
  CHECK(chat.conversation(groupId)->unreadCount == 2);
  CHECK(chat.markRead(groupId) == friendmesh::ResultCode::Ok);
  CHECK(chat.conversation(groupId)->unreadCount == 0);

  const uint8_t replyText[] = "reply";
  friendmesh::EventHeader reply = makeScopedEvent(
      11, friendmesh::EventType::ChatMessage, groupId, alice.id, 2, 4,
      sizeof(replyText) - 1 + friendmesh::kChatEnvelopeHeaderBytes, 520);
  CHECK(chat.createMessage(reply, replyText, sizeof(replyText) - 1,
                           incoming.eventId) == friendmesh::ResultCode::Ok);
  CHECK(friendmesh::idsEqual(chat.message(reply.eventId)->replyTo,
                             incoming.eventId));
  CHECK(chat.cancelOutgoing(reply.eventId) == friendmesh::ResultCode::Ok);
  CHECK(chat.outbox().find(reply.eventId)->state ==
        friendmesh::OutboxState::Cancelled);
  CHECK(chat.removeTerminalOutgoing(reply.eventId) == friendmesh::ResultCode::Ok);
  CHECK(chat.outbox().find(reply.eventId) == nullptr);

  const uint8_t expiringText[] = "expires";
  friendmesh::EventHeader expiring = makeScopedEvent(
      13, friendmesh::EventType::ChatMessage, groupId, alice.id, 3, 9,
      sizeof(expiringText) - 1 + friendmesh::kChatEnvelopeHeaderBytes, 521, 522);
  CHECK(chat.createMessage(expiring, expiringText, sizeof(expiringText) - 1) ==
        friendmesh::ResultCode::Ok);
  chat.nextReady(523);
  CHECK(chat.outbox().find(expiring.eventId)->state ==
        friendmesh::OutboxState::Expired);
  CHECK(chat.message(expiring.eventId)->record.delivery ==
        friendmesh::DeliveryState::Expired);

  friendmesh::EventHeader reaction = makeScopedEvent(
      12, friendmesh::EventType::ChatReaction, groupId, alice.id, 4, 5,
      friendmesh::kIdSize + 1, 530);
  CHECK(chat.applyReaction(reaction, incoming.eventId,
                           friendmesh::ReactionKind::ThumbsUp, true, 530) ==
        friendmesh::ResultCode::Ok);
  CHECK(chat.reactionCount() == 1);
  outboxStore.setReadOnly(true);
  CHECK(chat.beginTransmit(reaction.eventId) == friendmesh::ResultCode::ReadOnly);
  CHECK(chat.outbox().find(reaction.eventId)->state ==
        friendmesh::OutboxState::Queued);
  outboxStore.setReadOnly(false);

  friendmesh::EventHeader unauthorizedDeletion = makeScopedEvent(
      25, friendmesh::EventType::ChatDeleted, groupId, bob.id, 3, 8,
      friendmesh::kIdSize, 535);
  CHECK(chat.applyDeletion(unauthorizedDeletion, outgoing.eventId, false, 536) ==
        friendmesh::ResultCode::Unauthorized);

  friendmesh::EventHeader deletion = makeScopedEvent(
      22, friendmesh::EventType::ChatDeleted, groupId, bob.id, 3, 6,
      friendmesh::kIdSize, 540);
  CHECK(chat.applyDeletion(deletion, incoming.eventId, false, 541) ==
        friendmesh::ResultCode::Ok);
  CHECK(chat.message(incoming.eventId)->visibility ==
        friendmesh::ChatVisibility::Deleted);

  const uint8_t conflictText[] = "collision";
  friendmesh::EventHeader handleConflict = makeScopedEvent(
      23, friendmesh::EventType::ChatMessage, groupId, bob.id, 4, 1,
      sizeof(conflictText) - 1 + friendmesh::kChatEnvelopeHeaderBytes, 550);
  CHECK(chat.receiveMessage(handleConflict, conflictText,
                            sizeof(conflictText) - 1, 551, true) ==
        friendmesh::ResultCode::Quarantined);

  payloads.setFailWrites(true);
  friendmesh::EventHeader storageFailure = makeScopedEvent(
      24, friendmesh::EventType::ChatMessage, groupId, bob.id, 5, 7, 4, 560);
  storageFailure.payloadLength += friendmesh::kChatEnvelopeHeaderBytes;
  const uint8_t failText[] = "fail";
  CHECK(chat.receiveMessage(storageFailure, failText, 4, 561, true) ==
        friendmesh::ResultCode::StorageUnavailable);
  CHECK(chat.conversation(groupId)->incompleteHistory);
}

void testChatRebootRecovery() {
  friendmesh::DomainStore domain;
  const friendmesh::FriendRecord alice = makeFriend(1, "Alice");
  const friendmesh::FriendRecord bob = makeFriend(2, "Bob");
  CHECK(domain.addFriend(alice) == friendmesh::ResultCode::Ok);
  CHECK(domain.addFriend(bob) == friendmesh::ResultCode::Ok);
  const friendmesh::Id128 groupId = makeId(120);
  CHECK(domain.createGroup(groupId, "Recovery", alice.id, "Alice", 1) ==
        friendmesh::ResultCode::Ok);
  CHECK(domain.addMember(groupId, bob.id, "Bob", friendmesh::MemberRole::Member,
                         friendmesh::MemberState::Approved, 1, 2) ==
        friendmesh::ResultCode::Ok);
  friendmesh::DevelopmentPayloadStorage payloads;
  friendmesh::DevelopmentEventJournal journal;
  friendmesh::DevelopmentOutboxStore outboxStore;
  friendmesh::EventHeader outgoing = makeScopedEvent(
      30, friendmesh::EventType::ChatMessage, groupId, alice.id, 1, 11, 7, 10);
  outgoing.payloadLength += friendmesh::kChatEnvelopeHeaderBytes;
  const uint8_t queued[] = "queued!";
  friendmesh::EventHeader incoming = makeScopedEvent(
      31, friendmesh::EventType::ChatMessage, groupId, bob.id, 1, 12, 8, 11);
  incoming.payloadLength += friendmesh::kChatEnvelopeHeaderBytes;
  const uint8_t received[] = "received";
  friendmesh::EventHeader reply = makeScopedEvent(
      32, friendmesh::EventType::ChatMessage, groupId, bob.id, 2, 13, 5, 12);
  reply.payloadLength += friendmesh::kChatEnvelopeHeaderBytes;
  const uint8_t replyText[] = "reply";
  friendmesh::EventHeader reaction = makeScopedEvent(
      33, friendmesh::EventType::ChatReaction, groupId, bob.id, 3, 14,
      friendmesh::kIdSize + 1, 13);
  friendmesh::EventHeader deletion = makeScopedEvent(
      34, friendmesh::EventType::ChatDeleted, groupId, bob.id, 4, 15,
      friendmesh::kIdSize, 14);
  {
    friendmesh::ChatService beforeReboot(domain, payloads, journal, outboxStore);
    CHECK(beforeReboot.createMessage(outgoing, queued, 7) ==
          friendmesh::ResultCode::Ok);
    CHECK(beforeReboot.beginTransmit(outgoing.eventId) ==
          friendmesh::ResultCode::Ok);
    CHECK(beforeReboot.scheduleRetry(outgoing.eventId, 50,
                                     friendmesh::ResultCode::Incomplete) ==
          friendmesh::ResultCode::Ok);
    CHECK(beforeReboot.receiveMessage(incoming, received, 8, 12, true) ==
          friendmesh::ResultCode::Ok);
    CHECK(beforeReboot.receiveMessage(reply, replyText, 5, 13, true,
                                      outgoing.eventId) ==
          friendmesh::ResultCode::Ok);
    CHECK(beforeReboot.applyReaction(reaction, incoming.eventId,
                                     friendmesh::ReactionKind::ThumbsDown, false,
                                     14) == friendmesh::ResultCode::Ok);
    CHECK(beforeReboot.applyDeletion(deletion, incoming.eventId, false, 15) ==
          friendmesh::ResultCode::Ok);
  }
  friendmesh::ChatService afterReboot(domain, payloads, journal, outboxStore);
  CHECK(afterReboot.restoreFromJournals(100) == friendmesh::ResultCode::Ok);
  CHECK(afterReboot.messageCount() == 3);
  CHECK(afterReboot.reactionCount() == 1);
  CHECK(afterReboot.message(incoming.eventId)->visibility ==
        friendmesh::ChatVisibility::Deleted);
  CHECK(friendmesh::idsEqual(afterReboot.message(reply.eventId)->replyTo,
                             outgoing.eventId));
  CHECK(afterReboot.conversation(groupId)->incompleteHistory);
  CHECK(afterReboot.outbox().size() == 1);
  CHECK(afterReboot.outbox().find(outgoing.eventId)->state ==
        friendmesh::OutboxState::RetryWaiting);
  CHECK(afterReboot.nextReady(49) == nullptr);
  CHECK(afterReboot.nextReady(50) != nullptr);
}

void testSynchronization() {
  friendmesh::DomainStore domain;
  const friendmesh::FriendRecord alice = makeFriend(1, "Alice");
  const friendmesh::FriendRecord bob = makeFriend(2, "Bob");
  CHECK(domain.addFriend(alice) == friendmesh::ResultCode::Ok);
  CHECK(domain.addFriend(bob) == friendmesh::ResultCode::Ok);
  const friendmesh::Id128 groupId = makeId(120);
  CHECK(domain.createGroup(groupId, "Sync", alice.id, "Alice", 1) ==
        friendmesh::ResultCode::Ok);
  CHECK(domain.addMember(groupId, bob.id, "Bob", friendmesh::MemberRole::Member,
                         friendmesh::MemberState::Approved, 1, 2) ==
        friendmesh::ResultCode::Ok);

  friendmesh::SyncEngine reordered(domain);
  friendmesh::SyncBatch batch = {};
  batch.count = 3;
  batch.events[0] = makeScopedEvent(42, friendmesh::EventType::ChatMessage,
                                    groupId, bob.id, 3, 3, 1, 30);
  batch.events[1] = makeScopedEvent(40, friendmesh::EventType::ChatMessage,
                                    groupId, bob.id, 1, 1, 1, 10);
  batch.events[2] = makeScopedEvent(41, friendmesh::EventType::ChatMessage,
                                    groupId, bob.id, 2, 2, 1, 20);
  uint8_t accepted = 0;
  CHECK(reordered.applyBatch(batch, 100, accepted) == friendmesh::ResultCode::Ok);
  CHECK(accepted == 3);
  const friendmesh::SyncProgress* progress =
      reordered.progress(groupId, bob.id, 1);
  CHECK(progress && progress->highestContiguousSequence == 3);
  CHECK(!progress->incomplete);
  CHECK(reordered.observe(batch.events[0], 101) ==
        friendmesh::ResultCode::Duplicate);

  friendmesh::EventHeader conflictingId = batch.events[0];
  conflictingId.createdAt = 31;
  CHECK(reordered.observe(conflictingId, 102) ==
        friendmesh::ResultCode::Quarantined);
  friendmesh::EventHeader sequenceCollision = makeScopedEvent(
      43, friendmesh::EventType::ChatMessage, groupId, bob.id, 3, 4, 1, 32);
  CHECK(reordered.observe(sequenceCollision, 103) ==
        friendmesh::ResultCode::Quarantined);
  friendmesh::EventHeader outsideWindow = makeScopedEvent(
      44, friendmesh::EventType::ChatMessage, groupId, bob.id, 70, 5, 1, 40);
  CHECK(reordered.observe(outsideWindow, 104) ==
        friendmesh::ResultCode::Quarantined);
  friendmesh::EventHeader futureEpoch = makeScopedEvent(
      45, friendmesh::EventType::ChatMessage, groupId, bob.id, 1, 6, 1, 50);
  futureEpoch.membershipEpoch = 2;
  CHECK(reordered.observe(futureEpoch, 105) ==
        friendmesh::ResultCode::Quarantined);

  friendmesh::SyncInventoryRecord local = {};
  CHECK(reordered.inventory(groupId, bob.id, 1, local) ==
        friendmesh::ResultCode::Ok);
  friendmesh::SyncInventoryRecord remote = local;
  remote.highestSeenSequence = 8;
  remote.highestContiguousSequence = 8;
  friendmesh::SyncRange range = {};
  CHECK(reordered.planMissingRange(local, remote, 3, range) ==
        friendmesh::ResultCode::Ok);
  CHECK(range.firstSequence == 4);
  CHECK(range.lastSequence == 6);
  friendmesh::SyncRange availableRange = {};
  availableRange.groupId = groupId;
  availableRange.senderId = bob.id;
  availableRange.membershipEpoch = 1;
  availableRange.firstSequence = 1;
  availableRange.lastSequence = 3;
  friendmesh::SyncBatch rangeBatch = {};
  CHECK(reordered.buildRangeBatch(availableRange, 3, rangeBatch) ==
        friendmesh::ResultCode::Ok);
  CHECK(rangeBatch.count == 3);
  CHECK(rangeBatch.events[0].senderSequence == 1);
  CHECK(rangeBatch.events[2].senderSequence == 3);
  friendmesh::SyncReceiptRecord receipt = {};
  CHECK(reordered.makeReceipt(groupId, bob.id, 1, 110, receipt) ==
        friendmesh::ResultCode::Ok);
  CHECK(receipt.highestContiguousSequence == 3);

  friendmesh::SyncEngine priority(domain);
  friendmesh::EventHeader background = makeScopedEvent(
      50, friendmesh::EventType::SyncInventory, groupId, alice.id, 1, 0, 0, 10);
  friendmesh::EventHeader normal = makeScopedEvent(
      51, friendmesh::EventType::ChatMessage, groupId, alice.id, 2, 1, 1, 20);
  friendmesh::EventHeader elevated = makeScopedEvent(
      52, friendmesh::EventType::MemberStateChanged, groupId, alice.id, 3, 0, 0,
      30);
  friendmesh::EventHeader safety = makeScopedEvent(
      53, friendmesh::EventType::SosOpened, groupId, bob.id, 1, 0, 0, 40);
  CHECK(priority.observe(background, 50) == friendmesh::ResultCode::Ok);
  CHECK(priority.observe(normal, 50) == friendmesh::ResultCode::Ok);
  CHECK(priority.observe(elevated, 50) == friendmesh::ResultCode::Ok);
  CHECK(priority.observe(safety, 50) == friendmesh::ResultCode::Ok);
  friendmesh::SyncBatch prioritized = {};
  CHECK(priority.buildPriorityBatch(groupId, 1, 4, prioritized) ==
        friendmesh::ResultCode::Ok);
  CHECK(prioritized.count == 4);
  CHECK(prioritized.events[0].type == friendmesh::EventType::SosOpened);
  CHECK(prioritized.events[1].type ==
        friendmesh::EventType::MemberStateChanged);
  CHECK(prioritized.events[2].type == friendmesh::EventType::ChatMessage);
  CHECK(prioritized.events[3].type == friendmesh::EventType::SyncInventory);
}

void testPositionsAndNavigation() {
  friendmesh::DomainStore domain;
  const friendmesh::FriendRecord alice = makeFriend(1, "Alice");
  const friendmesh::FriendRecord bob = makeFriend(2, "Bob");
  CHECK(domain.addFriend(alice) == friendmesh::ResultCode::Ok);
  CHECK(domain.addFriend(bob) == friendmesh::ResultCode::Ok);
  const friendmesh::Id128 groupId = makeId(120);
  CHECK(domain.createGroup(groupId, "Navigation", alice.id, "Alice", 1) ==
        friendmesh::ResultCode::Ok);
  CHECK(domain.addMember(groupId, bob.id, "Bob", friendmesh::MemberRole::Member,
                         friendmesh::MemberState::Approved, 1, 2) ==
        friendmesh::ResultCode::Ok);
  domain.mutableGroupById(groupId)->locationVisibility =
      friendmesh::LocationVisibility::Precise;

  friendmesh::PositionBook positions;
  const friendmesh::PositionRecord alicePosition =
      makePosition(alice.id, 0, 0, 100);
  const friendmesh::PositionRecord bobPosition =
      makePosition(bob.id, 0, 10000, 100);
  friendmesh::EventHeader positionEvent = makeScopedEvent(
      90, friendmesh::EventType::PositionShared, groupId, bob.id, 1, 0, 0, 100);
  CHECK(positions.apply(positionEvent, bobPosition, domain, 105) ==
        friendmesh::ResultCode::Ok);
  CHECK(positions.size() == 1);
  CHECK(positions.bySubject(bob.id)->receivedAt == 105);
  CHECK(!positions.stale(bob.id, 399));
  CHECK(positions.stale(bob.id, 401));
  CHECK(friendmesh::greatCircleDistanceMeters(alicePosition, bobPosition) >= 110);
  CHECK(friendmesh::greatCircleDistanceMeters(alicePosition, bobPosition) <= 112);
  CHECK(friendmesh::absoluteBearingDegrees(alicePosition, bobPosition) == 90);

  const friendmesh::PositionRecord bobPrevious =
      makePosition(bob.id, 377749000, -1224194000, 100);
  const friendmesh::PositionRecord bobCurrent =
      makePosition(bob.id, 377749000, -1224191500, 120);
  const friendmesh::MotionEstimate eastMotion =
      friendmesh::estimateTargetMotion(bobPrevious, bobCurrent, 125);
  CHECK(eastMotion.samplesUsable);
  CHECK(eastMotion.moving);
  CHECK(eastMotion.confidence == friendmesh::MotionConfidence::Good);
  CHECK(eastMotion.bearingDegrees >= 89 && eastMotion.bearingDegrees <= 91);
  CHECK(eastMotion.speedCentimetersPerSecond >= 100);
  CHECK(eastMotion.speedCentimetersPerSecond <= 120);
  CHECK(eastMotion.horizonSeconds == friendmesh::kMotionPredictionSeconds);
  CHECK(eastMotion.predicted.longitudeE7 > bobCurrent.longitudeE7);
  // A spherical projection can move a few E7 units in latitude even when the
  // initial bearing is due east; assert that the drift remains negligible.
  CHECK(eastMotion.predicted.latitudeE7 >= bobCurrent.latitudeE7 - 10);
  CHECK(eastMotion.predicted.latitudeE7 <= bobCurrent.latitudeE7 + 10);

  const friendmesh::PositionRecord bobStill =
      makePosition(bob.id, bobCurrent.latitudeE7, bobCurrent.longitudeE7, 140);
  const friendmesh::MotionEstimate stillMotion =
      friendmesh::estimateTargetMotion(bobCurrent, bobStill, 145);
  CHECK(stillMotion.samplesUsable);
  CHECK(!stillMotion.moving);
  CHECK(stillMotion.speedCentimetersPerSecond == 0);

  friendmesh::PositionRecord impossibleJump = bobStill;
  impossibleJump.capturedAt = 150;
  impossibleJump.longitudeE7 += 100000;
  CHECK(!friendmesh::estimateTargetMotion(
      bobStill, impossibleJump, 151).samplesUsable);
  CHECK(!friendmesh::estimateTargetMotion(
      bobPrevious, bobCurrent, 1000).samplesUsable);

  // Course-over-ground for the local user is distinct from device heading.
  // Moving east toward an eastward target needs no turn and has positive
  // closing speed.
  const friendmesh::PositionRecord alicePrevious =
      makePosition(alice.id, 0, 0, 100);
  const friendmesh::PositionRecord aliceCurrent =
      makePosition(alice.id, 0, 2500, 120);
  const friendmesh::PositionRecord eastTarget =
      makePosition(bob.id, 0, 10000, 120);
  friendmesh::CourseToTargetEstimate course =
      friendmesh::estimateCourseToTarget(
          alicePrevious, aliceCurrent, eastTarget, 125);
  CHECK(course.targetUsable);
  CHECK(course.courseUsable);
  CHECK(course.motion.bearingDegrees >= 89 &&
        course.motion.bearingDegrees <= 91);
  CHECK(course.turnDegrees >= -1 && course.turnDegrees <= 1);
  CHECK(course.progress == friendmesh::NavigationProgress::Closer);
  CHECK(course.closingSpeedCentimetersPerSecond > 100);

  // Local headway remains usable above the walking-only prediction ceiling.
  // Roughly 600 m east in 20 s is about 30 m/s (highway speed).
  const friendmesh::PositionRecord aliceFast =
      makePosition(alice.id, 0, 53900, 120);
  const friendmesh::PositionRecord fartherEastTarget =
      makePosition(bob.id, 0, 200000, 120);
  course = friendmesh::estimateCourseToTarget(
      alicePrevious, aliceFast, fartherEastTarget, 125);
  CHECK(course.courseUsable);
  CHECK(course.motion.speedCentimetersPerSecond >= 2900);
  CHECK(course.motion.speedCentimetersPerSecond <= 3100);
  CHECK(course.motion.bearingDegrees >= 89 &&
        course.motion.bearingDegrees <= 91);

  // About five metres over 20 seconds is slow movement, not stationary jitter,
  // and should eventually establish a course.
  const friendmesh::PositionRecord aliceSlow =
      makePosition(alice.id, 0, 450, 120);
  course = friendmesh::estimateCourseToTarget(
      alicePrevious, aliceSlow, eastTarget, 125);
  CHECK(course.courseUsable);
  CHECK(course.motion.speedCentimetersPerSecond >= 20);
  CHECK(course.motion.speedCentimetersPerSecond <= 30);

  // Retain a high ceiling for rejecting a genuine GPS teleport.
  const friendmesh::PositionRecord aliceImplausible =
      makePosition(alice.id, 0, 200000, 120);
  course = friendmesh::estimateCourseToTarget(
      alicePrevious, aliceImplausible, fartherEastTarget, 125);
  CHECK(!course.courseUsable);

  // The same eastbound course needs a left turn for a target due north.
  const friendmesh::PositionRecord northTarget =
      makePosition(bob.id, 10000, 2500, 120);
  course = friendmesh::estimateCourseToTarget(
      alicePrevious, aliceCurrent, northTarget, 125);
  CHECK(course.courseUsable);
  CHECK(course.turnDegrees >= -91 && course.turnDegrees <= -89);

  // Westbound travel away from the east target produces negative headway.
  const friendmesh::PositionRecord aliceWest =
      makePosition(alice.id, 0, -2500, 120);
  course = friendmesh::estimateCourseToTarget(
      alicePrevious, aliceWest, eastTarget, 125);
  CHECK(course.courseUsable);
  CHECK(course.progress == friendmesh::NavigationProgress::Farther);
  CHECK(course.closingSpeedCentimetersPerSecond < 0);

  friendmesh::PositionRecord hidden = bobPosition;
  hidden.hiddenByPolicy = true;
  CHECK(friendmesh::greatCircleDistanceMeters(alicePosition, hidden) == UINT32_MAX);
  domain.mutableGroupById(groupId)->locationVisibility =
      friendmesh::LocationVisibility::Hidden;
  positionEvent.eventId = makeId(91);
  positionEvent.senderSequence = 2;
  CHECK(positions.apply(positionEvent, bobPosition, domain, 106) ==
        friendmesh::ResultCode::Unauthorized);
  domain.mutableGroupById(groupId)->locationVisibility =
      friendmesh::LocationVisibility::Precise;

  friendmesh::NavigationService navigation;
  CHECK(navigation.start(bob.id, 100) == friendmesh::ResultCode::Ok);
  friendmesh::NavigationUpdate update =
      navigation.update(alicePosition, &bobPosition, 101);
  CHECK(update.targetAvailable);
  CHECK(update.progress == friendmesh::NavigationProgress::Unknown);
  CHECK(update.state.headingSource == friendmesh::HeadingSource::NorthUp);
  CHECK(navigation.breadcrumbCount() == 1);

  friendmesh::PositionRecord closer = makePosition(alice.id, 0, 4000, 110);
  update = navigation.update(closer, &bobPosition, 111);
  CHECK(update.progress == friendmesh::NavigationProgress::Closer);
  CHECK(update.state.headingSource == friendmesh::HeadingSource::GpsCourse);
  CHECK(update.state.headingDegrees == 90);

  friendmesh::PositionRecord arrived = makePosition(alice.id, 0, 9500, 120);
  update = navigation.update(arrived, &bobPosition, 121, 270, true);
  CHECK(update.progress == friendmesh::NavigationProgress::Arrived);
  CHECK(update.arrivalChanged);
  CHECK(update.state.headingSource == friendmesh::HeadingSource::Magnetometer);
  CHECK(update.state.headingDegrees == 270);
  CHECK(navigation.stop() == friendmesh::ResultCode::Ok);
  CHECK(navigation.stop() == friendmesh::ResultCode::InvalidState);
}

void testMarkersAndMeetups() {
  friendmesh::DomainStore domain;
  const friendmesh::FriendRecord alice = makeFriend(1, "Alice");
  const friendmesh::FriendRecord bob = makeFriend(2, "Bob");
  CHECK(domain.addFriend(alice) == friendmesh::ResultCode::Ok);
  CHECK(domain.addFriend(bob) == friendmesh::ResultCode::Ok);
  const friendmesh::Id128 groupId = makeId(120);
  CHECK(domain.createGroup(groupId, "Field", alice.id, "Alice", 1) ==
        friendmesh::ResultCode::Ok);
  CHECK(domain.addMember(groupId, bob.id, "Bob", friendmesh::MemberRole::Member,
                         friendmesh::MemberState::Approved, 1, 2) ==
        friendmesh::ResultCode::Ok);

  friendmesh::MarkerService markers;
  friendmesh::EventHeader markerEvent = makeScopedEvent(
      92, friendmesh::EventType::MarkerCreated, groupId, bob.id, 1, 0, 0, 100);
  friendmesh::MarkerRecord marker = {};
  marker.markerId = markerEvent.eventId;
  marker.groupId = groupId;
  marker.creatorId = bob.id;
  marker.position = makePosition(bob.id, 340000000, -1180000000, 100);
  marker.type = friendmesh::MarkerType::Resource;
  marker.state = friendmesh::MarkerState::Active;
  marker.createdAt = 100;
  marker.expiresAt = 200;
  CHECK(markers.create(markerEvent, marker, domain) == friendmesh::ResultCode::Ok);
  CHECK(markers.create(markerEvent, marker, domain) ==
        friendmesh::ResultCode::Duplicate);

  friendmesh::MarkerRecord movedMarker = marker;
  movedMarker.position.longitudeE7 += 1000;
  friendmesh::EventHeader unauthorizedUpdate = makeScopedEvent(
      93, friendmesh::EventType::MarkerUpdated, groupId, makeId(3), 1, 0, 0,
      110);
  CHECK(markers.update(unauthorizedUpdate, movedMarker, domain) ==
        friendmesh::ResultCode::Unauthorized);
  friendmesh::EventHeader adminUpdate = makeScopedEvent(
      94, friendmesh::EventType::MarkerUpdated, groupId, alice.id, 1, 0, 0, 111);
  CHECK(markers.update(adminUpdate, movedMarker, domain) ==
        friendmesh::ResultCode::Ok);
  CHECK(markers.processTime(150, 40) == 1);
  CHECK(markers.byId(marker.markerId)->state ==
        friendmesh::MarkerState::NeedsReconfirmation);
  CHECK(markers.processTime(200, 40) == 1);
  CHECK(markers.byId(marker.markerId)->state == friendmesh::MarkerState::Expired);

  friendmesh::MeetupService meetups;
  friendmesh::EventHeader proposalEvent = makeScopedEvent(
      95, friendmesh::EventType::MeetupProposed, groupId, alice.id, 2, 0, 0,
      300);
  friendmesh::MeetupRecord meetup = {};
  meetup.meetupId = proposalEvent.eventId;
  meetup.groupId = groupId;
  meetup.proposerId = alice.id;
  meetup.position = makePosition(alice.id, 340000000, -1180000000, 300);
  meetup.state = friendmesh::MeetupState::Proposed;
  meetup.proposedAt = 300;
  meetup.voteClosesAt = 320;
  meetup.expiresAt = 500;
  CHECK(meetups.propose(proposalEvent, meetup, domain) ==
        friendmesh::ResultCode::Ok);

  friendmesh::EventHeader aliceVote = makeScopedEvent(
      96, friendmesh::EventType::MeetupVote, groupId, alice.id, 3, 0, 0, 301);
  friendmesh::EventHeader bobVote = makeScopedEvent(
      97, friendmesh::EventType::MeetupVote, groupId, bob.id, 2, 0, 0, 302);
  CHECK(meetups.vote(aliceVote, meetup.meetupId, friendmesh::VoteChoice::Yes,
                     domain, 301) == friendmesh::ResultCode::Ok);
  CHECK(meetups.vote(bobVote, meetup.meetupId, friendmesh::VoteChoice::No,
                     domain, 302) == friendmesh::ResultCode::Ok);
  CHECK(meetups.vote(bobVote, meetup.meetupId, friendmesh::VoteChoice::Yes,
                     domain, 303) == friendmesh::ResultCode::Ok);
  CHECK(meetups.byId(meetup.meetupId)->meetup.yesVotes == 2);
  CHECK(meetups.byId(meetup.meetupId)->meetup.noVotes == 0);

  friendmesh::EventHeader activate = makeScopedEvent(
      98, friendmesh::EventType::MeetupStateChanged, groupId, alice.id, 4, 0, 0,
      321);
  CHECK(meetups.setState(activate, meetup.meetupId,
                         friendmesh::MeetupState::Active, domain, 321) ==
        friendmesh::ResultCode::Ok);
  CHECK(meetups.setAttending(meetup.meetupId, alice.id, true, domain) ==
        friendmesh::ResultCode::Ok);
  CHECK(meetups.setAttending(meetup.meetupId, bob.id, true, domain) ==
        friendmesh::ResultCode::Ok);
  CHECK(meetups.byId(meetup.meetupId)->meetup.attendingCount == 2);
  CHECK(meetups.processExpiry(500) == 1);
  CHECK(meetups.byId(meetup.meetupId)->meetup.state ==
        friendmesh::MeetupState::Expired);
}

void testSafetyIncidentsAndNotifications() {
  friendmesh::DomainStore domain;
  friendmesh::FriendRecord alice = makeFriend(1, "Alice");
  friendmesh::FriendRecord bob = makeFriend(2, "Bob");
  alice.emergencyContactAllowed = true;
  bob.emergencyContactAllowed = true;
  CHECK(domain.addFriend(alice) == friendmesh::ResultCode::Ok);
  CHECK(domain.addFriend(bob) == friendmesh::ResultCode::Ok);
  const friendmesh::Id128 groupId = makeId(120);
  CHECK(domain.createGroup(groupId, "Safety", alice.id, "Alice", 1) ==
        friendmesh::ResultCode::Ok);
  CHECK(domain.addMember(groupId, bob.id, "Bob", friendmesh::MemberRole::Member,
                         friendmesh::MemberState::Approved, 1, 2) ==
        friendmesh::ResultCode::Ok);

  friendmesh::SafetyNotificationCenter notifications;
  friendmesh::SafetyService safety(domain, notifications);
  friendmesh::IncidentRecord sos = {};
  sos.incidentId = makeId(180);
  sos.originatorId = alice.id;
  sos.kind = friendmesh::IncidentKind::Sos;
  sos.position = makePosition(alice.id, 340000000, -1180000000, 100);
  CHECK(safety.beginSosHold(sos, groupId, 100) == friendmesh::ResultCode::Ok);
  CHECK(safety.updateSosHold(102, true, friendmesh::ResultCode::Ok) ==
        friendmesh::ResultCode::Incomplete);
  CHECK(safety.updateSosHold(103, true,
                             friendmesh::ResultCode::StorageUnavailable) ==
        friendmesh::ResultCode::Incomplete);
  CHECK(!safety.holdPending());
  CHECK(safety.size() == 1);
  CHECK(safety.byId(sos.incidentId)->incident.state ==
        friendmesh::IncidentState::Active);
  CHECK(safety.byId(sos.incidentId)->incident.persistenceFailed);
  CHECK(safety.byId(sos.incidentId)->locationAvailability ==
        friendmesh::LocationAvailability::Current);
  CHECK(notifications.size() == 1);
  CHECK(notifications.at(0)->priority == friendmesh::EventPriority::Safety);

  friendmesh::EventHeader responding = makeScopedEvent(
      181, friendmesh::EventType::SosStatusChanged, groupId, bob.id, 1, 0, 0,
      110);
  CHECK(safety.applyStatus(responding, sos.incidentId,
                           friendmesh::ResponderState::Responding, 110) ==
        friendmesh::ResultCode::Ok);
  CHECK(safety.byId(sos.incidentId)->incident.deliveredCount == 1);
  CHECK(safety.byId(sos.incidentId)->incident.respondingCount == 1);
  CHECK(safety.applyStatus(responding, sos.incidentId,
                           friendmesh::ResponderState::Responding, 111) ==
        friendmesh::ResultCode::Duplicate);
  CHECK(safety.approvePublicFallback(sos.incidentId, bob.id, true) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(safety.approvePublicFallback(sos.incidentId, alice.id, true) ==
        friendmesh::ResultCode::Ok);
  CHECK(safety.byId(sos.incidentId)->incident.publicFallbackApproved);

  CHECK(safety.beginCancel(sos.incidentId, bob.id, 120) ==
        friendmesh::ResultCode::Unauthorized);
  CHECK(safety.beginCancel(sos.incidentId, alice.id, 120) ==
        friendmesh::ResultCode::Ok);
  CHECK(safety.abortCancel(sos.incidentId, alice.id) ==
        friendmesh::ResultCode::Ok);
  CHECK(safety.beginCancel(sos.incidentId, alice.id, 130) ==
        friendmesh::ResultCode::Ok);
  CHECK(safety.processTime(134) == 0);
  CHECK(safety.processTime(135) == 1);
  CHECK(safety.byId(sos.incidentId)->incident.state ==
        friendmesh::IncidentState::ClosedFalseAlarm);

  friendmesh::IncidentRecord help = {};
  friendmesh::EventHeader helpOpen = makeScopedEvent(
      182, friendmesh::EventType::HelpOpened, groupId, alice.id, 2, 0, 0, 200);
  help.incidentId = helpOpen.eventId;
  help.originatorId = alice.id;
  help.kind = friendmesh::IncidentKind::HelpRide;
  help.position.source = friendmesh::PositionSource::Unknown;
  CHECK(safety.openHelp(helpOpen, help, groupId, friendmesh::ResultCode::Ok,
                        200) == friendmesh::ResultCode::Ok);
  CHECK(safety.byId(help.incidentId)->locationAvailability ==
        friendmesh::LocationAvailability::Missing);
  CHECK(notifications.at(1)->priority == friendmesh::EventPriority::Elevated);

  friendmesh::EventHeader unable = makeScopedEvent(
      183, friendmesh::EventType::HelpStatusChanged, groupId, bob.id, 2, 0, 0,
      201);
  CHECK(safety.applyStatus(unable, help.incidentId,
                           friendmesh::ResponderState::Unable, 201) ==
        friendmesh::ResultCode::Ok);
  friendmesh::EventHeader helpClose = makeScopedEvent(
      184, friendmesh::EventType::HelpClosed, groupId, alice.id, 3, 0, 0, 210);
  CHECK(safety.close(helpClose, help.incidentId,
                     friendmesh::IncidentState::ClosedSafe, 210) ==
        friendmesh::ResultCode::Ok);
  CHECK(notifications.size() == 3);
  CHECK(notifications.acknowledge(help.incidentId) ==
        friendmesh::ResultCode::Ok);
  CHECK(notifications.unacknowledgedCount() == 1);
}

void testDevelopmentRuntimeWalkthrough() {
  friendmesh::DevelopmentRuntime runtime;
  CHECK(runtime.addLocalMessage("before setup", 1) ==
        friendmesh::ResultCode::InvalidState);
  CHECK(runtime.initialize("T-Deck owner", 10) == friendmesh::ResultCode::Ok);
  CHECK(runtime.initialize("again", 11) == friendmesh::ResultCode::Duplicate);
  CHECK(runtime.addLocalMessage("First FriendMesh note", 12) ==
        friendmesh::ResultCode::Ok);
  CHECK(runtime.addLocalMarker(340000000, -1180000000,
                               friendmesh::MarkerType::Meetup, 13) ==
        friendmesh::ResultCode::Ok);
  CHECK(runtime.openLocalHelp(friendmesh::IncidentKind::HelpRide, 340000000,
                              -1180000000, true, 14) ==
        friendmesh::ResultCode::Ok);
  const friendmesh::DevelopmentRuntimeSnapshot snapshot = runtime.snapshot();
  CHECK(snapshot.initialized);
  CHECK(snapshot.messages == 1);
  CHECK(snapshot.markers == 1);
  CHECK(snapshot.incidents == 1);
  CHECK(snapshot.notifications == 1);
  CHECK(runtime.chat().outbox().size() == 1);
}

void testEventHistory() {
  friendmesh::EventHistory<3> history;
  const friendmesh::EventHeader first =
      makeEvent(1, friendmesh::EventType::ChatMessage, 10);
  const friendmesh::EventHeader second =
      makeEvent(2, friendmesh::EventType::MarkerCreated, 20);
  const friendmesh::EventHeader third =
      makeEvent(3, friendmesh::EventType::MeetupProposed, 30);
  const friendmesh::EventHeader fourth =
      makeEvent(4, friendmesh::EventType::HelpOpened, 40);

  CHECK(history.append(first) == friendmesh::ResultCode::Ok);
  CHECK(history.append(first) == friendmesh::ResultCode::Duplicate);
  CHECK(history.append(second) == friendmesh::ResultCode::Ok);
  CHECK(history.append(third) == friendmesh::ResultCode::Ok);
  CHECK(history.append(fourth) == friendmesh::ResultCode::Ok);
  CHECK(history.size() == 3);
  CHECK(!history.contains(first.eventId));
  CHECK(history.contains(fourth.eventId));
  CHECK(history.at(0)->type == friendmesh::EventType::MarkerCreated);
  CHECK(history.newest()->type == friendmesh::EventType::HelpOpened);
}

void testOutbox() {
  friendmesh::OutboxQueue<4> queue;
  const friendmesh::EventHeader chat =
      makeEvent(1, friendmesh::EventType::ChatMessage, 10, 1000);
  const friendmesh::EventHeader sync =
      makeEvent(2, friendmesh::EventType::SyncInventory, 5, 1000);
  const friendmesh::EventHeader sos =
      makeEvent(3, friendmesh::EventType::SosOpened, 20, 1000);

  CHECK(queue.enqueue(chat, false) == friendmesh::ResultCode::InvalidState);
  CHECK(queue.enqueue(chat, true) == friendmesh::ResultCode::Ok);
  CHECK(queue.enqueue(sync, true) == friendmesh::ResultCode::Ok);
  CHECK(queue.enqueue(sos, false) == friendmesh::ResultCode::Ok);
  CHECK(queue.nextReady(30)->event.type == friendmesh::EventType::SosOpened);

  CHECK(queue.markTransmitting(sos.eventId) == friendmesh::ResultCode::Ok);
  CHECK(queue.scheduleRetry(sos.eventId, 50, friendmesh::ResultCode::InvalidState) ==
        friendmesh::ResultCode::Ok);
  CHECK(queue.nextReady(40)->event.type == friendmesh::EventType::ChatMessage);
  CHECK(queue.nextReady(50)->event.type == friendmesh::EventType::SosOpened);
  CHECK(queue.markTransmitting(sos.eventId) == friendmesh::ResultCode::Ok);
  CHECK(queue.markRelayedOrObserved(sos.eventId) == friendmesh::ResultCode::Ok);
  CHECK(queue.markDelivered(sos.eventId) == friendmesh::ResultCode::Ok);
  CHECK(queue.removeTerminal(sos.eventId) == friendmesh::ResultCode::Ok);
  CHECK(queue.size() == 2);

  friendmesh::EventHeader expired =
      makeEvent(9, friendmesh::EventType::ChatMessage, 1, 2);
  CHECK(queue.enqueue(expired, true) == friendmesh::ResultCode::Ok);
  queue.nextReady(10);
  CHECK(queue.find(expired.eventId)->state == friendmesh::OutboxState::Expired);
}

void testFeatureServiceSafetyGate() {
  friendmesh::FriendMeshFeatureService service;
  service.begin();
  const friendmesh::StatusSnapshot status = service.status();
  CHECK(status.lifecycle == friendmesh::LifecycleState::NotConfigured);
  CHECK(status.reason == friendmesh::StatusReason::FoundationOnly);
  CHECK(!status.storageReady);
  CHECK(!status.identityReady);
  CHECK(!status.protocolReady);
  CHECK(!status.userConsent);
  CHECK(!service.canTransmit());
  CHECK(service.providers().storage == nullptr);
  CHECK(service.providers().security == nullptr);
  CHECK(service.providers().transport == nullptr);
}

void testFeatureCoordinator() {
  friendmesh::FeatureCoordinator<4, 3> coordinator;
  const friendmesh::EventHeader chat =
      makeEvent(10, friendmesh::EventType::ChatMessage, 10, 100);
  const friendmesh::EventHeader position =
      makeEvent(11, friendmesh::EventType::PositionShared, 11, 100);

  CHECK(coordinator.queueOutgoing(chat, false) == friendmesh::ResultCode::InvalidState);
  CHECK(coordinator.history().empty());
  CHECK(coordinator.outbox().size() == 0);
  CHECK(coordinator.queueOutgoing(chat, true) == friendmesh::ResultCode::Ok);
  CHECK(coordinator.history().contains(chat.eventId));
  CHECK(coordinator.outbox().find(chat.eventId) != nullptr);
  CHECK(coordinator.queueOutgoing(chat, true) == friendmesh::ResultCode::Duplicate);
  CHECK(coordinator.recordIncoming(position) == friendmesh::ResultCode::Ok);
  CHECK(coordinator.history().size() == 2);
}

void testDirectChannelInviteEnvelope() {
  friendmesh::DirectChannelInvite source = {};
  memcpy(source.channelName, "Trail Friends", 14);
  for (size_t i = 0; i < sizeof(source.channelSecret); ++i) {
    source.channelSecret[i] = static_cast<uint8_t>(0xA0 + i);
  }

  char encoded[friendmesh::kDirectChannelInviteMaxText + 1] = {};
  size_t written = 0;
  CHECK(friendmesh::encodeDirectChannelInvite(
            source, encoded, sizeof(encoded), written) ==
        friendmesh::ResultCode::Ok);
  CHECK(written == strlen(encoded));
  CHECK(friendmesh::isDirectChannelInviteText(encoded));

  friendmesh::DirectChannelInvite decoded = {};
  CHECK(friendmesh::decodeDirectChannelInvite(encoded, decoded) ==
        friendmesh::ResultCode::Ok);
  CHECK(strcmp(decoded.channelName, source.channelName) == 0);
  CHECK(memcmp(decoded.channelSecret, source.channelSecret,
               sizeof(source.channelSecret)) == 0);

  char malformed[sizeof(encoded)] = {};
  memcpy(malformed, encoded, written + 1);
  malformed[6] = 'Z';
  CHECK(friendmesh::decodeDirectChannelInvite(malformed, decoded) ==
        friendmesh::ResultCode::InvalidText);
  CHECK(friendmesh::decodeDirectChannelInvite("ordinary message", decoded) ==
        friendmesh::ResultCode::InvalidArgument);

  char tooSmall[16] = {};
  CHECK(friendmesh::encodeDirectChannelInvite(
            source, tooSmall, sizeof(tooSmall), written) ==
        friendmesh::ResultCode::CapacityReached);

  memset(&source, 0, sizeof(source));
  CHECK(friendmesh::encodeDirectChannelInvite(
            source, encoded, sizeof(encoded), written) ==
        friendmesh::ResultCode::InvalidText);

  uint8_t tag[friendmesh::kChannelControlTagBytes] = {};
  for (size_t i = 0; i < sizeof(tag); ++i) tag[i] = (uint8_t)(0x30 + i);
  const friendmesh::ChannelControlType controls[] = {
      friendmesh::ChannelControlType::Joined,
      friendmesh::ChannelControlType::Left,
      friendmesh::ChannelControlType::Removed,
  };
  for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
    char control[friendmesh::kChannelControlMaxText + 1] = {};
    CHECK(friendmesh::encodeChannelControl(
              controls[i], tag, control, sizeof(control), written) ==
          friendmesh::ResultCode::Ok);
    CHECK(friendmesh::isChannelControlText(control));
    friendmesh::ChannelControlType decodedType =
        friendmesh::ChannelControlType::Joined;
    uint8_t decodedTag[friendmesh::kChannelControlTagBytes] = {};
    CHECK(friendmesh::decodeChannelControl(control, decodedType, decodedTag) ==
          friendmesh::ResultCode::Ok);
    CHECK(decodedType == controls[i]);
    CHECK(memcmp(decodedTag, tag, sizeof(tag)) == 0);
  }
  friendmesh::ChannelControlType ignored =
      friendmesh::ChannelControlType::Joined;
  CHECK(friendmesh::decodeChannelControl("FMCA1:00", ignored, tag) ==
        friendmesh::ResultCode::InvalidText);

  friendmesh::CompassStartedNotice compass = {};
  memcpy(compass.channelTag, tag, sizeof(tag));
  compass.distanceMeters = 1234;
  char compassText[friendmesh::kCompassStartedNoticeMaxText + 1] = {};
  CHECK(friendmesh::encodeCompassStartedNotice(
            compass, compassText, sizeof(compassText), written) ==
        friendmesh::ResultCode::Ok);
  CHECK(written == friendmesh::kCompassStartedNoticeMaxText);
  CHECK(friendmesh::isCompassStartedNoticeText(compassText));
  friendmesh::CompassStartedNotice decodedCompass = {};
  CHECK(friendmesh::decodeCompassStartedNotice(
            compassText, decodedCompass) == friendmesh::ResultCode::Ok);
  CHECK(memcmp(decodedCompass.channelTag, tag, sizeof(tag)) == 0);
  CHECK(decodedCompass.distanceMeters == 1234);
  compassText[written - 1] = 'Z';
  CHECK(friendmesh::decodeCompassStartedNotice(
            compassText, decodedCompass) == friendmesh::ResultCode::InvalidText);
  compass.distanceMeters = friendmesh::kCompassStartedMaxDistanceMeters + 1;
  CHECK(friendmesh::encodeCompassStartedNotice(
            compass, compassText, sizeof(compassText), written) ==
        friendmesh::ResultCode::InvalidArgument);
}

void testChannelRoster() {
  friendmesh::ChannelRoster roster = {};
  uint8_t alice[friendmesh::kChannelRosterPrefixBytes] = {1,2,3,4,5,6};
  uint8_t bob[friendmesh::kChannelRosterPrefixBytes] = {8,9,10,11,12,13};
  CHECK(friendmesh::setChannelRosterMember(
            roster, alice, friendmesh::ChannelRosterRole::Admin,
            friendmesh::ChannelRosterState::Joined) ==
        friendmesh::ResultCode::Ok);
  CHECK(friendmesh::setChannelRosterMember(
            roster, bob, friendmesh::ChannelRosterRole::Member,
            friendmesh::ChannelRosterState::Invited) ==
        friendmesh::ResultCode::Ok);
  CHECK(friendmesh::setChannelRosterMember(
            roster, bob, friendmesh::ChannelRosterRole::Member,
            friendmesh::ChannelRosterState::Joined) ==
        friendmesh::ResultCode::Ok);
  roster.rekeyRequired = true;
  CHECK(roster.memberCount == 2);
  CHECK(friendmesh::findChannelRosterMember(roster, bob)->state ==
        friendmesh::ChannelRosterState::Joined);

  uint8_t encoded[friendmesh::kChannelRosterEncodedBytes] = {};
  size_t written = 0;
  CHECK(friendmesh::encodeChannelRoster(
            roster, encoded, sizeof(encoded), written) ==
        friendmesh::ResultCode::Ok);
  friendmesh::ChannelRoster decoded = {};
  CHECK(friendmesh::decodeChannelRoster(encoded, written, decoded) ==
        friendmesh::ResultCode::Ok);
  CHECK(decoded.memberCount == 2);
  CHECK(decoded.rekeyRequired);
  CHECK(friendmesh::findChannelRosterMember(decoded, alice)->role ==
        friendmesh::ChannelRosterRole::Admin);
  char text[friendmesh::kChannelRosterMaxText + 1] = {};
  CHECK(friendmesh::encodeChannelRosterText(
            roster, text, sizeof(text), written) == friendmesh::ResultCode::Ok);
  CHECK(friendmesh::isChannelRosterText(text));
  CHECK(friendmesh::legacyChannelRosterTextPayload(text) == text);
  char prefixed[friendmesh::kChannelRosterMaxText + 16] = "Alice: ";
  strncat(prefixed, text, sizeof(prefixed) - strlen(prefixed) - 1);
  CHECK(friendmesh::legacyChannelRosterTextPayload(prefixed) == prefixed + 7);
  CHECK(friendmesh::legacyChannelRosterTextPayload("Alice: hello") == nullptr);
  CHECK(friendmesh::legacyChannelRosterTextPayload(
            "FMRS1 is documentation") == nullptr);
  friendmesh::ChannelRoster fromText = {};
  CHECK(friendmesh::decodeChannelRosterText(text, fromText) ==
        friendmesh::ResultCode::Ok);
  CHECK(fromText.memberCount == 2);
  text[6] = 'Z';
  CHECK(friendmesh::decodeChannelRosterText(text, fromText) ==
        friendmesh::ResultCode::InvalidText);
  encoded[0] ^= 1;
  CHECK(friendmesh::decodeChannelRoster(encoded, written, decoded) ==
        friendmesh::ResultCode::CorruptData);
}

void testBlePresenceEnvelope() {
  uint8_t pub[32] = {};
  for (size_t i = 0; i < sizeof(pub); ++i) pub[i] = (uint8_t)(i + 1);
  uint8_t payload[friendmesh::kBlePresencePayloadBytes] = {};
  CHECK(friendmesh::encodeBlePresencePayload(
      pub, payload, sizeof(payload)));
  uint8_t prefix[friendmesh::kBlePresencePrefixBytes] = {};
  CHECK(friendmesh::decodeBlePresencePayload(
      payload, sizeof(payload), prefix));
  CHECK(memcmp(prefix, pub, sizeof(prefix)) == 0);
  CHECK(!friendmesh::encodeBlePresencePayload(
      pub, payload, sizeof(payload) - 1));
  CHECK(!friendmesh::decodeBlePresencePayload(
      payload, sizeof(payload) - 1, prefix));
  payload[2] ^= 0x01;
  CHECK(!friendmesh::decodeBlePresencePayload(
      payload, sizeof(payload), prefix));
}

void testGroupCoordinationCodecAndState() {
  uint8_t alice[friendmesh::kChannelRosterPrefixBytes] = {1,2,3,4,5,6};
  uint8_t bob[friendmesh::kChannelRosterPrefixBytes] = {7,8,9,10,11,12};
  uint8_t outsider[friendmesh::kChannelRosterPrefixBytes] = {20,21,22,23,24,25};
  friendmesh::ChannelRoster roster = {};
  CHECK(friendmesh::setChannelRosterMember(
            roster, alice, friendmesh::ChannelRosterRole::Admin,
            friendmesh::ChannelRosterState::Joined) ==
        friendmesh::ResultCode::Ok);
  CHECK(friendmesh::setChannelRosterMember(
            roster, bob, friendmesh::ChannelRosterRole::Member,
            friendmesh::ChannelRosterState::Joined) ==
        friendmesh::ResultCode::Ok);

  friendmesh::GroupCoordinationEvent meetup = {};
  meetup.action = friendmesh::GroupCoordinationAction::SetMeetup;
  meetup.item.objectId[0] = 44;
  memcpy(meetup.item.ownerPrefix, alice, sizeof(alice));
  meetup.item.kind = friendmesh::GroupCoordinationKind::Meetup;
  meetup.item.status = friendmesh::GroupCoordinationStatus::Active;
  meetup.item.createdAt = 100;
  meetup.item.updatedAt = 100;
  meetup.item.expiresAt = 1000;
  meetup.item.latitudeE6 = 37774900;
  meetup.item.longitudeE6 = -122419400;
  meetup.item.hasLocation = true;
  memcpy(meetup.item.note, "Trailhead", 10);

  uint8_t wire[friendmesh::kGroupCoordinationEventMaxBytes] = {};
  size_t written = 0;
  CHECK(friendmesh::encodeGroupCoordinationEvent(
            meetup, wire, sizeof(wire), written) ==
        friendmesh::ResultCode::Ok);
  CHECK(written <= friendmesh::kGroupCoordinationEventMaxBytes);
  friendmesh::GroupCoordinationEvent decoded = {};
  CHECK(friendmesh::decodeGroupCoordinationEvent(wire, written, decoded) ==
        friendmesh::ResultCode::Ok);
  CHECK(strcmp(decoded.item.note, "Trailhead") == 0);
  const size_t eventWritten = written;

  friendmesh::GroupCoordinationState state = {};
  CHECK(friendmesh::applyGroupCoordinationEvent(
            state, decoded, roster, 100) == friendmesh::ResultCode::Ok);
  CHECK(friendmesh::groupCoordinationItemActive(state.meetup, 101));

  friendmesh::GroupCoordinationEvent response = decoded;
  response.action = friendmesh::GroupCoordinationAction::MeetupResponse;
  response.response = friendmesh::GroupCoordinationResponse::Going;
  memcpy(response.item.ownerPrefix, bob, sizeof(bob));
  response.item.updatedAt = 110;
  CHECK(friendmesh::applyGroupCoordinationEvent(
            state, response, roster, 110) == friendmesh::ResultCode::Ok);
  CHECK(state.meetupResponseCount == 1);
  CHECK(state.meetupResponses[0].response ==
        friendmesh::GroupCoordinationResponse::Going);
  const uint32_t acceptedResponseTime = state.meetup.updatedAt;
  CHECK(friendmesh::applyGroupCoordinationEvent(
            state, response, roster, 110) == friendmesh::ResultCode::Duplicate);
  CHECK(state.meetup.updatedAt == acceptedResponseTime);

  friendmesh::GroupCoordinationEvent concurrent = meetup;
  concurrent.item.objectId[0] = 45;
  concurrent.item.updatedAt = 110;
  friendmesh::GroupCoordinationState concurrentState = state;
  CHECK(friendmesh::applyGroupCoordinationEvent(
            concurrentState, concurrent, roster, 110) ==
        friendmesh::ResultCode::Ok);
  concurrent.item.objectId[0] = 43;
  CHECK(friendmesh::applyGroupCoordinationEvent(
            concurrentState, concurrent, roster, 110) ==
        friendmesh::ResultCode::Conflict);
  CHECK(concurrentState.meetup.objectId[0] == 45);

  memcpy(response.item.ownerPrefix, outsider, sizeof(outsider));
  response.item.updatedAt = 111;
  CHECK(friendmesh::applyGroupCoordinationEvent(
            state, response, roster, 111) ==
        friendmesh::ResultCode::Unauthorized);

  uint8_t saved[friendmesh::kGroupCoordinationStateMaxBytes] = {};
  CHECK(friendmesh::encodeGroupCoordinationState(
            state, saved, sizeof(saved), written) ==
        friendmesh::ResultCode::Ok);
  friendmesh::GroupCoordinationState restored = {};
  CHECK(friendmesh::decodeGroupCoordinationState(saved, written, restored) ==
        friendmesh::ResultCode::Ok);
  CHECK(restored.meetupResponseCount == 1);
  CHECK(strcmp(restored.meetup.note, "Trailhead") == 0);
  CHECK(friendmesh::expireGroupCoordinationState(restored, 1000) == 1);
  CHECK(!friendmesh::groupCoordinationItemActive(restored.meetup, 1000));

  wire[0] ^= 1;
  CHECK(friendmesh::decodeGroupCoordinationEvent(
            wire, eventWritten, decoded) ==
        friendmesh::ResultCode::CorruptData);

  // Exercise the exact persistent-state upper bound: two records and all
  // response slots occupied. Prefixes stay nonzero and unique per list.
  restored.incident = meetup.item;
  restored.incident.objectId[0] = 55;
  restored.incident.kind = friendmesh::GroupCoordinationKind::HelpRide;
  restored.incident.status = friendmesh::GroupCoordinationStatus::Active;
  restored.incident.updatedAt = 200;
  restored.meetup.status = friendmesh::GroupCoordinationStatus::Active;
  restored.meetup.updatedAt = 200;
  restored.meetupResponseCount = friendmesh::kCoordinationMaxResponses;
  restored.incidentResponseCount = friendmesh::kCoordinationMaxResponses;
  for (size_t i = 0; i < friendmesh::kCoordinationMaxResponses; ++i) {
    restored.meetupResponses[i].memberPrefix[0] =
        static_cast<uint8_t>(i + 1);
    restored.meetupResponses[i].response =
        friendmesh::GroupCoordinationResponse::Going;
    restored.incidentResponses[i].memberPrefix[0] =
        static_cast<uint8_t>(i + 21);
    restored.incidentResponses[i].response =
        friendmesh::GroupCoordinationResponse::Arrived;
  }
  CHECK(friendmesh::encodeGroupCoordinationState(
            restored, saved, sizeof(saved), written) ==
        friendmesh::ResultCode::Ok);
  CHECK(written == 244);
  CHECK(friendmesh::decodeGroupCoordinationState(saved, written, state) ==
        friendmesh::ResultCode::Ok);
  saved[244 - 7] = saved[244 - 14];
  CHECK(friendmesh::decodeGroupCoordinationState(saved, written, state) ==
        friendmesh::ResultCode::CorruptData);
}

void testMeshCorePositionAdapter() {
  friendmesh::MeshCorePositionInput input = {};
  for (size_t i = 0; i < sizeof(input.publicKey); ++i)
    input.publicKey[i] = static_cast<uint8_t>(i + 1);
  input.latitudeE6 = 37774900;
  input.longitudeE6 = -122419400;
  input.observedAt = 100;
  input.receivedAt = 110;
  friendmesh::PositionRecord output = {};
  CHECK(friendmesh::adaptMeshCorePosition(input, output) ==
        friendmesh::ResultCode::Ok);
  CHECK(output.latitudeE7 == 377749000);
  CHECK(output.longitudeE7 == -1224194000);
  CHECK(output.capturedAt == 100);
  CHECK(output.receivedAt == 110);
  CHECK(output.source == friendmesh::PositionSource::LastKnown);
  CHECK(memcmp(output.subjectId.bytes, input.publicKey,
               friendmesh::kIdSize) == 0);
  input.latitudeE6 = 0;
  input.longitudeE6 = 0;
  CHECK(friendmesh::adaptMeshCorePosition(input, output) ==
        friendmesh::ResultCode::InvalidArgument);
  input.latitudeE6 = 91000000;
  input.longitudeE6 = 1;
  CHECK(friendmesh::adaptMeshCorePosition(input, output) ==
        friendmesh::ResultCode::InvalidArgument);
}

void testFriendRequestEnvelopeAndPath() {
  friendmesh::FriendRequestEnvelope request = {};
  for (size_t i = 0; i < sizeof(request.requestId); ++i)
    request.requestId[i] = static_cast<uint8_t>(i + 1);
  for (size_t i = 0; i < sizeof(request.targetMessageHash); ++i)
    request.targetMessageHash[i] = static_cast<uint8_t>(i + 21);
  for (size_t i = 0; i < sizeof(request.requesterPublicKey); ++i)
    request.requesterPublicKey[i] = static_cast<uint8_t>(i + 41);
  request.createdAt = 1800000000UL;
  request.expiresAt = request.createdAt + 3600;
  strcpy(request.requesterName, "Tyler");
  request.returnPathLength = static_cast<uint8_t>(0x40 | 3);
  const uint8_t requestReturnPath[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  memcpy(request.returnPath, requestReturnPath, sizeof(requestReturnPath));
  for (size_t i = 0; i < sizeof(request.signature); ++i)
    request.signature[i] = static_cast<uint8_t>(i + 81);

  uint8_t wire[friendmesh::kFriendRequestEncodedBytes] = {};
  size_t written = 0;
  CHECK(friendmesh::encodeFriendRequest(request, wire, sizeof(wire), written) ==
        friendmesh::ResultCode::Ok);
  CHECK(written == friendmesh::kFriendRequestEncodedBytes);
  CHECK(written <= 165);
  friendmesh::FriendRequestEnvelope decoded = {};
  CHECK(friendmesh::decodeFriendRequest(wire, written, decoded) ==
        friendmesh::ResultCode::Ok);
  CHECK(memcmp(decoded.requestId, request.requestId,
               sizeof(request.requestId)) == 0);
  CHECK(memcmp(decoded.targetMessageHash, request.targetMessageHash,
               sizeof(request.targetMessageHash)) == 0);
  CHECK(strcmp(decoded.requesterName, "Tyler") == 0);
  CHECK(decoded.returnPathLength == request.returnPathLength);
  CHECK(memcmp(decoded.returnPath, requestReturnPath,
               sizeof(requestReturnPath)) == 0);
  uint8_t targetPub[friendmesh::kFriendRequestPublicKeyBytes] = {};
  for (size_t i = 0; i < sizeof(targetPub); ++i)
    targetPub[i] = static_cast<uint8_t>(0xA0 + i);
  request.flags = friendmesh::kFriendRequestFlagNearbyBle;
  friendmesh::makeNearbyFriendTargetHash(
      targetPub, request.targetMessageHash);
  request.returnPathLength = 0;
  memset(request.returnPath, 0, sizeof(request.returnPath));
  CHECK(friendmesh::encodeFriendRequest(request, wire, sizeof(wire), written) ==
        friendmesh::ResultCode::Ok);
  CHECK(friendmesh::decodeFriendRequest(wire, written, decoded) ==
        friendmesh::ResultCode::Ok);
  CHECK(decoded.flags == friendmesh::kFriendRequestFlagNearbyBle);
  CHECK(memcmp(decoded.targetMessageHash, targetPub,
               friendmesh::kFriendRequestTargetHashBytes) == 0);
  request.flags = 0x80;
  CHECK(friendmesh::encodeFriendRequest(request, wire, sizeof(wire), written) ==
        friendmesh::ResultCode::InvalidArgument);
  request.flags = friendmesh::kFriendRequestFlagNearbyBle;
  CHECK(friendmesh::encodeFriendRequest(request, wire, sizeof(wire), written) ==
        friendmesh::ResultCode::Ok);
  wire[0] ^= 1;
  CHECK(friendmesh::decodeFriendRequest(wire, written, decoded) ==
        friendmesh::ResultCode::CorruptData);

  friendmesh::FriendAcceptEnvelope accepted = {};
  memcpy(accepted.requestId, request.requestId, sizeof(accepted.requestId));
  strcpy(accepted.responderName, "Friend");
  uint8_t acceptWire[friendmesh::kFriendAcceptEncodedBytes] = {};
  CHECK(friendmesh::encodeFriendAccept(accepted, acceptWire,
                                      sizeof(acceptWire), written) ==
        friendmesh::ResultCode::Ok);
  CHECK(written == friendmesh::kFriendAcceptEncodedBytes);
  friendmesh::FriendAcceptEnvelope acceptedDecoded = {};
  CHECK(friendmesh::decodeFriendAccept(acceptWire, written, acceptedDecoded) ==
        friendmesh::ResultCode::Ok);
  CHECK(strcmp(acceptedDecoded.responderName, "Friend") == 0);
  uint8_t paddedAccept[friendmesh::kFriendAcceptEncodedBytes + 16] = {};
  memcpy(paddedAccept, acceptWire, friendmesh::kFriendAcceptEncodedBytes);
  CHECK(friendmesh::decodeFriendAccept(
            paddedAccept, friendmesh::kFriendAcceptEncodedBytes + 7,
            acceptedDecoded) == friendmesh::ResultCode::Ok);
  paddedAccept[friendmesh::kFriendAcceptEncodedBytes + 2] = 1;
  CHECK(friendmesh::decodeFriendAccept(
            paddedAccept, friendmesh::kFriendAcceptEncodedBytes + 7,
            acceptedDecoded) == friendmesh::ResultCode::CorruptData);
  paddedAccept[friendmesh::kFriendAcceptEncodedBytes + 2] = 0;
  CHECK(friendmesh::decodeFriendAccept(
            paddedAccept, friendmesh::kFriendAcceptEncodedBytes + 16,
            acceptedDecoded) == friendmesh::ResultCode::CorruptData);

  friendmesh::FriendLinkEnvelope link = {};
  link.action = friendmesh::FriendLinkAction::Accepted;
  memcpy(link.requestId, request.requestId, sizeof(link.requestId));
  strcpy(link.peerName, "Friend");
  uint8_t linkWire[friendmesh::kFriendLinkEncodedBytes] = {};
  CHECK(friendmesh::encodeFriendLink(link, linkWire, sizeof(linkWire), written) ==
        friendmesh::ResultCode::Ok);
  CHECK(written == friendmesh::kFriendLinkEncodedBytes);
  friendmesh::FriendLinkEnvelope decodedLink = {};
  CHECK(friendmesh::decodeFriendLink(linkWire, written, decodedLink) ==
        friendmesh::ResultCode::Ok);
  CHECK(decodedLink.action == friendmesh::FriendLinkAction::Accepted);
  CHECK(memcmp(decodedLink.requestId, request.requestId,
               sizeof(request.requestId)) == 0);
  uint8_t paddedLink[friendmesh::kFriendLinkEncodedBytes + 16] = {};
  memcpy(paddedLink, linkWire, friendmesh::kFriendLinkEncodedBytes);
  CHECK(friendmesh::decodeFriendLink(
            paddedLink, friendmesh::kFriendLinkEncodedBytes + 6,
            decodedLink) == friendmesh::ResultCode::Ok);
  paddedLink[friendmesh::kFriendLinkEncodedBytes] = 1;
  CHECK(friendmesh::decodeFriendLink(
            paddedLink, friendmesh::kFriendLinkEncodedBytes + 6,
            decodedLink) == friendmesh::ResultCode::CorruptData);
  paddedLink[friendmesh::kFriendLinkEncodedBytes] = 0;
  CHECK(friendmesh::decodeFriendLink(
            paddedLink, friendmesh::kFriendLinkEncodedBytes + 16,
            decodedLink) == friendmesh::ResultCode::CorruptData);

  link = {};
  link.action = friendmesh::FriendLinkAction::Removed;
  strcpy(link.peerName, "Former friend");
  CHECK(friendmesh::encodeFriendLink(link, linkWire, sizeof(linkWire), written) ==
        friendmesh::ResultCode::Ok);
  CHECK(friendmesh::decodeFriendLink(linkWire, written, decodedLink) ==
        friendmesh::ResultCode::Ok);
  CHECK(decodedLink.action == friendmesh::FriendLinkAction::Removed);
  link.requestId[0] = 1;
  CHECK(friendmesh::encodeFriendLink(link, linkWire, sizeof(linkWire), written) ==
        friendmesh::ResultCode::Ok);
  CHECK(friendmesh::decodeFriendLink(linkWire, written, decodedLink) ==
        friendmesh::ResultCode::Ok);
  CHECK(decodedLink.requestId[0] == 1);

  link.action = friendmesh::FriendLinkAction::Acknowledged;
  CHECK(friendmesh::encodeFriendLink(link, linkWire, sizeof(linkWire), written) ==
        friendmesh::ResultCode::Ok);
  CHECK(friendmesh::decodeFriendLink(linkWire, written, decodedLink) ==
        friendmesh::ResultCode::Ok);
  CHECK(decodedLink.action == friendmesh::FriendLinkAction::Acknowledged);
  memset(link.requestId, 0, sizeof(link.requestId));
  CHECK(friendmesh::encodeFriendLink(link, linkWire, sizeof(linkWire), written) ==
        friendmesh::ResultCode::InvalidArgument);

  // Three 2-byte repeater hashes: AABB, CCDD, EEFF -> reverse order.
  const uint8_t sourcePath[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  uint8_t reversed[sizeof(sourcePath)] = {};
  const uint8_t encodedPathLength = static_cast<uint8_t>(0x40 | 3);
  CHECK(friendmesh::reverseMeshPath(sourcePath, encodedPathLength, reversed,
                                    sizeof(reversed)) == sizeof(reversed));
  const uint8_t expected[] = {0xEE, 0xFF, 0xCC, 0xDD, 0xAA, 0xBB};
  CHECK(memcmp(reversed, expected, sizeof(expected)) == 0);
  CHECK(friendmesh::reverseMeshPath(sourcePath, encodedPathLength, reversed, 5) == 0);
}

}  // namespace

int main() {
  testCoreTypes();
  testEveryFeatureFamilyHasPolicy();
  testSharedFeatureModels();
  testDomainStore();
  testDevelopmentIdentityLifecycle();
  testTrustedContactRepository();
  testMembershipLifecycle();
  testMembershipBounds();
  testDevelopmentChatStorage();
  testFragmentation();
  testChatLifecycleAndOutbox();
  testChatRebootRecovery();
  testSynchronization();
  testPositionsAndNavigation();
  testMarkersAndMeetups();
  testSafetyIncidentsAndNotifications();
  testDevelopmentRuntimeWalkthrough();
  testEventHistory();
  testOutbox();
  testFeatureServiceSafetyGate();
  testFeatureCoordinator();
  testDirectChannelInviteEnvelope();
  testChannelRoster();
  testBlePresenceEnvelope();
  testGroupCoordinationCodecAndState();
  testMeshCorePositionAdapter();
  testFriendRequestEnvelopeAndPath();

  if (failures != 0) {
    fprintf(stderr, "%d FriendMesh core check(s) failed\n", failures);
    return 1;
  }
  printf("FriendMesh shared-core checks passed\n");
  return 0;
}
