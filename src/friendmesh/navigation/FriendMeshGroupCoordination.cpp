#include "FriendMeshGroupCoordination.h"

#include <string.h>

namespace friendmesh {
namespace {

constexpr uint8_t kEventMagic[] = {'F', 'M', 'C', '1'};
constexpr uint8_t kStateMagic[] = {'F', 'M', 'S', '1'};
constexpr uint8_t kVersion = 1;
constexpr size_t kItemBytes = kCoordinationObjectIdBytes +
    kChannelRosterPrefixBytes + 2 + 4 + 4 + 4 + 4 + 4 + 1 + 1 +
    kCoordinationNoteBytes;
constexpr size_t kEventHeaderBytes = 8;
constexpr size_t kStateHeaderBytes = 8;
constexpr size_t kResponseBytes = kChannelRosterPrefixBytes + 1;

bool allZero(const uint8_t* value, size_t length) {
  if (!value) return true;
  for (size_t i = 0; i < length; ++i) if (value[i] != 0) return false;
  return true;
}

void put32(uint8_t* destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t get32(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) |
      (static_cast<uint32_t>(source[1]) << 8) |
      (static_cast<uint32_t>(source[2]) << 16) |
      (static_cast<uint32_t>(source[3]) << 24);
}

bool validKind(uint8_t value) {
  return value <= static_cast<uint8_t>(GroupCoordinationKind::Sos);
}

bool validStatus(uint8_t value) {
  return value <= static_cast<uint8_t>(GroupCoordinationStatus::Expired);
}

bool validResponse(uint8_t value) {
  return value <= static_cast<uint8_t>(GroupCoordinationResponse::Arrived);
}

bool validAction(uint8_t value) {
  return value <= static_cast<uint8_t>(GroupCoordinationAction::CloseIncident);
}

bool responseListValid(const GroupCoordinationResponder* responses,
                       uint8_t count) {
  for (uint8_t i = 0; i < count; ++i) {
    if (allZero(responses[i].memberPrefix, kChannelRosterPrefixBytes) ||
        !validResponse(static_cast<uint8_t>(responses[i].response)) ||
        responses[i].response == GroupCoordinationResponse::None) return false;
    for (uint8_t j = 0; j < i; ++j) {
      if (memcmp(responses[i].memberPrefix, responses[j].memberPrefix,
                 kChannelRosterPrefixBytes) == 0) return false;
    }
  }
  return true;
}

bool validCoordinates(int32_t latitudeE6, int32_t longitudeE6) {
  return latitudeE6 >= -90000000 && latitudeE6 <= 90000000 &&
         longitudeE6 >= -180000000 && longitudeE6 <= 180000000 &&
         (latitudeE6 != 0 || longitudeE6 != 0);
}

bool itemValid(const GroupCoordinationItem& item, bool allowInactive) {
  if (allZero(item.objectId, sizeof(item.objectId)) ||
      allZero(item.ownerPrefix, sizeof(item.ownerPrefix)) ||
      !validKind(static_cast<uint8_t>(item.kind)) ||
      !validStatus(static_cast<uint8_t>(item.status)) ||
      item.createdAt == 0 || item.updatedAt < item.createdAt ||
      (item.expiresAt != 0 && item.expiresAt <= item.createdAt) ||
      (item.hasLocation && !validCoordinates(item.latitudeE6,
                                             item.longitudeE6)) ||
      strnlen(item.note, sizeof(item.note)) > kCoordinationNoteBytes) return false;
  return allowInactive || item.status == GroupCoordinationStatus::Active;
}

size_t encodeItem(const GroupCoordinationItem& item, uint8_t* destination) {
  size_t offset = 0;
  memcpy(destination + offset, item.objectId, sizeof(item.objectId));
  offset += sizeof(item.objectId);
  memcpy(destination + offset, item.ownerPrefix, sizeof(item.ownerPrefix));
  offset += sizeof(item.ownerPrefix);
  destination[offset++] = static_cast<uint8_t>(item.kind);
  destination[offset++] = static_cast<uint8_t>(item.status);
  put32(destination + offset, item.createdAt); offset += 4;
  put32(destination + offset, item.updatedAt); offset += 4;
  put32(destination + offset, item.expiresAt); offset += 4;
  put32(destination + offset, static_cast<uint32_t>(item.latitudeE6)); offset += 4;
  put32(destination + offset, static_cast<uint32_t>(item.longitudeE6)); offset += 4;
  destination[offset++] = item.hasLocation ? 1 : 0;
  const size_t noteLength = strnlen(item.note, kCoordinationNoteBytes);
  destination[offset++] = static_cast<uint8_t>(noteLength);
  memset(destination + offset, 0, kCoordinationNoteBytes);
  memcpy(destination + offset, item.note, noteLength);
  offset += kCoordinationNoteBytes;
  return offset;
}

bool decodeItem(const uint8_t* source, GroupCoordinationItem& item) {
  item = {};
  size_t offset = 0;
  memcpy(item.objectId, source + offset, sizeof(item.objectId));
  offset += sizeof(item.objectId);
  memcpy(item.ownerPrefix, source + offset, sizeof(item.ownerPrefix));
  offset += sizeof(item.ownerPrefix);
  const uint8_t kind = source[offset++];
  const uint8_t status = source[offset++];
  item.createdAt = get32(source + offset); offset += 4;
  item.updatedAt = get32(source + offset); offset += 4;
  item.expiresAt = get32(source + offset); offset += 4;
  item.latitudeE6 = static_cast<int32_t>(get32(source + offset)); offset += 4;
  item.longitudeE6 = static_cast<int32_t>(get32(source + offset)); offset += 4;
  const uint8_t hasLocation = source[offset++];
  const uint8_t noteLength = source[offset++];
  if (!validKind(kind) || !validStatus(status) || hasLocation > 1 ||
      noteLength > kCoordinationNoteBytes) return false;
  item.kind = static_cast<GroupCoordinationKind>(kind);
  item.status = static_cast<GroupCoordinationStatus>(status);
  item.hasLocation = hasLocation != 0;
  memcpy(item.note, source + offset, noteLength);
  item.note[noteLength] = '\0';
  return itemValid(item, true);
}

const ChannelRosterMember* joinedMember(const ChannelRoster& roster,
                                        const uint8_t* prefix) {
  const ChannelRosterMember* member = findChannelRosterMember(roster, prefix);
  return member && member->state == ChannelRosterState::Joined ? member : nullptr;
}

bool sameObject(const GroupCoordinationItem& item,
                const GroupCoordinationEvent& event) {
  return memcmp(item.objectId, event.item.objectId,
                kCoordinationObjectIdBytes) == 0;
}

ResultCode mayReplace(const GroupCoordinationItem& current,
                      const GroupCoordinationItem& incoming) {
  if (allZero(current.objectId, sizeof(current.objectId))) return ResultCode::Ok;
  if (current.updatedAt > incoming.updatedAt) return ResultCode::Conflict;
  if (current.updatedAt < incoming.updatedAt) return ResultCode::Ok;
  const int order = memcmp(current.objectId, incoming.objectId,
                           kCoordinationObjectIdBytes);
  if (order == 0) return ResultCode::Duplicate;
  // The lexicographically greater random object ID wins everywhere when two
  // members create a replacement during the same clock second.
  return order < 0 ? ResultCode::Ok : ResultCode::Conflict;
}

bool ownerOrAdmin(const GroupCoordinationItem& item,
                  const GroupCoordinationEvent& event,
                  const ChannelRosterMember& actor) {
  return memcmp(item.ownerPrefix, event.item.ownerPrefix,
                kChannelRosterPrefixBytes) == 0 ||
         actor.role == ChannelRosterRole::Admin;
}

ResultCode setResponse(GroupCoordinationResponder* responses, uint8_t& count,
                       const uint8_t* member,
                       GroupCoordinationResponse response) {
  if (response == GroupCoordinationResponse::None) return ResultCode::InvalidArgument;
  for (uint8_t i = 0; i < count; ++i) {
    if (memcmp(responses[i].memberPrefix, member,
               kChannelRosterPrefixBytes) != 0) continue;
    if (responses[i].response == response) return ResultCode::Duplicate;
    responses[i].response = response;
    return ResultCode::Ok;
  }
  if (count >= kCoordinationMaxResponses) return ResultCode::CapacityReached;
  memcpy(responses[count].memberPrefix, member, kChannelRosterPrefixBytes);
  responses[count].response = response;
  ++count;
  return ResultCode::Ok;
}

}  // namespace

void clearGroupCoordinationState(GroupCoordinationState& state) {
  memset(&state, 0, sizeof(state));
}

bool groupCoordinationItemActive(const GroupCoordinationItem& item,
                                 uint32_t now) {
  return item.status == GroupCoordinationStatus::Active &&
         !allZero(item.objectId, sizeof(item.objectId)) &&
         (item.expiresAt == 0 || now == 0 || now < item.expiresAt);
}

ResultCode encodeGroupCoordinationEvent(const GroupCoordinationEvent& event,
                                        uint8_t* destination,
                                        size_t capacity, size_t& written) {
  written = 0;
  if (!destination || capacity < kEventHeaderBytes + kItemBytes ||
      !validAction(static_cast<uint8_t>(event.action)) ||
      !validResponse(static_cast<uint8_t>(event.response)) ||
      !itemValid(event.item, true)) return ResultCode::InvalidArgument;
  memcpy(destination, kEventMagic, sizeof(kEventMagic));
  destination[4] = kVersion;
  destination[5] = static_cast<uint8_t>(event.action);
  destination[6] = static_cast<uint8_t>(event.response);
  destination[7] = 0;
  written = kEventHeaderBytes + encodeItem(event.item,
                                           destination + kEventHeaderBytes);
  return ResultCode::Ok;
}

ResultCode decodeGroupCoordinationEvent(const uint8_t* source, size_t length,
                                        GroupCoordinationEvent& event) {
  event = {};
  if (!source || length != kEventHeaderBytes + kItemBytes ||
      memcmp(source, kEventMagic, sizeof(kEventMagic)) != 0 ||
      source[4] != kVersion || !validAction(source[5]) ||
      !validResponse(source[6]) || source[7] != 0 ||
      !decodeItem(source + kEventHeaderBytes, event.item)) {
    return ResultCode::CorruptData;
  }
  event.action = static_cast<GroupCoordinationAction>(source[5]);
  event.response = static_cast<GroupCoordinationResponse>(source[6]);
  return ResultCode::Ok;
}

ResultCode encodeGroupCoordinationState(const GroupCoordinationState& state,
                                        uint8_t* destination,
                                        size_t capacity, size_t& written) {
  written = 0;
  if (!destination || state.meetupResponseCount > kCoordinationMaxResponses ||
      state.incidentResponseCount > kCoordinationMaxResponses ||
      !responseListValid(state.meetupResponses,
                         state.meetupResponseCount) ||
      !responseListValid(state.incidentResponses,
                         state.incidentResponseCount)) {
    return ResultCode::InvalidArgument;
  }
  const bool hasMeetup = !allZero(state.meetup.objectId,
                                  sizeof(state.meetup.objectId));
  const bool hasIncident = !allZero(state.incident.objectId,
                                    sizeof(state.incident.objectId));
  if ((hasMeetup && !itemValid(state.meetup, true)) ||
      (hasIncident && !itemValid(state.incident, true))) return ResultCode::CorruptData;
  const size_t required = kStateHeaderBytes + (hasMeetup ? kItemBytes : 0) +
      (hasIncident ? kItemBytes : 0) +
      (state.meetupResponseCount + state.incidentResponseCount) * kResponseBytes;
  if (required > capacity || required > kGroupCoordinationStateMaxBytes)
    return ResultCode::CapacityReached;
  memcpy(destination, kStateMagic, sizeof(kStateMagic));
  destination[4] = kVersion;
  destination[5] = (hasMeetup ? 1 : 0) | (hasIncident ? 2 : 0);
  destination[6] = state.meetupResponseCount;
  destination[7] = state.incidentResponseCount;
  size_t offset = kStateHeaderBytes;
  if (hasMeetup) offset += encodeItem(state.meetup, destination + offset);
  if (hasIncident) offset += encodeItem(state.incident, destination + offset);
  for (uint8_t i = 0; i < state.meetupResponseCount; ++i) {
    memcpy(destination + offset, state.meetupResponses[i].memberPrefix,
           kChannelRosterPrefixBytes); offset += kChannelRosterPrefixBytes;
    destination[offset++] = static_cast<uint8_t>(state.meetupResponses[i].response);
  }
  for (uint8_t i = 0; i < state.incidentResponseCount; ++i) {
    memcpy(destination + offset, state.incidentResponses[i].memberPrefix,
           kChannelRosterPrefixBytes); offset += kChannelRosterPrefixBytes;
    destination[offset++] = static_cast<uint8_t>(state.incidentResponses[i].response);
  }
  written = offset;
  return ResultCode::Ok;
}

ResultCode decodeGroupCoordinationState(const uint8_t* source, size_t length,
                                        GroupCoordinationState& state) {
  clearGroupCoordinationState(state);
  if (!source || length < kStateHeaderBytes ||
      memcmp(source, kStateMagic, sizeof(kStateMagic)) != 0 ||
      source[4] != kVersion || (source[5] & ~3U) != 0 ||
      source[6] > kCoordinationMaxResponses ||
      source[7] > kCoordinationMaxResponses) return ResultCode::CorruptData;
  const bool hasMeetup = (source[5] & 1U) != 0;
  const bool hasIncident = (source[5] & 2U) != 0;
  const size_t expected = kStateHeaderBytes +
      (hasMeetup ? kItemBytes : 0) + (hasIncident ? kItemBytes : 0) +
      (source[6] + source[7]) * kResponseBytes;
  if (length != expected || length > kGroupCoordinationStateMaxBytes)
    return ResultCode::CorruptData;
  size_t offset = kStateHeaderBytes;
  if (hasMeetup && !decodeItem(source + offset, state.meetup))
    return ResultCode::CorruptData;
  if (hasMeetup) offset += kItemBytes;
  if (hasIncident && !decodeItem(source + offset, state.incident))
    return ResultCode::CorruptData;
  if (hasIncident) offset += kItemBytes;
  state.meetupResponseCount = source[6];
  state.incidentResponseCount = source[7];
  for (uint8_t i = 0; i < state.meetupResponseCount; ++i) {
    memcpy(state.meetupResponses[i].memberPrefix, source + offset,
           kChannelRosterPrefixBytes); offset += kChannelRosterPrefixBytes;
    const uint8_t response = source[offset++];
    if (allZero(state.meetupResponses[i].memberPrefix,
                kChannelRosterPrefixBytes) || !validResponse(response) ||
        response == 0) return ResultCode::CorruptData;
    state.meetupResponses[i].response =
        static_cast<GroupCoordinationResponse>(response);
    for (uint8_t j = 0; j < i; ++j) {
      if (memcmp(state.meetupResponses[i].memberPrefix,
                 state.meetupResponses[j].memberPrefix,
                 kChannelRosterPrefixBytes) == 0)
        return ResultCode::CorruptData;
    }
  }
  for (uint8_t i = 0; i < state.incidentResponseCount; ++i) {
    memcpy(state.incidentResponses[i].memberPrefix, source + offset,
           kChannelRosterPrefixBytes); offset += kChannelRosterPrefixBytes;
    const uint8_t response = source[offset++];
    if (allZero(state.incidentResponses[i].memberPrefix,
                kChannelRosterPrefixBytes) || !validResponse(response) ||
        response == 0) return ResultCode::CorruptData;
    state.incidentResponses[i].response =
        static_cast<GroupCoordinationResponse>(response);
    for (uint8_t j = 0; j < i; ++j) {
      if (memcmp(state.incidentResponses[i].memberPrefix,
                 state.incidentResponses[j].memberPrefix,
                 kChannelRosterPrefixBytes) == 0)
        return ResultCode::CorruptData;
    }
  }
  return ResultCode::Ok;
}

ResultCode applyGroupCoordinationEvent(GroupCoordinationState& state,
                                       const GroupCoordinationEvent& event,
                                       const ChannelRoster& roster,
                                       uint32_t now) {
  const ChannelRosterMember* actor = joinedMember(roster,
                                                   event.item.ownerPrefix);
  if (!actor || !itemValid(event.item, true)) return ResultCode::Unauthorized;
  const bool meetupAction = event.action == GroupCoordinationAction::SetMeetup ||
      event.action == GroupCoordinationAction::CancelMeetup ||
      event.action == GroupCoordinationAction::MeetupResponse;
  GroupCoordinationItem& current = meetupAction ? state.meetup : state.incident;
  if (event.action == GroupCoordinationAction::SetMeetup) {
    if (event.item.kind != GroupCoordinationKind::Meetup &&
        event.item.kind != GroupCoordinationKind::Pickup)
      return ResultCode::InvalidArgument;
    if (!event.item.hasLocation) return ResultCode::InvalidArgument;
    const ResultCode replace = mayReplace(current, event.item);
    if (replace != ResultCode::Ok) return replace;
    current = event.item;
    memset(state.meetupResponses, 0, sizeof(state.meetupResponses));
    state.meetupResponseCount = 0;
    return ResultCode::Ok;
  }
  if (event.action == GroupCoordinationAction::OpenIncident) {
    if (event.item.kind == GroupCoordinationKind::Meetup ||
        event.item.kind == GroupCoordinationKind::Pickup)
      return ResultCode::InvalidArgument;
    const ResultCode replace = mayReplace(current, event.item);
    if (replace != ResultCode::Ok) return replace;
    current = event.item;
    memset(state.incidentResponses, 0, sizeof(state.incidentResponses));
    state.incidentResponseCount = 0;
    return ResultCode::Ok;
  }
  if (!sameObject(current, event) ||
      !groupCoordinationItemActive(current, now)) return ResultCode::NotFound;
  if (event.item.updatedAt < current.updatedAt) return ResultCode::Conflict;
  if (event.action == GroupCoordinationAction::CancelMeetup ||
      event.action == GroupCoordinationAction::CloseIncident) {
    if (!ownerOrAdmin(current, event, *actor)) return ResultCode::Unauthorized;
    current.status = event.action == GroupCoordinationAction::CancelMeetup
        ? GroupCoordinationStatus::Cancelled
        : GroupCoordinationStatus::Closed;
    current.updatedAt = event.item.updatedAt;
    return ResultCode::Ok;
  }
  if (event.action == GroupCoordinationAction::MeetupResponse) {
    const ResultCode result = setResponse(
        state.meetupResponses, state.meetupResponseCount,
        event.item.ownerPrefix, event.response);
    if (result == ResultCode::Ok) current.updatedAt = event.item.updatedAt;
    return result;
  }
  if (event.action == GroupCoordinationAction::IncidentResponse) {
    const ResultCode result = setResponse(
        state.incidentResponses, state.incidentResponseCount,
        event.item.ownerPrefix, event.response);
    if (result == ResultCode::Ok) current.updatedAt = event.item.updatedAt;
    return result;
  }
  return ResultCode::InvalidArgument;
}

size_t expireGroupCoordinationState(GroupCoordinationState& state,
                                    uint32_t now) {
  size_t changed = 0;
  GroupCoordinationItem* items[] = {&state.meetup, &state.incident};
  for (size_t i = 0; i < 2; ++i) {
    GroupCoordinationItem& item = *items[i];
    if (item.status == GroupCoordinationStatus::Active &&
        item.expiresAt != 0 && now >= item.expiresAt) {
      item.status = GroupCoordinationStatus::Expired;
      item.updatedAt = now;
      ++changed;
    }
  }
  return changed;
}

}  // namespace friendmesh
