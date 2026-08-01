#include "FriendMeshDevelopmentStorage.h"

#include <string.h>

namespace friendmesh {

DevelopmentPayloadStorage::DevelopmentPayloadStorage()
    : internalReadOnly_(false),
      expansionAvailable_(true),
      expansionReadOnly_(false),
      failWrites_(false) {
  memset(internal_, 0, sizeof(internal_));
  memset(expansion_, 0, sizeof(expansion_));
}

ProviderState DevelopmentPayloadStorage::state() const {
  if (failWrites_) return ProviderState::Degraded;
  if (internalReadOnly_ && (!expansionAvailable_ || expansionReadOnly_)) {
    return ProviderState::Degraded;
  }
  return ProviderState::DevelopmentOnly;
}

uint32_t DevelopmentPayloadStorage::checksum(const uint8_t* data, size_t length) {
  uint32_t value = 2166136261u;
  for (size_t i = 0; i < length; ++i) {
    value ^= data[i];
    value *= 16777619u;
  }
  return value;
}

DevelopmentPayloadStorage::Slot* DevelopmentPayloadStorage::find(uint32_t handle) {
  for (size_t i = 0; i < kDevelopmentInternalPayloadRecords; ++i) {
    if (internal_[i].used && internal_[i].handle == handle) return &internal_[i];
  }
  for (size_t i = 0; i < kDevelopmentExpansionPayloadRecords; ++i) {
    if (expansion_[i].used && expansion_[i].handle == handle) return &expansion_[i];
  }
  return nullptr;
}

const DevelopmentPayloadStorage::Slot* DevelopmentPayloadStorage::find(
    uint32_t handle) const {
  for (size_t i = 0; i < kDevelopmentInternalPayloadRecords; ++i) {
    if (internal_[i].used && internal_[i].handle == handle) return &internal_[i];
  }
  for (size_t i = 0; i < kDevelopmentExpansionPayloadRecords; ++i) {
    if (expansion_[i].used && expansion_[i].handle == handle) return &expansion_[i];
  }
  return nullptr;
}

DevelopmentPayloadStorage::Slot* DevelopmentPayloadStorage::freeSlot(
    Slot* slots, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!slots[i].used) return &slots[i];
  }
  return nullptr;
}

ResultCode DevelopmentPayloadStorage::writeSlot(
    Slot& slot, uint32_t handle, const uint8_t* data, size_t length) {
  slot.handle = handle;
  slot.length = static_cast<uint16_t>(length);
  memcpy(slot.data, data, length);
  slot.checksum = checksum(data, length);
  slot.used = true;
  return ResultCode::Ok;
}

ResultCode DevelopmentPayloadStorage::put(uint32_t handle, const uint8_t* data,
                                          size_t length) {
  if (handle == 0 || !data || length == 0) return ResultCode::InvalidArgument;
  if (length > kMaxEventPayloadBytes) return ResultCode::CapacityReached;
  if (find(handle)) return ResultCode::Duplicate;
  if (failWrites_) return ResultCode::StorageUnavailable;

  Slot* slot = nullptr;
  if (!internalReadOnly_) {
    slot = freeSlot(internal_, kDevelopmentInternalPayloadRecords);
  }
  if (slot) return writeSlot(*slot, handle, data, length);
  if (!expansionAvailable_) {
    return internalReadOnly_ ? ResultCode::ReadOnly : ResultCode::CapacityReached;
  }
  if (expansionReadOnly_) return ResultCode::ReadOnly;
  slot = freeSlot(expansion_, kDevelopmentExpansionPayloadRecords);
  if (!slot) return ResultCode::CapacityReached;
  return writeSlot(*slot, handle, data, length);
}

ResultCode DevelopmentPayloadStorage::get(uint32_t handle, uint8_t* data,
                                          size_t capacity, size_t& length) {
  length = 0;
  if (handle == 0 || !data) return ResultCode::InvalidArgument;
  const Slot* slot = find(handle);
  if (!slot) return ResultCode::NotFound;
  if (slot->length > capacity) return ResultCode::CapacityReached;
  if (checksum(slot->data, slot->length) != slot->checksum) {
    return ResultCode::CorruptData;
  }
  memcpy(data, slot->data, slot->length);
  length = slot->length;
  return ResultCode::Ok;
}

ResultCode DevelopmentPayloadStorage::remove(uint32_t handle) {
  Slot* slot = find(handle);
  if (!slot) return ResultCode::NotFound;
  const PayloadPlacement where = placement(handle);
  if ((where == PayloadPlacement::Internal && internalReadOnly_) ||
      (where == PayloadPlacement::Expansion &&
       (!expansionAvailable_ || expansionReadOnly_))) {
    return ResultCode::ReadOnly;
  }
  memset(slot->data, 0, slot->length);
  *slot = {};
  return ResultCode::Ok;
}

