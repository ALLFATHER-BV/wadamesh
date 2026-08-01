#pragma once

#include "FriendMeshDomain.h"
#include "FriendMeshEventHistory.h"
#include "FriendMeshOutbox.h"

namespace friendmesh {

// Owns the shared functional models when a caller explicitly instantiates it.
// The firmware does not allocate this maximum-capacity coordinator globally;
// future phases can choose PSRAM, internal storage, or smaller test capacities.
template <size_t HistoryCapacity = kMaxHistoryEntries,
          size_t OutboxCapacity = kMaxOutboxEntries>
class FeatureCoordinator {
 public:
  DomainStore& domain() { return domain_; }
  const DomainStore& domain() const { return domain_; }
  EventHistory<HistoryCapacity>& history() { return history_; }
  const EventHistory<HistoryCapacity>& history() const { return history_; }
  OutboxQueue<OutboxCapacity>& outbox() { return outbox_; }
  const OutboxQueue<OutboxCapacity>& outbox() const { return outbox_; }

  ResultCode recordIncoming(const EventHeader& event) {
    return history_.append(event);
  }

  ResultCode queueOutgoing(const EventHeader& event, bool persistenceConfirmed) {
    if (history_.contains(event.eventId)) return ResultCode::Duplicate;
    const ResultCode queued = outbox_.enqueue(event, persistenceConfirmed);
    if (queued != ResultCode::Ok) return queued;
    return history_.append(event);
  }

 private:
  DomainStore domain_;
  EventHistory<HistoryCapacity> history_;
  OutboxQueue<OutboxCapacity> outbox_;
};

}  // namespace friendmesh
