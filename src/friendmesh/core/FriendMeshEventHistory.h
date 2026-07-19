#pragma once

#include "FriendMeshEvent.h"

namespace friendmesh {

template <size_t Capacity>
class EventHistory {
 public:
  static_assert(Capacity > 0, "Event history capacity must be non-zero");

  EventHistory() : start_(0), count_(0) {}

  size_t size() const { return count_; }
  size_t capacity() const { return Capacity; }
  bool empty() const { return count_ == 0; }

  bool contains(const Id128& eventId) const {
    for (size_t i = 0; i < count_; ++i) {
      if (idsEqual(at(i)->eventId, eventId)) return true;
    }
    return false;
  }

  ResultCode append(const EventHeader& event) {
    const ResultCode validation = validateEventHeader(event);
    if (validation != ResultCode::Ok) return validation;
    if (contains(event.eventId)) return ResultCode::Duplicate;

    size_t destination = 0;
    if (count_ < Capacity) {
      destination = (start_ + count_) % Capacity;
      ++count_;
    } else {
      destination = start_;
      start_ = (start_ + 1) % Capacity;
    }
    entries_[destination] = event;
    return ResultCode::Ok;
  }

  const EventHeader* at(size_t chronologicalIndex) const {
    if (chronologicalIndex >= count_) return nullptr;
    return &entries_[(start_ + chronologicalIndex) % Capacity];
  }

  const EventHeader* newest() const {
    return count_ == 0 ? nullptr : at(count_ - 1);
  }

  void clear() {
    start_ = 0;
    count_ = 0;
  }

 private:
  EventHeader entries_[Capacity];
  size_t start_;
  size_t count_;
};

}  // namespace friendmesh