void DevelopmentPayloadStorage::corrupt(uint32_t handle) {
  Slot* slot = find(handle);
  if (slot && slot->length > 0) slot->data[0] ^= 0x80;
}

PayloadPlacement DevelopmentPayloadStorage::placement(uint32_t handle) const {
  for (size_t i = 0; i < kDevelopmentInternalPayloadRecords; ++i) {
    if (internal_[i].used && internal_[i].handle == handle) {
      return PayloadPlacement::Internal;
    }
  }
  for (size_t i = 0; i < kDevelopmentExpansionPayloadRecords; ++i) {
    if (expansion_[i].used && expansion_[i].handle == handle) {
      return PayloadPlacement::Expansion;
    }
  }
  return PayloadPlacement::None;
}

size_t DevelopmentPayloadStorage::internalUsed() const {
  size_t count = 0;
  for (size_t i = 0; i < kDevelopmentInternalPayloadRecords; ++i) {
    if (internal_[i].used) ++count;
  }
  return count;
}

size_t DevelopmentPayloadStorage::expansionUsed() const {
  size_t count = 0;
  for (size_t i = 0; i < kDevelopmentExpansionPayloadRecords; ++i) {
    if (expansion_[i].used) ++count;
  }
  return count;
}

DevelopmentEventJournal::DevelopmentEventJournal()
    : count_(0), readOnly_(false), failWrites_(false) {
  memset(entries_, 0, sizeof(entries_));
}

ProviderState DevelopmentEventJournal::state() const {
  return readOnly_ || failWrites_ ? ProviderState::Degraded
                                 : ProviderState::DevelopmentOnly;
}

ResultCode DevelopmentEventJournal::append(const EventHeader& event) {
  const ResultCode valid = validateEventHeader(event);
  if (valid != ResultCode::Ok) return valid;
  for (size_t i = 0; i < count_; ++i) {
    if (idsEqual(entries_[i].eventId, event.eventId)) {
      return ResultCode::Duplicate;
    }
  }
  if (readOnly_) return ResultCode::ReadOnly;
  if (failWrites_) return ResultCode::StorageUnavailable;
  if (count_ >= kDevelopmentJournalRecords) return ResultCode::CapacityReached;
  entries_[count_++] = event;
  return ResultCode::Ok;
}

const EventHeader* DevelopmentEventJournal::at(size_t index) const {
  return index < count_ ? &entries_[index] : nullptr;
}

DevelopmentOutboxStore::DevelopmentOutboxStore()
    : count_(0), readOnly_(false), failWrites_(false) {
  memset(entries_, 0, sizeof(entries_));
  memset(used_, 0, sizeof(used_));
}

ProviderState DevelopmentOutboxStore::state() const {
  return readOnly_ || failWrites_ ? ProviderState::Degraded
                                 : ProviderState::DevelopmentOnly;
}

ResultCode DevelopmentOutboxStore::upsert(const OutboxEntry& entry) {
  if (entry.state == OutboxState::Empty) return ResultCode::InvalidState;
  const ResultCode valid = validateEventHeader(entry.event);
  if (valid != ResultCode::Ok) return valid;
  if (readOnly_) return ResultCode::ReadOnly;
  if (failWrites_) return ResultCode::StorageUnavailable;
  size_t freeIndex = kMaxOutboxEntries;
  for (size_t i = 0; i < kMaxOutboxEntries; ++i) {
    if (used_[i] && idsEqual(entries_[i].event.eventId, entry.event.eventId)) {
      entries_[i] = entry;
      return ResultCode::Ok;
    }
    if (!used_[i] && freeIndex == kMaxOutboxEntries) freeIndex = i;
  }
  if (freeIndex == kMaxOutboxEntries) return ResultCode::CapacityReached;
  entries_[freeIndex] = entry;
  used_[freeIndex] = true;
  ++count_;
  return ResultCode::Ok;
}

ResultCode DevelopmentOutboxStore::remove(const Id128& eventId) {
  if (readOnly_) return ResultCode::ReadOnly;
  if (failWrites_) return ResultCode::StorageUnavailable;
  for (size_t i = 0; i < kMaxOutboxEntries; ++i) {
    if (used_[i] && idsEqual(entries_[i].event.eventId, eventId)) {
      entries_[i] = {};
      used_[i] = false;
      --count_;
      return ResultCode::Ok;
    }
  }
  return ResultCode::NotFound;
}

const OutboxEntry* DevelopmentOutboxStore::at(size_t index) const {
  size_t seen = 0;
  for (size_t i = 0; i < kMaxOutboxEntries; ++i) {
    if (!used_[i]) continue;
    if (seen++ == index) return &entries_[i];
  }
  return nullptr;
}

}  // namespace friendmesh
