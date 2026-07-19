#include "FriendMeshDevelopmentRuntime.h"

#include <string.h>

namespace friendmesh {

namespace {

DevelopmentRuntime* gRuntime = nullptr;

Id128 seededId(uint8_t seed) {
  Id128 id = {};
  for (size_t i = 0; i < kIdSize; ++i) id.bytes[i] = seed + i;
  return id;
}

}  // namespace

void setDevelopmentRuntime(DevelopmentRuntime* runtime) { gRuntime = runtime; }

DevelopmentRuntime* developmentRuntime() { return gRuntime; }

DevelopmentRuntime::DevelopmentRuntime()
    : chat_(domain_, payloads_, journal_, outboxStore_),
      safety_(domain_, notifications_),
      nextSequence_(1),
      nextPayloadHandle_(1),
      nextEventSeed_(32),
      initialized_(false) {
  ownerId_ = seededId(1);
  groupId_ = seededId(120);
}

EventHeader DevelopmentRuntime::event(EventType type, uint32_t payloadLength,
                                      uint32_t now, uint32_t expiresAt) {
  EventHeader next = {};
  next.eventId = seededId(nextEventSeed_++);
  next.groupId = groupId_;
  next.senderId = ownerId_;
  next.type = type;
  next.priority = policyForEvent(type).priority;
  next.membershipEpoch = 1;
  next.senderSequence = nextSequence_++;
  next.createdAt = now;
  next.expiresAt = expiresAt;
  next.payloadLength = payloadLength;
  if (payloadLength > 0) next.payloadHandle = nextPayloadHandle_++;
  return next;
}

ResultCode DevelopmentRuntime::initialize(const char* ownerLabel,
                                          uint32_t now) {
  if (initialized_) return ResultCode::Duplicate;
  FriendRecord owner = {};
  owner.id = ownerId_;
  owner.identity.meshIdentity = seededId(2);
  owner.identity.signingIdentity = seededId(3);
  if (!copyBoundedText(owner.label, sizeof(owner.label), ownerLabel,
                       kMaxFriendLabelBytes)) return ResultCode::InvalidText;
  owner.verification = VerificationState::Verified;
  owner.emergencyContactAllowed = true;
  ResultCode result = domain_.addFriend(owner);
  if (result != ResultCode::Ok) return result;
  result = domain_.createGroup(groupId_, "Friends", ownerId_, owner.label, now);
  if (result != ResultCode::Ok) return result;
  domain_.mutableGroupById(groupId_)->locationVisibility =
      LocationVisibility::Precise;
  initialized_ = true;
  return ResultCode::Ok;
}

ResultCode DevelopmentRuntime::addLocalMessage(const char* text, uint32_t now) {
  if (!initialized_) return ResultCode::InvalidState;
  if (!text) return ResultCode::InvalidArgument;
  const size_t length = strlen(text);
  EventHeader next = event(EventType::ChatMessage,
                           length + kChatEnvelopeHeaderBytes, now);
  return chat_.createMessage(next, reinterpret_cast<const uint8_t*>(text),
                             length);
}

ResultCode DevelopmentRuntime::addLocalMarker(int32_t latitudeE7,
                                              int32_t longitudeE7,
                                              MarkerType type,
                                              uint32_t now) {
  if (!initialized_) return ResultCode::InvalidState;
  EventHeader next = event(EventType::MarkerCreated, 0, now, now + 3600);
  MarkerRecord marker = {};
  marker.markerId = next.eventId;
  marker.groupId = groupId_;
  marker.creatorId = ownerId_;
  marker.position.subjectId = ownerId_;
  marker.position.latitudeE7 = latitudeE7;
  marker.position.longitudeE7 = longitudeE7;
  marker.position.capturedAt = now;
  marker.position.receivedAt = now;
  marker.position.accuracyMeters = 10;
  marker.position.source = PositionSource::OnDeviceGps;
  marker.position.valid = true;
  marker.type = type;
  marker.state = MarkerState::Active;
  marker.createdAt = now;
  marker.expiresAt = next.expiresAt;
  return markers_.create(next, marker, domain_);
}

ResultCode DevelopmentRuntime::openLocalHelp(IncidentKind kind,
                                             int32_t latitudeE7,
                                             int32_t longitudeE7,
                                             bool locationValid,
                                             uint32_t now) {
  if (!initialized_ || kind == IncidentKind::Sos) return ResultCode::InvalidState;
  EventHeader next = event(EventType::HelpOpened, 0, now);
  IncidentRecord incident = {};
  incident.incidentId = next.eventId;
  incident.originatorId = ownerId_;
  incident.kind = kind;
  incident.position.subjectId = ownerId_;
  incident.position.latitudeE7 = latitudeE7;
  incident.position.longitudeE7 = longitudeE7;
  incident.position.capturedAt = now;
  incident.position.receivedAt = now;
  incident.position.source = PositionSource::OnDeviceGps;
  incident.position.valid = locationValid;
  return safety_.openHelp(next, incident, groupId_, ResultCode::Ok, now);
}

DevelopmentRuntimeSnapshot DevelopmentRuntime::snapshot() const {
  DevelopmentRuntimeSnapshot value = {};
  value.initialized = initialized_;
  value.messages = chat_.messageCount();
  value.markers = markers_.size();
  value.meetups = meetups_.size();
  value.incidents = safety_.size();
  value.notifications = notifications_.size();
  value.nextSequence = nextSequence_;
  return value;
}

}  // namespace friendmesh
