#pragma once

#include "friendmesh/chat/FriendMeshChat.h"
#include "friendmesh/navigation/FriendMeshNavigation.h"
#include "friendmesh/safety/FriendMeshSafety.h"

namespace friendmesh {

struct DevelopmentRuntimeSnapshot {
  bool initialized;
  size_t messages;
  size_t markers;
  size_t meetups;
  size_t incidents;
  size_t notifications;
  uint32_t nextSequence;
};

// Local-only composition used to exercise completed feature behavior on a
// T-Deck before production persistence, identity crypto, or radio is enabled.
class DevelopmentRuntime {
 public:
  DevelopmentRuntime();

  ResultCode initialize(const char* ownerLabel, uint32_t now);
  ResultCode addLocalMessage(const char* text, uint32_t now);
  ResultCode addLocalMarker(int32_t latitudeE7, int32_t longitudeE7,
                            MarkerType type, uint32_t now);
  ResultCode openLocalHelp(IncidentKind kind, int32_t latitudeE7,
                           int32_t longitudeE7, bool locationValid,
                           uint32_t now);
  DevelopmentRuntimeSnapshot snapshot() const;

  const DomainStore& domain() const { return domain_; }
  const ChatService& chat() const { return chat_; }
  const MarkerService& markers() const { return markers_; }
  const SafetyService& safety() const { return safety_; }
  const Id128& ownerId() const { return ownerId_; }
  const Id128& groupId() const { return groupId_; }

 private:
  DomainStore domain_;
  DevelopmentPayloadStorage payloads_;
  DevelopmentEventJournal journal_;
  DevelopmentOutboxStore outboxStore_;
  ChatService chat_;
  PositionBook positions_;
  NavigationService navigation_;
  MarkerService markers_;
  MeetupService meetups_;
  SafetyNotificationCenter notifications_;
  SafetyService safety_;
  Id128 ownerId_;
  Id128 groupId_;
  uint32_t nextSequence_;
  uint32_t nextPayloadHandle_;
  uint8_t nextEventSeed_;
  bool initialized_;

  EventHeader event(EventType type, uint32_t payloadLength, uint32_t now,
                    uint32_t expiresAt = 0);
};

void setDevelopmentRuntime(DevelopmentRuntime* runtime);
DevelopmentRuntime* developmentRuntime();

}  // namespace friendmesh
