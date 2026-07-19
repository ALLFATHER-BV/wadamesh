#pragma once

#include "FriendMeshEvent.h"

namespace friendmesh {

enum class OutboxState : uint8_t {
  Empty = 0,
  Queued,
  Transmitting,
  RelayedOrObserved,
  Delivered,
  RetryWaiting,
  Failed,
  Expired,
  Cancelled,
};

struct OutboxEntry {
  EventHeader event;
  OutboxState state;
  ResultCode lastResult;
  uint32_t nextAttemptAt;
  uint16_t attemptCount;
  bool persistenceConfirmed;
};

template <size_t Capacity>
class OutboxQueue {
 public:
  static_assert(Capacity > 0, "Outbox capacity must be non-zero");

  OutboxQueue() : count_(0) {
    for (size_t i = 0; i < Capacity; ++i) entries_[i].state = OutboxState::Empty;
  }

  size_t size() const { return count_; }
  size_t capacity() const { return Capacity; }

  const OutboxEntry* at(size_t logicalIndex) const {
    size_t seen = 0;
    for (size_t i = 0; i < Capacity; ++i) {
      if (entries_[i].state == OutboxState::Empty) continue;
      if (seen++ == logicalIndex) return &entries_[i];
    }
    return nullptr;
  }

  const OutboxEntry* find(const Id128& eventId) const {
    for (size_t i = 0; i < Capacity; ++i) {
      if (entries_[i].state != OutboxState::Empty &&
          idsEqual(entries_[i].event.eventId, eventId)) return &entries_[i];
    }
    return nullptr;
  }

  OutboxEntry* mutableFind(const Id128& eventId) {
    for (size_t i = 0; i < Capacity; ++i) {
      if (entries_[i].state != OutboxState::Empty &&
          idsEqual(entries_[i].event.eventId, eventId)) return &entries_[i];
    }
    return nullptr;
  }

  ResultCode enqueue(const EventHeader& event, bool persistenceConfirmed) {
    const ResultCode validation = validateEventHeader(event);
    if (validation != ResultCode::Ok) return validation;
    if (find(event.eventId)) return ResultCode::Duplicate;

    const EventPolicy policy = policyForEvent(event.type);
    if (policy.durability != DurabilityRule::MemoryOnly &&
        policy.durability != DurabilityRule::SafetyMayBypassPersistence &&
        !persistenceConfirmed) return ResultCode::InvalidState;

    for (size_t i = 0; i < Capacity; ++i) {
      if (entries_[i].state == OutboxState::Empty) {
        entries_[i].event = event;
        entries_[i].state = OutboxState::Queued;
        entries_[i].lastResult = ResultCode::Ok;
        entries_[i].nextAttemptAt = 0;
        entries_[i].attemptCount = 0;
        entries_[i].persistenceConfirmed = persistenceConfirmed;
        ++count_;
        return ResultCode::Ok;
      }
    }
    return ResultCode::CapacityReached;
  }

  OutboxEntry* nextReady(uint32_t now) {
    OutboxEntry* selected = nullptr;
    for (size_t i = 0; i < Capacity; ++i) {
      OutboxEntry& candidate = entries_[i];
      if (candidate.state == OutboxState::Empty) continue;
      if (eventIsExpired(candidate.event, now)) {
        if (candidate.state != OutboxState::Delivered &&
            candidate.state != OutboxState::Cancelled &&
            candidate.state != OutboxState::Failed) {
          candidate.state = OutboxState::Expired;
        }
        continue;
      }
      const bool ready = candidate.state == OutboxState::Queued ||
                         (candidate.state == OutboxState::RetryWaiting &&
                          static_cast<int32_t>(now - candidate.nextAttemptAt) >= 0);
      if (!ready) continue;
      if (!selected || candidate.event.priority > selected->event.priority ||
          (candidate.event.priority == selected->event.priority &&
           candidate.event.createdAt < selected->event.createdAt)) {
        selected = &candidate;
      }
    }
    return selected;
  }

