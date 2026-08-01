#include "FriendMeshSafety.h"

#include <string.h>

namespace friendmesh {

namespace {

bool closedState(IncidentState state) {
  return state == IncidentState::ClosedFalseAlarm ||
         state == IncidentState::ClosedSafe ||
         state == IncidentState::ClosedHelpArrived ||
         state == IncidentState::Expired;
}

bool helpKind(IncidentKind kind) {
  return kind != IncidentKind::Sos;
}

LocationAvailability locationAvailability(const PositionRecord& position) {
  if (!position.valid || position.hiddenByPolicy ||
      position.latitudeE7 < -900000000 || position.latitudeE7 > 900000000 ||
      position.longitudeE7 < -1800000000 ||
      position.longitudeE7 > 1800000000) {
    return LocationAvailability::Missing;
  }
  return position.source == PositionSource::LastKnown
             ? LocationAvailability::LastKnown
             : LocationAvailability::Current;
}

}  // namespace

SafetyNotificationCenter::SafetyNotificationCenter() : count_(0) {
  memset(entries_, 0, sizeof(entries_));
}

ResultCode SafetyNotificationCenter::push(const IncidentRecord& incident,
                                          EventPriority priority,
                                          uint32_t createdAt) {
  for (size_t i = 0; i < count_; ++i) {
    if (idsEqual(entries_[i].incidentId, incident.incidentId) &&
        entries_[i].state == incident.state) return ResultCode::Duplicate;
  }
  SafetyNotification next = {};
  next.incidentId = incident.incidentId;
  next.kind = incident.kind;
  next.state = incident.state;
  next.priority = priority;
  next.createdAt = createdAt;
  if (count_ < kMaxSafetyNotifications) {
    entries_[count_++] = next;
    return ResultCode::Ok;
  }
  size_t replace = kMaxSafetyNotifications;
  for (size_t i = 0; i < count_; ++i) {
    if (entries_[i].acknowledged) {
      replace = i;
      break;
    }
  }
  if (replace == kMaxSafetyNotifications) return ResultCode::CapacityReached;
  entries_[replace] = next;
  return ResultCode::Ok;
}

ResultCode SafetyNotificationCenter::acknowledge(const Id128& incidentId) {
  bool found = false;
  for (size_t i = 0; i < count_; ++i) {
    if (idsEqual(entries_[i].incidentId, incidentId)) {
      entries_[i].acknowledged = true;
      found = true;
    }
  }
  return found ? ResultCode::Ok : ResultCode::NotFound;
}

const SafetyNotification* SafetyNotificationCenter::at(size_t index) const {
  return index < count_ ? &entries_[index] : nullptr;
}

size_t SafetyNotificationCenter::unacknowledgedCount() const {
  size_t count = 0;
  for (size_t i = 0; i < count_; ++i) {
    if (!entries_[i].acknowledged) ++count;
  }
  return count;
}

SafetyService::SafetyService(const DomainStore& domain,
                             SafetyNotificationCenter& notifications)
    : domain_(domain),
      notifications_(notifications),
      count_(0),
      holdStartedAt_(0),
      holdPending_(false) {
  memset(incidents_, 0, sizeof(incidents_));
  memset(&holdDraft_, 0, sizeof(holdDraft_));
  memset(&holdGroupId_, 0, sizeof(holdGroupId_));
}

bool SafetyService::actorAllowed(const Id128& groupId,
                                 const Id128& actorId) const {
  const FriendRecord* actor = domain_.friendById(actorId);
  if (!actor || actor->blockedLocally) return false;
  if (idIsZero(groupId)) return actor->emergencyContactAllowed;
  const GroupRecord* group = domain_.groupById(groupId);
  const GroupMember* member = domain_.groupMember(groupId, actorId);
  return group && member && group->security == GroupSecurityState::ReadyForDevelopment &&
         member->state == MemberState::Approved &&
         member->grantState == GrantState::Active &&
         member->grantedEpoch == group->membershipEpoch;
}

IncidentEntry* SafetyService::mutableById(const Id128& incidentId) {
  for (size_t i = 0; i < count_; ++i) {
    if (idsEqual(incidents_[i].incident.incidentId, incidentId)) {
      return &incidents_[i];
    }
  }
  return nullptr;
}

const IncidentEntry* SafetyService::byId(const Id128& incidentId) const {
  for (size_t i = 0; i < count_; ++i) {
    if (idsEqual(incidents_[i].incident.incidentId, incidentId)) {
      return &incidents_[i];
    }
  }
  return nullptr;
}

const IncidentEntry* SafetyService::at(size_t index) const {
  return index < count_ ? &incidents_[index] : nullptr;
}

ResultCode SafetyService::beginSosHold(const IncidentRecord& draft,
                                       const Id128& groupId,
                                       uint32_t startedAt) {
  if (holdPending_ || draft.kind != IncidentKind::Sos ||
      idIsZero(draft.incidentId) || idIsZero(draft.originatorId) ||
      byId(draft.incidentId)) return ResultCode::InvalidState;
  if (!actorAllowed(groupId, draft.originatorId)) return ResultCode::Unauthorized;
  holdDraft_ = draft;
  holdDraft_.state = IncidentState::Holding;
  holdGroupId_ = groupId;
  holdStartedAt_ = startedAt;
  holdPending_ = true;
  return ResultCode::Ok;
}

ResultCode SafetyService::activate(const IncidentRecord& draft,
                                   const Id128& groupId,
                                   ResultCode persistenceResult,
                                   uint32_t activatedAt,
                                   EventPriority priority) {
  if (byId(draft.incidentId)) return ResultCode::Duplicate;
  if (count_ >= kMaxIncidents) return ResultCode::CapacityReached;
  IncidentEntry& next = incidents_[count_++];
  next = {};
  next.incident = draft;
  next.incident.state = IncidentState::Active;
  next.incident.openedAt = activatedAt;
  next.incident.updatedAt = activatedAt;
  next.incident.persistenceFailed = persistenceResult != ResultCode::Ok;
  next.groupId = groupId;
  next.locationAvailability = locationAvailability(draft.position);
  notifications_.push(next.incident, priority, activatedAt);
  return persistenceResult == ResultCode::Ok ? ResultCode::Ok
                                              : ResultCode::Incomplete;
}

ResultCode SafetyService::updateSosHold(uint32_t now, bool stillHeld,
                                        ResultCode persistenceResult) {
  if (!holdPending_) return ResultCode::InvalidState;
  if (!stillHeld) {
    holdPending_ = false;
    return ResultCode::Incomplete;
  }
  if (now < holdStartedAt_ || now - holdStartedAt_ < kSosHoldSeconds) {
    return ResultCode::Incomplete;
  }
  const ResultCode result = activate(holdDraft_, holdGroupId_, persistenceResult,
                                     now, EventPriority::Safety);
  holdPending_ = false;
  return result;
}

ResultCode SafetyService::openHelp(const EventHeader& event,
                                   const IncidentRecord& draft,
                                   const Id128& groupId,
                                   ResultCode persistenceResult,
                                   uint32_t receivedAt) {
  if (event.type != EventType::HelpOpened || !helpKind(draft.kind) ||
      !idsEqual(event.eventId, draft.incidentId) ||
      !idsEqual(event.senderId, draft.originatorId) ||
      !idsEqual(event.groupId, groupId) ||
      validateEventHeader(event) != ResultCode::Ok) {
    return ResultCode::InvalidArgument;
  }
  if (!actorAllowed(groupId, event.senderId)) return ResultCode::Unauthorized;
  if (appliedEvents_.contains(event.eventId)) return ResultCode::Duplicate;
  const ResultCode applied = appliedEvents_.append(event);
  if (applied != ResultCode::Ok) return applied;
  return activate(draft, groupId, persistenceResult, receivedAt,
                  EventPriority::Elevated);
}

void SafetyService::recount(IncidentEntry& entry) {
  entry.incident.deliveredCount = 0;
  entry.incident.respondingCount = 0;
  for (size_t i = 0; i < entry.responderCount; ++i) {
    const ResponderState state = entry.responders[i].state;
    if (state != ResponderState::None) ++entry.incident.deliveredCount;
    if (state == ResponderState::Responding || state == ResponderState::Arrived) {
      ++entry.incident.respondingCount;
    }
  }
}

ResultCode SafetyService::applyStatus(const EventHeader& event,
                                      const Id128& incidentId,
                                      ResponderState state,
                                      uint32_t receivedAt) {
  if ((event.type != EventType::SosStatusChanged &&
       event.type != EventType::HelpStatusChanged) ||
      state == ResponderState::None) return ResultCode::InvalidArgument;
  IncidentEntry* entry = mutableById(incidentId);
  if (!entry) return ResultCode::NotFound;
  if (!idsEqual(event.groupId, entry->groupId) ||
      validateEventHeader(event) != ResultCode::Ok) {
    return ResultCode::InvalidArgument;
  }
  if (closedState(entry->incident.state)) return ResultCode::InvalidState;
  if (!actorAllowed(entry->groupId, event.senderId)) return ResultCode::Unauthorized;
  if (appliedEvents_.contains(event.eventId)) return ResultCode::Duplicate;
  const ResultCode applied = appliedEvents_.append(event);
  if (applied != ResultCode::Ok) return applied;
  for (size_t i = 0; i < entry->responderCount; ++i) {
    if (!idsEqual(entry->responders[i].friendId, event.senderId)) continue;
    entry->responders[i].state = state;
    entry->responders[i].updatedAt = receivedAt;
    entry->incident.updatedAt = receivedAt;
    recount(*entry);
    return ResultCode::Ok;
  }
  if (entry->responderCount >= kMaxIncidentResponders) {
    return ResultCode::CapacityReached;
  }
  IncidentResponder& responder = entry->responders[entry->responderCount++];
  responder.friendId = event.senderId;
  responder.state = state;
  responder.updatedAt = receivedAt;
  entry->incident.updatedAt = receivedAt;
  recount(*entry);
  return ResultCode::Ok;
}

ResultCode SafetyService::beginCancel(const Id128& incidentId,
                                      const Id128& actorId, uint32_t now) {
  IncidentEntry* entry = mutableById(incidentId);
  if (!entry) return ResultCode::NotFound;
  if (!idsEqual(entry->incident.originatorId, actorId)) return ResultCode::Unauthorized;
  if (closedState(entry->incident.state)) return ResultCode::InvalidState;
  entry->incident.state = IncidentState::CancelCountdown;
  entry->incident.updatedAt = now;
  entry->cancelDeadline = now + kSosCancelCountdownSeconds;
  return ResultCode::Ok;
}

ResultCode SafetyService::abortCancel(const Id128& incidentId,
                                      const Id128& actorId) {
  IncidentEntry* entry = mutableById(incidentId);
  if (!entry) return ResultCode::NotFound;
  if (!idsEqual(entry->incident.originatorId, actorId)) return ResultCode::Unauthorized;
  if (entry->incident.state != IncidentState::CancelCountdown) {
    return ResultCode::InvalidState;
  }
  entry->incident.state = IncidentState::Active;
  entry->cancelDeadline = 0;
  return ResultCode::Ok;
}

ResultCode SafetyService::close(const EventHeader& event,
                                const Id128& incidentId,
                                IncidentState nextState,
                                uint32_t receivedAt) {
  if ((event.type != EventType::SosClosed &&
       event.type != EventType::HelpClosed) || !closedState(nextState) ||
      nextState == IncidentState::Expired) return ResultCode::InvalidArgument;
  IncidentEntry* entry = mutableById(incidentId);
  if (!entry) return ResultCode::NotFound;
  if (!idsEqual(event.groupId, entry->groupId) ||
      validateEventHeader(event) != ResultCode::Ok) {
    return ResultCode::InvalidArgument;
  }
  if (closedState(entry->incident.state)) return ResultCode::Duplicate;
  const GroupMember* member = idIsZero(entry->groupId)
                                  ? nullptr
                                  : domain_.groupMember(entry->groupId,
                                                        event.senderId);
  if (!idsEqual(entry->incident.originatorId, event.senderId) &&
      (!member || member->role != MemberRole::Admin)) {
    return ResultCode::Unauthorized;
  }
  if (appliedEvents_.contains(event.eventId)) return ResultCode::Duplicate;
  const ResultCode applied = appliedEvents_.append(event);
  if (applied != ResultCode::Ok) return applied;
  entry->incident.state = nextState;
  entry->incident.updatedAt = receivedAt;
  entry->incident.closedAt = receivedAt;
  entry->cancelDeadline = 0;
  notifications_.push(entry->incident,
                      entry->incident.kind == IncidentKind::Sos
                          ? EventPriority::Safety
                          : EventPriority::Elevated,
                      receivedAt);
  return ResultCode::Ok;
}

ResultCode SafetyService::approvePublicFallback(const Id128& incidentId,
                                                const Id128& actorId,
                                                bool approved) {
  IncidentEntry* entry = mutableById(incidentId);
  if (!entry) return ResultCode::NotFound;
  if (!idsEqual(entry->incident.originatorId, actorId)) return ResultCode::Unauthorized;
  entry->incident.publicFallbackApproved = approved;
  return ResultCode::Ok;
}

size_t SafetyService::processTime(uint32_t now) {
  size_t changed = 0;
  for (size_t i = 0; i < count_; ++i) {
    IncidentEntry& entry = incidents_[i];
    if (entry.incident.state == IncidentState::CancelCountdown &&
        now >= entry.cancelDeadline) {
      entry.incident.state = IncidentState::ClosedFalseAlarm;
      entry.incident.closedAt = now;
      entry.incident.updatedAt = now;
      entry.cancelDeadline = 0;
      ++changed;
    }
  }
  return changed;
}

}  // namespace friendmesh
