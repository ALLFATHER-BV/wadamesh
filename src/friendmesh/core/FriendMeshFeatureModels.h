#pragma once

#include "FriendMeshEvent.h"

namespace friendmesh {

enum class DeliveryState : uint8_t {
  Draft = 0,
  Queued,
  Transmitting,
  RelayedOrObserved,
  Delivered,
  TimedOut,
  Failed,
  Expired,
  Deleted,
  Incomplete,
};

enum class ReactionKind : uint8_t {
  None = 0,
  ThumbsUp,
  ThumbsDown,
};

struct ChatRecord {
  Id128 eventId;
  Id128 groupId;
  Id128 senderId;
  uint32_t textHandle;
  uint32_t textLength;
  uint32_t originalCreatedAt;
  uint32_t locallyReceivedAt;
  DeliveryState delivery;
  bool delayed;
  bool senderClockUntrusted;
};

struct ReactionRecord {
  Id128 eventId;
  Id128 messageId;
  Id128 senderId;
  ReactionKind reaction;
  uint32_t createdAt;
};

struct SyncCursor {
  Id128 senderId;
  uint32_t membershipEpoch;
  uint32_t highestContiguousSequence;
  uint32_t firstMissingSequence;
  uint32_t lastUpdatedAt;
};

enum class PositionSource : uint8_t {
  Unknown = 0,
  OnDeviceGps,
  CompanionProvided,
  LastKnown,
};

struct PositionRecord {
  Id128 subjectId;
  int32_t latitudeE7;
  int32_t longitudeE7;
  uint32_t capturedAt;
  uint32_t receivedAt;
  uint16_t accuracyMeters;
  PositionSource source;
  bool valid;
  bool hiddenByPolicy;
};

enum class HeadingSource : uint8_t {
  NorthUp = 0,
  GpsCourse,
  Magnetometer,
};

struct NavigationState {
  Id128 targetId;
  PositionRecord targetPosition;
  uint32_t startedAt;
  uint32_t distanceMeters;
  uint16_t absoluteBearingDegrees;
  uint16_t headingDegrees;
  HeadingSource headingSource;
  bool targetStale;
  bool active;
};

enum class MarkerType : uint8_t {
  Meetup = 0,
  Danger,
  Avoid,
  Resource,
  Vehicle,
  Camp,
  LastSeen,
  Pickup,
  Help,
  Sos,
};

enum class MarkerState : uint8_t {
  Active = 0,
  NeedsReconfirmation,
  Removed,
  Expired,
};

struct MarkerRecord {
  Id128 markerId;
  Id128 groupId;
  Id128 creatorId;
  PositionRecord position;
  MarkerType type;
  MarkerState state;
  uint32_t createdAt;
  uint32_t expiresAt;
  uint32_t descriptionHandle;
  uint16_t descriptionLength;
};

enum class MeetupState : uint8_t {
  Proposed = 0,
  Accepted,
  Rejected,
  Active,
  Completed,
  Cancelled,
  Expired,
};

enum class VoteChoice : uint8_t {
  Abstain = 0,
  Yes,
  No,
};

struct MeetupRecord {
  Id128 meetupId;
  Id128 groupId;
  Id128 proposerId;
  PositionRecord position;
  MeetupState state;
  uint32_t proposedAt;
  uint32_t voteClosesAt;
  uint32_t expiresAt;
  uint8_t yesVotes;
  uint8_t noVotes;
  uint8_t attendingCount;
};

enum class IncidentKind : uint8_t {
  Sos = 0,
  HelpRide,
  HelpUncomfortable,
  HelpLost,
  HelpEquipment,
  HelpMedicalNonEmergency,
  HelpContactMe,
  HelpDiscreetExtraction,
  HelpFreeText,
};

enum class IncidentState : uint8_t {
  Holding = 0,
  CancelCountdown,
  Active,
  Sending,
  Delivered,
  Responding,
  Unable,
  Arrived,
  ClosedFalseAlarm,
  ClosedSafe,
  ClosedHelpArrived,
  Expired,
};

struct IncidentRecord {
  Id128 incidentId;
  Id128 originatorId;
  IncidentKind kind;
  IncidentState state;
  PositionRecord position;
  uint32_t openedAt;
  uint32_t updatedAt;
  uint32_t closedAt;
  uint32_t detailHandle;
  uint16_t detailLength;
  uint8_t deliveredCount;
  uint8_t respondingCount;
  bool publicFallbackApproved;
  bool persistenceFailed;
};

}  // namespace friendmesh
