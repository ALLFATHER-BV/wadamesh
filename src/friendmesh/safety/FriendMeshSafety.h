#pragma once

#include "friendmesh/core/FriendMeshDomain.h"
#include "friendmesh/core/FriendMeshEventHistory.h"
#include "friendmesh/core/FriendMeshFeatureModels.h"

namespace friendmesh {

constexpr size_t kMaxIncidents = 16;
constexpr size_t kMaxIncidentResponders = kMaxGroupMembers;
constexpr size_t kMaxSafetyNotifications = 32;
constexpr uint32_t kSosHoldSeconds = 3;
constexpr uint32_t kSosCancelCountdownSeconds = 5;

enum class ResponderState : uint8_t {
  None = 0,
  Delivered,
  Responding,
  Unable,
  Arrived,
};

enum class LocationAvailability : uint8_t {
  Missing = 0,
  LastKnown,
  Current,
};

struct IncidentResponder {
  Id128 friendId;
  ResponderState state;
  uint32_t updatedAt;
};

struct IncidentEntry {
  IncidentRecord incident;
  Id128 groupId;
  IncidentResponder responders[kMaxIncidentResponders];
  uint8_t responderCount;
  LocationAvailability locationAvailability;
  uint32_t cancelDeadline;
};

struct SafetyNotification {
  Id128 incidentId;
  IncidentKind kind;
  IncidentState state;
  EventPriority priority;
  uint32_t createdAt;
  bool acknowledged;
};

class SafetyNotificationCenter {
 public:
  SafetyNotificationCenter();

  ResultCode push(const IncidentRecord& incident, EventPriority priority,
                  uint32_t createdAt);
  ResultCode acknowledge(const Id128& incidentId);
  const SafetyNotification* at(size_t index) const;
  size_t size() const { return count_; }
  size_t unacknowledgedCount() const;

 private:
  SafetyNotification entries_[kMaxSafetyNotifications];
  size_t count_;
};

class SafetyService {
 public:
  SafetyService(const DomainStore& domain, SafetyNotificationCenter& notifications);

  ResultCode beginSosHold(const IncidentRecord& draft, const Id128& groupId,
                          uint32_t startedAt);
  ResultCode updateSosHold(uint32_t now, bool stillHeld,
                           ResultCode persistenceResult);
  ResultCode openHelp(const EventHeader& event, const IncidentRecord& draft,
                      const Id128& groupId, ResultCode persistenceResult,
                      uint32_t receivedAt);
  ResultCode applyStatus(const EventHeader& event, const Id128& incidentId,
                         ResponderState state, uint32_t receivedAt);
  ResultCode beginCancel(const Id128& incidentId, const Id128& actorId,
                         uint32_t now);
  ResultCode abortCancel(const Id128& incidentId, const Id128& actorId);
  ResultCode close(const EventHeader& event, const Id128& incidentId,
                   IncidentState closedState, uint32_t receivedAt);
  ResultCode approvePublicFallback(const Id128& incidentId,
                                   const Id128& actorId, bool approved);
  size_t processTime(uint32_t now);

  const IncidentEntry* byId(const Id128& incidentId) const;
  const IncidentEntry* at(size_t index) const;
  size_t size() const { return count_; }
  bool holdPending() const { return holdPending_; }

 private:
  const DomainStore& domain_;
  SafetyNotificationCenter& notifications_;
  IncidentEntry incidents_[kMaxIncidents];
  EventHistory<kMaxHistoryEntries> appliedEvents_;
  size_t count_;
  IncidentRecord holdDraft_;
  Id128 holdGroupId_;
  uint32_t holdStartedAt_;
  bool holdPending_;

  bool actorAllowed(const Id128& groupId, const Id128& actorId) const;
  ResultCode activate(const IncidentRecord& draft, const Id128& groupId,
                      ResultCode persistenceResult, uint32_t activatedAt,
                      EventPriority priority);
  IncidentEntry* mutableById(const Id128& incidentId);
  void recount(IncidentEntry& entry);
};

}  // namespace friendmesh
