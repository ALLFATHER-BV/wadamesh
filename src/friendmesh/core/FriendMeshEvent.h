#pragma once

#include "FriendMeshCoreTypes.h"

namespace friendmesh {

enum class FeatureFamily : uint8_t {
  IdentityAndTrust = 0,
  GroupsAndMembership,
  ChatAndHistory,
  Synchronization,
  LocationAndNavigation,
  MarkersAndMeetups,
  Safety,
  Control,
};

enum class EventType : uint8_t {
  IdentityCreated = 0,
  IdentityReplaced,
  FriendVerified,
  FriendBlocked,
  GroupCreated,
  GroupRenamed,
  InvitationOpened,
  JoinRequested,
  MemberApproved,
  MemberStateChanged,
  AdminTransferred,
  RekeyRequested,
  ChatMessage,
  ChatReaction,
  ChatDeleted,
  SyncInventory,
  SyncBatch,
  SyncReceipt,
  PositionShared,
  NavigationStarted,
  MarkerCreated,
  MarkerUpdated,
  MarkerRemoved,
  MeetupProposed,
  MeetupVote,
  MeetupStateChanged,
  SosOpened,
  SosStatusChanged,
  SosClosed,
  HelpOpened,
  HelpStatusChanged,
  HelpClosed,
  GroupDisbanded,
  Count,
};

enum class EventPriority : uint8_t {
  Background = 0,
  Normal,
  Elevated,
  Safety,
};

enum class DurabilityRule : uint8_t {
  MemoryOnly = 0,
  PersistBeforeTransmit,
  PersistBeforeApplyAndTransmit,
  SafetyMayBypassPersistence,
};

struct EventPolicy {
  FeatureFamily family;
  EventPriority priority;
  DurabilityRule durability;
  bool requiresGroup;
  bool requiresSignature;
  bool requiresApprovedMember;
};

struct EventHeader {
  Id128 eventId;
  Id128 groupId;
  Id128 senderId;
  EventType type;
  EventPriority priority;
  uint32_t membershipEpoch;
  uint32_t senderSequence;
  uint32_t createdAt;
  uint32_t expiresAt;
  uint32_t payloadHandle;
  uint32_t payloadLength;
  uint8_t payloadHash[32];
};

EventPolicy policyForEvent(EventType type);
bool eventTypeIsValid(EventType type);
bool eventRequiresGroup(EventType type);
bool eventIsExpired(const EventHeader& event, uint32_t now);
ResultCode validateEventHeader(const EventHeader& event);

}  // namespace friendmesh