  ResultCode markTransmitting(const Id128& eventId) {
    OutboxEntry* entry = mutableFind(eventId);
    if (!entry) return ResultCode::NotFound;
    if (entry->state != OutboxState::Queued &&
        entry->state != OutboxState::RetryWaiting) return ResultCode::InvalidState;
    entry->state = OutboxState::Transmitting;
    ++entry->attemptCount;
    return ResultCode::Ok;
  }

  ResultCode markRelayedOrObserved(const Id128& eventId) {
    OutboxEntry* entry = mutableFind(eventId);
    if (!entry) return ResultCode::NotFound;
    if (entry->state != OutboxState::Transmitting) return ResultCode::InvalidState;
    entry->state = OutboxState::RelayedOrObserved;
    return ResultCode::Ok;
  }

  ResultCode markDelivered(const Id128& eventId) {
    OutboxEntry* entry = mutableFind(eventId);
    if (!entry) return ResultCode::NotFound;
    if (entry->state != OutboxState::Transmitting &&
        entry->state != OutboxState::RelayedOrObserved) return ResultCode::InvalidState;
    entry->state = OutboxState::Delivered;
    return ResultCode::Ok;
  }

  ResultCode scheduleRetry(const Id128& eventId, uint32_t nextAttemptAt,
                           ResultCode reason) {
    OutboxEntry* entry = mutableFind(eventId);
    if (!entry) return ResultCode::NotFound;
    if (entry->state != OutboxState::Transmitting &&
        entry->state != OutboxState::RelayedOrObserved) return ResultCode::InvalidState;
    entry->state = OutboxState::RetryWaiting;
    entry->nextAttemptAt = nextAttemptAt;
    entry->lastResult = reason;
    return ResultCode::Ok;
  }

  ResultCode cancel(const Id128& eventId) {
    OutboxEntry* entry = mutableFind(eventId);
    if (!entry) return ResultCode::NotFound;
    if (entry->state == OutboxState::Delivered || entry->state == OutboxState::Expired) {
      return ResultCode::InvalidState;
    }
    entry->state = OutboxState::Cancelled;
    return ResultCode::Ok;
  }

  ResultCode removeTerminal(const Id128& eventId) {
    OutboxEntry* entry = mutableFind(eventId);
    if (!entry) return ResultCode::NotFound;
    if (entry->state != OutboxState::Delivered && entry->state != OutboxState::Failed &&
        entry->state != OutboxState::Expired && entry->state != OutboxState::Cancelled) {
      return ResultCode::InvalidState;
    }
    entry->state = OutboxState::Empty;
    --count_;
    return ResultCode::Ok;
  }

  ResultCode restore(const OutboxEntry& persisted) {
    if (persisted.state == OutboxState::Empty) return ResultCode::InvalidState;
    const ResultCode validation = validateEventHeader(persisted.event);
    if (validation != ResultCode::Ok) return validation;
    if (find(persisted.event.eventId)) return ResultCode::Duplicate;
    for (size_t i = 0; i < Capacity; ++i) {
      if (entries_[i].state == OutboxState::Empty) {
        entries_[i] = persisted;
        ++count_;
        return ResultCode::Ok;
      }
    }
    return ResultCode::CapacityReached;
  }

  ResultCode replacePersisted(const OutboxEntry& persisted) {
    if (persisted.state == OutboxState::Empty) return ResultCode::InvalidState;
    OutboxEntry* current = mutableFind(persisted.event.eventId);
    if (!current) return ResultCode::NotFound;
    const ResultCode validation = validateEventHeader(persisted.event);
    if (validation != ResultCode::Ok) return validation;
    *current = persisted;
    return ResultCode::Ok;
  }

 private:
  OutboxEntry entries_[Capacity];
  size_t count_;
};

}  // namespace friendmesh
