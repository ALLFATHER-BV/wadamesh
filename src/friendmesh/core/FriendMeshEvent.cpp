#include "FriendMeshEvent.h"

namespace friendmesh {

namespace {

EventPolicy makePolicy(FeatureFamily family, EventPriority priority,
                       DurabilityRule durability, bool requiresGroup,
                       bool requiresSignature, bool requiresApprovedMember) {
  EventPolicy policy = {family, priority, durability, requiresGroup,
                        requiresSignature, requiresApprovedMember};
  return policy;
}

}  // namespace

EventPolicy policyForEvent(EventType type) {
  switch (type) {
    case EventType::IdentityCreated:
    case EventType::IdentityReplaced:
    case EventType::FriendVerified:
    case EventType::FriendBlocked:
      return makePolicy(FeatureFamily::IdentityAndTrust, EventPriority::Elevated,
                        DurabilityRule::PersistBeforeApplyAndTransmit,
                        false, true, false);

    case EventType::GroupCreated:
    case EventType::GroupRenamed:
    case EventType::InvitationOpened:
    case EventType::JoinRequested:
    case EventType::MemberApproved:
    case EventType::MemberStateChanged:
    case EventType::AdminTransferred:
    case EventType::RekeyRequested:
      return makePolicy(FeatureFamily::GroupsAndMembership, EventPriority::Elevated,
                        DurabilityRule::PersistBeforeApplyAndTransmit,
                        true, true, type != EventType::JoinRequested);

    case EventType::ChatMessage:
    case EventType::ChatReaction:
    case EventType::ChatDeleted:
      return makePolicy(FeatureFamily::ChatAndHistory, EventPriority::Normal,
                        DurabilityRule::PersistBeforeTransmit,
                        true, true, true);

    case EventType::SyncInventory:
    case EventType::SyncBatch:
    case EventType::SyncReceipt:
      return makePolicy(FeatureFamily::Synchronization, EventPriority::Background,
                        DurabilityRule::PersistBeforeTransmit,
                        true, true, true);

    case EventType::PositionShared:
    case EventType::NavigationStarted:
      return makePolicy(FeatureFamily::LocationAndNavigation, EventPriority::Normal,
                        DurabilityRule::PersistBeforeTransmit,
                        true, true, true);

    case EventType::MarkerCreated:
    case EventType::MarkerUpdated:
    case EventType::MarkerRemoved:
    case EventType::MeetupProposed:
    case EventType::MeetupVote:
    case EventType::MeetupStateChanged:
      return makePolicy(FeatureFamily::MarkersAndMeetups, EventPriority::Normal,
                        DurabilityRule::PersistBeforeTransmit,
                        true, true, true);

    case EventType::SosOpened:
    case EventType::SosStatusChanged:
    case EventType::SosClosed:
      return makePolicy(FeatureFamily::Safety, EventPriority::Safety,
                        DurabilityRule::SafetyMayBypassPersistence,
                        false, true, false);

    case EventType::HelpOpened:
    case EventType::HelpStatusChanged:
    case EventType::HelpClosed:
      return makePolicy(FeatureFamily::Safety, EventPriority::Elevated,
                        DurabilityRule::SafetyMayBypassPersistence,
                        false, true, false);

    case EventType::GroupDisbanded:
      return makePolicy(FeatureFamily::Control, EventPriority::Elevated,
                        DurabilityRule::PersistBeforeApplyAndTransmit,
                        true, true, true);

    case EventType::Count:
      break;
  }

  return makePolicy(FeatureFamily::Control, EventPriority::Background,
                    DurabilityRule::MemoryOnly, false, true, true);
}

bool eventTypeIsValid(EventType type) {
  return static_cast<uint8_t>(type) < static_cast<uint8_t>(EventType::Count);
}

bool eventRequiresGroup(EventType type) {
  return eventTypeIsValid(type) && policyForEvent(type).requiresGroup;
}

bool eventIsExpired(const EventHeader& event, uint32_t now) {
  return event.expiresAt != 0 && static_cast<int32_t>(now - event.expiresAt) >= 0;
}

ResultCode validateEventHeader(const EventHeader& event) {
  if (!eventTypeIsValid(event.type)) return ResultCode::InvalidArgument;
  if (idIsZero(event.eventId) || idIsZero(event.senderId)) return ResultCode::InvalidId;
  if (eventRequiresGroup(event.type) && idIsZero(event.groupId)) return ResultCode::InvalidId;
  if (event.payloadLength > kMaxEventPayloadBytes) return ResultCode::CapacityReached;
  if (event.payloadLength > 0 && event.payloadHandle == 0) return ResultCode::InvalidArgument;
  return ResultCode::Ok;
}

}  // namespace friendmesh
