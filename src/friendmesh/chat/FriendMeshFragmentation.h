#pragma once

#include <string.h>

#include "friendmesh/core/FriendMeshCoreTypes.h"

namespace friendmesh {

constexpr size_t kFragmentPayloadBytes = 192;
constexpr size_t kMaxPayloadFragments =
    (kMaxEventPayloadBytes + kFragmentPayloadBytes - 1) / kFragmentPayloadBytes;

class FragmentAssembler {
 public:
  FragmentAssembler() { reset(); }

  ResultCode begin(const Id128& eventId, size_t totalLength,
                   uint8_t fragmentCount) {
    if (idIsZero(eventId) || totalLength == 0 ||
        totalLength > kMaxEventPayloadBytes || fragmentCount == 0 ||
        fragmentCount > kMaxPayloadFragments ||
        fragmentCount !=
            (totalLength + kFragmentPayloadBytes - 1) / kFragmentPayloadBytes) {
      return ResultCode::InvalidArgument;
    }
    reset();
    eventId_ = eventId;
    totalLength_ = totalLength;
    fragmentCount_ = fragmentCount;
    active_ = true;
    return ResultCode::Ok;
  }

  ResultCode accept(uint8_t index, const uint8_t* data, size_t length) {
    if (!active_ || !data || index >= fragmentCount_) {
      return ResultCode::InvalidState;
    }
    const size_t offset = static_cast<size_t>(index) * kFragmentPayloadBytes;
    const size_t expected =
        index + 1 == fragmentCount_ ? totalLength_ - offset : kFragmentPayloadBytes;
    if (length != expected) return ResultCode::InvalidArgument;
    const uint32_t bit = 1u << index;
    if ((receivedMask_ & bit) != 0) {
      return memcmp(buffer_ + offset, data, length) == 0
                 ? ResultCode::Duplicate
                 : ResultCode::Conflict;
    }
    memcpy(buffer_ + offset, data, length);
    receivedMask_ |= bit;
    return ResultCode::Ok;
  }

  bool complete() const {
    if (!active_) return false;
    const uint32_t expectedMask = (1u << fragmentCount_) - 1u;
    return receivedMask_ == expectedMask;
  }

  ResultCode copy(uint8_t* destination, size_t capacity, size_t& length) const {
    length = 0;
    if (!complete()) return ResultCode::Incomplete;
    if (!destination || capacity < totalLength_) return ResultCode::CapacityReached;
    memcpy(destination, buffer_, totalLength_);
    length = totalLength_;
    return ResultCode::Ok;
  }

  void reset() {
    eventId_ = {};
    totalLength_ = 0;
    fragmentCount_ = 0;
    receivedMask_ = 0;
    active_ = false;
    memset(buffer_, 0, sizeof(buffer_));
  }

 private:
  Id128 eventId_;
  size_t totalLength_;
  uint8_t fragmentCount_;
  uint32_t receivedMask_;
  bool active_;
  uint8_t buffer_[kMaxEventPayloadBytes];
};

}  // namespace friendmesh
