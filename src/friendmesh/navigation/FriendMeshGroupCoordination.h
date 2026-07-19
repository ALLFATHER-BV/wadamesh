#pragma once

#include <stddef.h>
#include <stdint.h>

#include "friendmesh/core/FriendMeshCoreTypes.h"
#include "friendmesh/people/FriendMeshChannelRoster.h"

namespace friendmesh {

// MeshCore's unregistered development range. Coordination records are binary
// encrypted group datagrams and never pass through channel chat text.
constexpr uint16_t kGroupCoordinationDataType = 0xFF02;
constexpr size_t kCoordinationObjectIdBytes = 8;
constexpr size_t kCoordinationNoteBytes = 24;
constexpr size_t kCoordinationMaxResponses = kChannelRosterMaxMembers;
constexpr size_t kGroupCoordinationEventMaxBytes = 72;
// Worst case is two 62-byte records plus sixteen 7-byte responses and the
// 8-byte header (244 bytes). Keep a small explicit margin while remaining
// below DataStore's uint8_t blob-length limit.
constexpr size_t kGroupCoordinationStateMaxBytes = 248;

enum class GroupCoordinationAction : uint8_t {
  SetMeetup = 0,
  CancelMeetup,
  MeetupResponse,
  OpenIncident,
  IncidentResponse,
  CloseIncident,
};

enum class GroupCoordinationKind : uint8_t {
  Meetup = 0,
  Pickup,
  HelpRide,
  HelpLost,
  HelpEquipment,
  HelpContact,
  Sos,
};

enum class GroupCoordinationStatus : uint8_t {
  Active = 0,
  Cancelled,
  Closed,
  Expired,
};

enum class GroupCoordinationResponse : uint8_t {
  None = 0,
  Going,
  Unable,
  Arrived,
};

struct GroupCoordinationItem {
  uint8_t objectId[kCoordinationObjectIdBytes];
  uint8_t ownerPrefix[kChannelRosterPrefixBytes];
  GroupCoordinationKind kind;
  GroupCoordinationStatus status;
  uint32_t createdAt;
  uint32_t updatedAt;
  uint32_t expiresAt;
  int32_t latitudeE6;
  int32_t longitudeE6;
  bool hasLocation;
  char note[kCoordinationNoteBytes + 1];
};

struct GroupCoordinationResponder {
  uint8_t memberPrefix[kChannelRosterPrefixBytes];
  GroupCoordinationResponse response;
};

struct GroupCoordinationState {
  GroupCoordinationItem meetup;
  GroupCoordinationItem incident;
  GroupCoordinationResponder meetupResponses[kCoordinationMaxResponses];
  GroupCoordinationResponder incidentResponses[kCoordinationMaxResponses];
  uint8_t meetupResponseCount;
  uint8_t incidentResponseCount;
};

struct GroupCoordinationEvent {
  GroupCoordinationAction action;
  GroupCoordinationItem item;
  GroupCoordinationResponse response;
};

void clearGroupCoordinationState(GroupCoordinationState& state);
bool groupCoordinationItemActive(const GroupCoordinationItem& item,
                                 uint32_t now);
ResultCode encodeGroupCoordinationEvent(const GroupCoordinationEvent& event,
                                        uint8_t* destination,
                                        size_t capacity, size_t& written);
ResultCode decodeGroupCoordinationEvent(const uint8_t* source, size_t length,
                                        GroupCoordinationEvent& event);
ResultCode encodeGroupCoordinationState(const GroupCoordinationState& state,
                                        uint8_t* destination,
                                        size_t capacity, size_t& written);
ResultCode decodeGroupCoordinationState(const uint8_t* source, size_t length,
                                        GroupCoordinationState& state);
ResultCode applyGroupCoordinationEvent(GroupCoordinationState& state,
                                       const GroupCoordinationEvent& event,
                                       const ChannelRoster& roster,
                                       uint32_t now);
size_t expireGroupCoordinationState(GroupCoordinationState& state,
                                    uint32_t now);

}  // namespace friendmesh
