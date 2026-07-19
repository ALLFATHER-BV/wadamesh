#include "FriendMeshSync.h"

#include <string.h>

namespace friendmesh {

namespace {

bool sameEvent(const EventHeader& lhs, const EventHeader& rhs) {
  return idsEqual(lhs.eventId, rhs.eventId) && idsEqual(lhs.groupId, rhs.groupId) &&
         idsEqual(lhs.senderId, rhs.senderId) && lhs.type == rhs.type &&
         lhs.membershipEpoch == rhs.membershipEpoch &&
         lhs.senderSequence == rhs.senderSequence &&
         lhs.createdAt == rhs.createdAt && lhs.payloadLength == rhs.payloadLength &&
         memcmp(lhs.payloadHash, rhs.payloadHash, sizeof(lhs.payloadHash)) == 0;
}

}  // namespace

SyncEngine::SyncEngine(const DomainStore& domain)
    : domain_(domain), eventCount_(0), progressCount_(0), conflictCount_(0) {
  memset(events_, 0, sizeof(events_));
  memset(progress_, 0, sizeof(progress_));
  memset(conflicts_, 0, sizeof(conflicts_));
}

bool SyncEngine::memberAuthorizedAtEpoch(const EventHeader& event) const {
  if (!policyForEvent(event.type).requiresGroup) return true;
  const GroupRecord* group = domain_.groupById(event.groupId);
  const GroupMember* member = domain_.groupMember(event.groupId, event.senderId);
  if (!group || !member || event.membershipEpoch == 0 ||
      event.membershipEpoch > group->membershipEpoch ||
      member->admittedEpoch > event.membershipEpoch) {
    return false;
  }
  return member->removedEpoch == 0 || member->removedEpoch > event.membershipEpoch;
}

const SyncProgress* SyncEngine::progress(const Id128& groupId,
                                         const Id128& senderId,
                                         uint32_t epoch) const {
  for (size_t i = 0; i < progressCount_; ++i) {
    if (idsEqual(progress_[i].groupId, groupId) &&
        idsEqual(progress_[i].senderId, senderId) &&
        progress_[i].membershipEpoch == epoch) {
      return &progress_[i];
    }
  }
  return nullptr;
}

SyncProgress* SyncEngine::mutableProgress(const Id128& groupId,
                                         const Id128& senderId, uint32_t epoch,
                                         uint32_t at) {
  for (size_t i = 0; i < progressCount_; ++i) {
    if (idsEqual(progress_[i].groupId, groupId) &&
        idsEqual(progress_[i].senderId, senderId) &&
        progress_[i].membershipEpoch == epoch) {
      progress_[i].lastUpdatedAt = at;
      return &progress_[i];
    }
  }
  if (progressCount_ >= kMaxSyncSenders) return nullptr;
  SyncProgress& next = progress_[progressCount_++];
  next = {};
  next.groupId = groupId;
  next.senderId = senderId;
  next.membershipEpoch = epoch;
  next.lastUpdatedAt = at;
  return &next;
}

ResultCode SyncEngine::quarantine(const EventHeader& event,
                                  SyncConflictReason reason, uint32_t at) {
  if (conflictCount_ >= kMaxSyncConflicts) return ResultCode::CapacityReached;
  SyncConflict& conflict = conflicts_[conflictCount_++];
  conflict = {};
  conflict.observed = event;
  conflict.reason = reason;
  conflict.quarantinedAt = at;
  return ResultCode::Quarantined;
}

const SyncConflict* SyncEngine::conflictAt(size_t index) const {
  return index < conflictCount_ ? &conflicts_[index] : nullptr;
}

ResultCode SyncEngine::observe(const EventHeader& event, uint32_t receivedAt) {
  const ResultCode valid = validateEventHeader(event);
  if (valid != ResultCode::Ok) return valid;
  if (event.senderSequence == 0) return ResultCode::InvalidArgument;
  if (!memberAuthorizedAtEpoch(event)) {
    return quarantine(event, SyncConflictReason::StaleEpoch, receivedAt);
  }

  for (size_t i = 0; i < eventCount_; ++i) {
    if (idsEqual(events_[i].eventId, event.eventId)) {
      return sameEvent(events_[i], event)
                 ? ResultCode::Duplicate
                 : quarantine(event, SyncConflictReason::EventIdMismatch,
                              receivedAt);
    }
    if (idsEqual(events_[i].groupId, event.groupId) &&
        idsEqual(events_[i].senderId, event.senderId) &&
        events_[i].membershipEpoch == event.membershipEpoch &&
        events_[i].senderSequence == event.senderSequence) {
      return quarantine(event, SyncConflictReason::SenderSequenceCollision,
                        receivedAt);
    }
  }
  if (eventCount_ >= kMaxSyncIndexedEvents) return ResultCode::CapacityReached;
  SyncProgress* state = mutableProgress(event.groupId, event.senderId,
                                        event.membershipEpoch, receivedAt);
  if (!state) return ResultCode::CapacityReached;
  if (event.senderSequence <= state->highestContiguousSequence) {
    return quarantine(event, SyncConflictReason::SenderSequenceCollision,
                      receivedAt);
  }
  const uint32_t distance =
      event.senderSequence - state->highestContiguousSequence;
  if (distance > kSyncSequenceWindow) {
    state->incomplete = true;
    if (event.senderSequence > state->highestSeenSequence) {
      state->highestSeenSequence = event.senderSequence;
    }
    return quarantine(event, SyncConflictReason::SequenceOutsideWindow,
                      receivedAt);
  }

  state->pendingMask |= UINT64_C(1) << (distance - 1);
  if (event.senderSequence > state->highestSeenSequence) {
    state->highestSeenSequence = event.senderSequence;
  }
  while ((state->pendingMask & UINT64_C(1)) != 0) {
    ++state->highestContiguousSequence;
    state->pendingMask >>= 1;
  }
  state->incomplete = state->highestSeenSequence >
                      state->highestContiguousSequence;
  events_[eventCount_++] = event;
  return ResultCode::Ok;
}

ResultCode SyncEngine::applyBatch(const SyncBatch& batch, uint32_t receivedAt,
                                  uint8_t& acceptedCount) {
  acceptedCount = 0;
  if (batch.count > kMaxSyncBatchEvents) return ResultCode::InvalidArgument;
  ResultCode aggregate = ResultCode::Ok;
  for (size_t i = 0; i < batch.count; ++i) {
    const ResultCode result = observe(batch.events[i], receivedAt);
    if (result == ResultCode::Ok) {
      ++acceptedCount;
    } else if (result != ResultCode::Duplicate && aggregate == ResultCode::Ok) {
      aggregate = result;
    }
  }
  return aggregate;
}

ResultCode SyncEngine::inventory(const Id128& groupId, const Id128& senderId,
                                 uint32_t epoch,
                                 SyncInventoryRecord& result) const {
  const SyncProgress* state = progress(groupId, senderId, epoch);
  if (!state) return ResultCode::NotFound;
  result = {};
  result.groupId = groupId;
  result.senderId = senderId;
  result.membershipEpoch = epoch;
  result.highestContiguousSequence = state->highestContiguousSequence;
  result.highestSeenSequence = state->highestSeenSequence;
  result.firstMissingSequence =
      state->highestSeenSequence > state->highestContiguousSequence
          ? state->highestContiguousSequence + 1
          : 0;
  result.incomplete = state->incomplete;
  return ResultCode::Ok;
}

ResultCode SyncEngine::planMissingRange(
    const SyncInventoryRecord& local, const SyncInventoryRecord& remote,
    uint32_t maxEvents, SyncRange& range) const {
  if (!idsEqual(local.groupId, remote.groupId) ||
      !idsEqual(local.senderId, remote.senderId) ||
      local.membershipEpoch != remote.membershipEpoch || maxEvents == 0) {
    return ResultCode::InvalidArgument;
  }
  if (local.highestSeenSequence < local.highestContiguousSequence ||
      remote.highestSeenSequence < remote.highestContiguousSequence ||
      local.highestContiguousSequence == UINT32_MAX) {
    return ResultCode::CorruptData;
  }
  if (remote.highestSeenSequence <= local.highestContiguousSequence) {
    return ResultCode::NotFound;
  }
  range = {};
  range.groupId = local.groupId;
  range.senderId = local.senderId;
  range.membershipEpoch = local.membershipEpoch;
  range.firstSequence = local.highestContiguousSequence + 1;
  const uint32_t available =
      remote.highestSeenSequence - range.firstSequence + 1;
  const uint32_t count = available < maxEvents ? available : maxEvents;
  range.lastSequence = range.firstSequence + count - 1;
  return ResultCode::Ok;
}

ResultCode SyncEngine::buildPriorityBatch(const Id128& groupId, uint32_t epoch,
                                          uint8_t maxEvents,
                                          SyncBatch& batch) const {
  batch = {};
  if (maxEvents == 0) return ResultCode::InvalidArgument;
  if (maxEvents > kMaxSyncBatchEvents) maxEvents = kMaxSyncBatchEvents;
  bool selected[kMaxSyncIndexedEvents] = {};
  while (batch.count < maxEvents) {
    size_t best = kMaxSyncIndexedEvents;
    for (size_t i = 0; i < eventCount_; ++i) {
      if (selected[i] || !idsEqual(events_[i].groupId, groupId) ||
          events_[i].membershipEpoch != epoch) {
        continue;
      }
      if (best == kMaxSyncIndexedEvents ||
          events_[i].priority > events_[best].priority ||
          (events_[i].priority == events_[best].priority &&
           events_[i].createdAt > events_[best].createdAt)) {
        best = i;
      }
    }
    if (best == kMaxSyncIndexedEvents) break;
    selected[best] = true;
    batch.events[batch.count++] = events_[best];
  }
  return batch.count > 0 ? ResultCode::Ok : ResultCode::NotFound;
}

ResultCode SyncEngine::buildRangeBatch(const SyncRange& range,
                                       uint8_t maxEvents,
                                       SyncBatch& batch) const {
  batch = {};
  if (idIsZero(range.groupId) || idIsZero(range.senderId) ||
      range.membershipEpoch == 0 || range.firstSequence == 0 ||
      range.lastSequence < range.firstSequence || maxEvents == 0) {
    return ResultCode::InvalidArgument;
  }
  if (maxEvents > kMaxSyncBatchEvents) maxEvents = kMaxSyncBatchEvents;
  for (uint32_t sequence = range.firstSequence;
       sequence <= range.lastSequence && batch.count < maxEvents; ++sequence) {
    for (size_t i = 0; i < eventCount_; ++i) {
      if (idsEqual(events_[i].groupId, range.groupId) &&
          idsEqual(events_[i].senderId, range.senderId) &&
          events_[i].membershipEpoch == range.membershipEpoch &&
          events_[i].senderSequence == sequence) {
        batch.events[batch.count++] = events_[i];
        break;
      }
    }
    if (sequence == UINT32_MAX) break;
  }
  return batch.count > 0 ? ResultCode::Ok : ResultCode::NotFound;
}

ResultCode SyncEngine::makeReceipt(const Id128& groupId, const Id128& senderId,
                                   uint32_t epoch, uint32_t receivedAt,
                                   SyncReceiptRecord& receipt) const {
  const SyncProgress* state = progress(groupId, senderId, epoch);
  if (!state) return ResultCode::NotFound;
  receipt = {};
  receipt.groupId = groupId;
  receipt.senderId = senderId;
  receipt.membershipEpoch = epoch;
  receipt.highestContiguousSequence = state->highestContiguousSequence;
  receipt.receivedAt = receivedAt;
  return ResultCode::Ok;
}

}  // namespace friendmesh
