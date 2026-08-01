#pragma once

#include "friendmesh/core/FriendMeshDomain.h"
#include "friendmesh/core/FriendMeshFeatureModels.h"

namespace friendmesh {

constexpr size_t kMaxSyncIndexedEvents = kMaxHistoryEntries;
constexpr size_t kMaxSyncSenders = 32;
constexpr size_t kMaxSyncConflicts = 16;
constexpr size_t kMaxSyncBatchEvents = 8;
constexpr uint32_t kSyncSequenceWindow = 64;

enum class SyncConflictReason : uint8_t {
  EventIdMismatch = 0,
  SenderSequenceCollision,
  StaleEpoch,
  SequenceOutsideWindow,
};

struct SyncProgress {
  Id128 groupId;
  Id128 senderId;
  uint32_t membershipEpoch;
  uint32_t highestContiguousSequence;
  uint32_t highestSeenSequence;
  uint64_t pendingMask;
  uint32_t lastUpdatedAt;
  bool incomplete;
};

struct SyncInventoryRecord {
  Id128 groupId;
  Id128 senderId;
  uint32_t membershipEpoch;
  uint32_t highestContiguousSequence;
  uint32_t highestSeenSequence;
  uint32_t firstMissingSequence;
  bool incomplete;
};

struct SyncRange {
  Id128 groupId;
  Id128 senderId;
  uint32_t membershipEpoch;
  uint32_t firstSequence;
  uint32_t lastSequence;
};

struct SyncBatch {
  EventHeader events[kMaxSyncBatchEvents];
  uint8_t count;
};

struct SyncReceiptRecord {
  Id128 groupId;
  Id128 senderId;
  uint32_t membershipEpoch;
  uint32_t highestContiguousSequence;
  uint32_t receivedAt;
};

struct SyncConflict {
  EventHeader observed;
  SyncConflictReason reason;
  uint32_t quarantinedAt;
};

class SyncEngine {
 public:
  explicit SyncEngine(const DomainStore& domain);

  ResultCode observe(const EventHeader& event, uint32_t receivedAt);
  ResultCode applyBatch(const SyncBatch& batch, uint32_t receivedAt,
                        uint8_t& acceptedCount);
  const SyncProgress* progress(const Id128& groupId, const Id128& senderId,
                               uint32_t epoch) const;
  ResultCode inventory(const Id128& groupId, const Id128& senderId,
                       uint32_t epoch, SyncInventoryRecord& result) const;
  ResultCode planMissingRange(const SyncInventoryRecord& local,
                              const SyncInventoryRecord& remote,
                              uint32_t maxEvents, SyncRange& range) const;
  ResultCode buildPriorityBatch(const Id128& groupId, uint32_t epoch,
                                uint8_t maxEvents, SyncBatch& batch) const;
  ResultCode buildRangeBatch(const SyncRange& range, uint8_t maxEvents,
                             SyncBatch& batch) const;
  ResultCode makeReceipt(const Id128& groupId, const Id128& senderId,
                         uint32_t epoch, uint32_t receivedAt,
                         SyncReceiptRecord& receipt) const;

  size_t indexedEventCount() const { return eventCount_; }
  size_t conflictCount() const { return conflictCount_; }
  const SyncConflict* conflictAt(size_t index) const;

 private:
  const DomainStore& domain_;
  EventHeader events_[kMaxSyncIndexedEvents];
  SyncProgress progress_[kMaxSyncSenders];
  SyncConflict conflicts_[kMaxSyncConflicts];
  size_t eventCount_;
  size_t progressCount_;
  size_t conflictCount_;

  SyncProgress* mutableProgress(const Id128& groupId, const Id128& senderId,
                                uint32_t epoch, uint32_t at);
  ResultCode quarantine(const EventHeader& event, SyncConflictReason reason,
                        uint32_t at);
  bool memberAuthorizedAtEpoch(const EventHeader& event) const;
};

}  // namespace friendmesh
