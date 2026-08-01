#include "FriendMeshTransaction.h"

#include <string.h>

namespace friendmesh {
namespace {

constexpr uint8_t kMagic[] = {'F', 'M', 'T', 'J'};
constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderBytes = 16;
constexpr size_t kRecordBytes = kFriendTransactionIdBytes +
    kFriendTransactionPeerKeyBytes + kFriendTransactionPeerNameBytes + 5 + 12;

bool allZero(const uint8_t* value, size_t length) {
  if (!value) return true;
  for (size_t i = 0; i < length; ++i)
    if (value[i] != 0) return false;
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

uint32_t checksum(const uint8_t* source, size_t length) {
  uint32_t result = 2166136261UL;
  for (size_t i = 0; i < length; ++i) {
    result ^= source[i];
    result *= 16777619UL;
  }
  return result;
}

bool transitionAllowed(FriendTransactionStage from,
                       FriendTransactionStage to) {
  if (from == to) return true;
  if (friendTransactionStageTerminal(from)) return false;
  if (to == FriendTransactionStage::Failed ||
      to == FriendTransactionStage::Expired ||
      to == FriendTransactionStage::Interrupted) return true;
  switch (from) {
    case FriendTransactionStage::Prepared:
      return to == FriendTransactionStage::DirectSent ||
          to == FriendTransactionStage::AwaitingResponse ||
          to == FriendTransactionStage::RetryingDirect ||
          to == FriendTransactionStage::FloodFallback ||
          to == FriendTransactionStage::Complete;
    case FriendTransactionStage::DirectSent:
    case FriendTransactionStage::RetryingDirect:
    case FriendTransactionStage::FloodFallback:
      return to == FriendTransactionStage::AwaitingResponse ||
          to == FriendTransactionStage::RetryingDirect ||
          to == FriendTransactionStage::FloodFallback ||
          to == FriendTransactionStage::ResponseReceived ||
          to == FriendTransactionStage::ConfirmationSent ||
          to == FriendTransactionStage::Complete ||
          to == FriendTransactionStage::Declined;
    case FriendTransactionStage::AwaitingResponse:
      return to == FriendTransactionStage::RetryingDirect ||
          to == FriendTransactionStage::FloodFallback ||
          to == FriendTransactionStage::ResponseReceived ||
          to == FriendTransactionStage::ConfirmationSent ||
          to == FriendTransactionStage::Complete ||
          to == FriendTransactionStage::Declined;
    case FriendTransactionStage::ResponseReceived:
      return to == FriendTransactionStage::ConfirmationSent ||
          to == FriendTransactionStage::Complete ||
          to == FriendTransactionStage::Declined;
    case FriendTransactionStage::ConfirmationSent:
      return to == FriendTransactionStage::Complete;
    default:
      return false;
  }
}

bool recordValid(const FriendTransactionRecord& record) {
  if (allZero(record.transactionId, sizeof(record.transactionId)) ||
      !friendTransactionKindValid(record.kind) ||
      !friendTransactionStageValid(record.stage) ||
      (record.flags & ~(FriendTransactionInitiator |
                        FriendTransactionAllowFlood |
                        FriendTransactionDurable)) != 0 ||
      record.floodsUsed > kFriendTransactionFloodLimit ||
      record.directAttempts > friendTransactionDirectAttemptLimit(record.kind) ||
      (record.expiresAt != 0 && record.createdAt != 0 &&
       record.expiresAt <= record.createdAt)) return false;
  return true;
}

}  // namespace

bool friendTransactionKindValid(FriendTransactionKind kind) {
  const uint8_t value = static_cast<uint8_t>(kind);
  return value >= static_cast<uint8_t>(FriendTransactionKind::Request) &&
      value <= static_cast<uint8_t>(FriendTransactionKind::Safety);
}

bool friendTransactionStageValid(FriendTransactionStage stage) {
  const uint8_t value = static_cast<uint8_t>(stage);
  return value >= static_cast<uint8_t>(FriendTransactionStage::Prepared) &&
      value <= static_cast<uint8_t>(FriendTransactionStage::Interrupted);
}

bool friendTransactionStageTerminal(FriendTransactionStage stage) {
  return stage == FriendTransactionStage::Complete ||
      stage == FriendTransactionStage::Failed ||
      stage == FriendTransactionStage::Declined ||
      stage == FriendTransactionStage::Expired ||
      stage == FriendTransactionStage::Interrupted;
}

bool friendTransactionCanFlood(uint8_t floodsUsed) {
  return floodsUsed < kFriendTransactionFloodLimit;
}

bool friendTransactionMayFlood(const FriendTransactionRecord& record) {
  return (record.flags & FriendTransactionInitiator) != 0 &&
      (record.flags & FriendTransactionAllowFlood) != 0 &&
      friendTransactionCanFlood(record.floodsUsed) &&
      (record.kind == FriendTransactionKind::Request ||
       record.kind == FriendTransactionKind::Remove ||
       record.kind == FriendTransactionKind::GroupMutation ||
       record.kind == FriendTransactionKind::Safety);
}

uint8_t friendTransactionDirectAttemptLimit(FriendTransactionKind kind) {
  if (kind == FriendTransactionKind::Decline) return 1;
  if (kind == FriendTransactionKind::Remove) return 2;
  return 4;
}

FriendTransactionLedger::FriendTransactionLedger() : count_(0) {
  memset(records_, 0, sizeof(records_));
}

void FriendTransactionLedger::clear() {
  memset(records_, 0, sizeof(records_));
  count_ = 0;
}

const FriendTransactionRecord* FriendTransactionLedger::at(
    size_t index) const {
  return index < count_ ? &records_[index] : nullptr;
}

const FriendTransactionRecord* FriendTransactionLedger::find(
    const uint8_t transactionId[kFriendTransactionIdBytes]) const {
  if (!transactionId) return nullptr;
  for (size_t i = 0; i < count_; ++i)
    if (memcmp(records_[i].transactionId, transactionId,
               kFriendTransactionIdBytes) == 0) return &records_[i];
  return nullptr;
}

FriendTransactionRecord* FriendTransactionLedger::mutableFind(
    const uint8_t transactionId[kFriendTransactionIdBytes]) {
  return const_cast<FriendTransactionRecord*>(
      static_cast<const FriendTransactionLedger*>(this)->find(transactionId));
}

ResultCode FriendTransactionLedger::begin(
    const uint8_t transactionId[kFriendTransactionIdBytes],
    FriendTransactionKind kind, const uint8_t* peerPublicKey,
    const char* peerName, uint8_t flags, uint32_t createdAt,
    uint32_t expiresAt) {
  if (!transactionId || allZero(transactionId, kFriendTransactionIdBytes) ||
      !friendTransactionKindValid(kind) ||
      (expiresAt != 0 && createdAt != 0 && expiresAt <= createdAt))
    return ResultCode::InvalidArgument;
  if (find(transactionId)) return ResultCode::Duplicate;
  if (count_ >= kFriendTransactionCapacity) return ResultCode::CapacityReached;
  FriendTransactionRecord& record = records_[count_];
  memset(&record, 0, sizeof(record));
  memcpy(record.transactionId, transactionId, kFriendTransactionIdBytes);
  if (peerPublicKey)
    memcpy(record.peerPublicKey, peerPublicKey,
           kFriendTransactionPeerKeyBytes);
  if (peerName)
    strncpy(record.peerName, peerName, sizeof(record.peerName) - 1);
  record.kind = kind;
  record.stage = FriendTransactionStage::Prepared;
  record.flags = flags & (FriendTransactionInitiator |
                          FriendTransactionAllowFlood |
                          FriendTransactionDurable);
  record.createdAt = createdAt;
  record.updatedAt = createdAt;
  record.expiresAt = expiresAt;
  ++count_;
  return ResultCode::Ok;
}

ResultCode FriendTransactionLedger::transition(
    const uint8_t transactionId[kFriendTransactionIdBytes],
    FriendTransactionStage stage, uint32_t updatedAt) {
  FriendTransactionRecord* record = mutableFind(transactionId);
  if (!record) return ResultCode::NotFound;
  if (!friendTransactionStageValid(stage) ||
      !transitionAllowed(record->stage, stage)) return ResultCode::InvalidState;
  record->stage = stage;
  record->updatedAt = updatedAt;
  return ResultCode::Ok;
}

ResultCode FriendTransactionLedger::bindPeer(
    const uint8_t transactionId[kFriendTransactionIdBytes],
    const uint8_t peerPublicKey[kFriendTransactionPeerKeyBytes],
    const char* peerName, uint32_t updatedAt) {
  FriendTransactionRecord* record = mutableFind(transactionId);
  if (!record) return ResultCode::NotFound;
  if (!peerPublicKey || allZero(peerPublicKey, kFriendTransactionPeerKeyBytes))
    return ResultCode::InvalidArgument;
  if (!allZero(record->peerPublicKey, sizeof(record->peerPublicKey)) &&
      memcmp(record->peerPublicKey, peerPublicKey,
             sizeof(record->peerPublicKey)) != 0) return ResultCode::Conflict;
  memcpy(record->peerPublicKey, peerPublicKey, sizeof(record->peerPublicKey));
  if (peerName && peerName[0]) {
    memset(record->peerName, 0, sizeof(record->peerName));
    strncpy(record->peerName, peerName, sizeof(record->peerName) - 1);
  }
  record->updatedAt = updatedAt;
  return ResultCode::Ok;
}

ResultCode FriendTransactionLedger::recordDirectAttempt(
    const uint8_t transactionId[kFriendTransactionIdBytes],
    uint32_t updatedAt) {
  FriendTransactionRecord* record = mutableFind(transactionId);
  if (!record) return ResultCode::NotFound;
  if (friendTransactionStageTerminal(record->stage) ||
      record->directAttempts >= friendTransactionDirectAttemptLimit(record->kind))
    return ResultCode::CapacityReached;
  ++record->directAttempts;
  record->updatedAt = updatedAt;
  return ResultCode::Ok;
}

ResultCode FriendTransactionLedger::recordFlood(
    const uint8_t transactionId[kFriendTransactionIdBytes],
    uint32_t updatedAt) {
  FriendTransactionRecord* record = mutableFind(transactionId);
  if (!record) return ResultCode::NotFound;
  if (!friendTransactionMayFlood(*record)) return ResultCode::Unauthorized;
  ++record->floodsUsed;
  record->updatedAt = updatedAt;
  return ResultCode::Ok;
}

size_t FriendTransactionLedger::expire(uint32_t now, bool timeTrusted) {
  if (!timeTrusted || now == 0) return 0;
  size_t changed = 0;
  for (size_t i = 0; i < count_; ++i) {
    FriendTransactionRecord& record = records_[i];
    if (!friendTransactionStageTerminal(record.stage) &&
        record.expiresAt != 0 && now >= record.expiresAt) {
      record.stage = FriendTransactionStage::Expired;
      record.updatedAt = now;
      ++changed;
    }
  }
  return changed;
}

size_t FriendTransactionLedger::pruneTerminal(
    uint32_t now, bool timeTrusted, uint32_t retentionSeconds) {
  if (!timeTrusted || now == 0 || retentionSeconds == 0) return 0;
  size_t removed = 0;
  for (size_t i = 0; i < count_;) {
    const FriendTransactionRecord& record = records_[i];
    if (!friendTransactionStageTerminal(record.stage) ||
        record.updatedAt == 0 || now < record.updatedAt ||
        now - record.updatedAt < retentionSeconds) {
      ++i;
      continue;
    }
    const uint8_t* id = record.transactionId;
    uint8_t copy[kFriendTransactionIdBytes] = {};
    memcpy(copy, id, sizeof(copy));
    remove(copy);
    ++removed;
  }
  return removed;
}

ResultCode FriendTransactionLedger::remove(
    const uint8_t transactionId[kFriendTransactionIdBytes]) {
  if (!transactionId) return ResultCode::InvalidArgument;
  for (size_t i = 0; i < count_; ++i) {
    if (memcmp(records_[i].transactionId, transactionId,
               kFriendTransactionIdBytes) != 0) continue;
    for (size_t j = i; j + 1 < count_; ++j) records_[j] = records_[j + 1];
    memset(&records_[count_ - 1], 0, sizeof(records_[count_ - 1]));
    --count_;
    return ResultCode::Ok;
  }
  return ResultCode::NotFound;
}

ResultCode FriendTransactionLedger::encode(
    uint32_t generation, uint8_t* destination, size_t capacity,
    size_t& written) const {
  written = 0;
  const size_t required = kHeaderBytes + count_ * kRecordBytes;
  if (!destination || required > capacity ||
      required > kFriendTransactionJournalMaxBytes)
    return ResultCode::CapacityReached;
  memset(destination, 0, required);
  memcpy(destination, kMagic, sizeof(kMagic));
  destination[4] = kVersion;
  destination[5] = static_cast<uint8_t>(count_);
  destination[6] = 0;
  destination[7] = 0;
  put32(destination + 8, generation);
  size_t offset = kHeaderBytes;
  for (size_t i = 0; i < count_; ++i) {
    const FriendTransactionRecord& record = records_[i];
    if (!recordValid(record)) return ResultCode::CorruptData;
    memcpy(destination + offset, record.transactionId,
           sizeof(record.transactionId)); offset += sizeof(record.transactionId);
    memcpy(destination + offset, record.peerPublicKey,
           sizeof(record.peerPublicKey)); offset += sizeof(record.peerPublicKey);
    memcpy(destination + offset, record.peerName,
           sizeof(record.peerName)); offset += sizeof(record.peerName);
    destination[offset++] = static_cast<uint8_t>(record.kind);
    destination[offset++] = static_cast<uint8_t>(record.stage);
    destination[offset++] = record.flags;
    destination[offset++] = record.floodsUsed;
    destination[offset++] = record.directAttempts;
    put32(destination + offset, record.createdAt); offset += 4;
    put32(destination + offset, record.updatedAt); offset += 4;
    put32(destination + offset, record.expiresAt); offset += 4;
  }
  put32(destination + 12, checksum(destination, 12));
  const uint32_t bodyChecksum = checksum(destination + kHeaderBytes,
                                         required - kHeaderBytes);
  put32(destination + 12, get32(destination + 12) ^ bodyChecksum);
  written = required;
  return ResultCode::Ok;
}

ResultCode FriendTransactionLedger::decode(
    const uint8_t* source, size_t length, uint32_t& generation) {
  generation = 0;
  if (!source || length < kHeaderBytes ||
      memcmp(source, kMagic, sizeof(kMagic)) != 0 ||
      source[4] != kVersion || source[5] > kFriendTransactionCapacity ||
      source[6] != 0 || source[7] != 0 ||
      length != kHeaderBytes + source[5] * kRecordBytes ||
      length > kFriendTransactionJournalMaxBytes) return ResultCode::CorruptData;
  uint8_t header[kHeaderBytes] = {};
  memcpy(header, source, sizeof(header));
  const uint32_t storedChecksum = get32(header + 12);
  memset(header + 12, 0, 4);
  const uint32_t expectedChecksum = checksum(header, 12) ^
      checksum(source + kHeaderBytes, length - kHeaderBytes);
  if (storedChecksum != expectedChecksum) return ResultCode::CorruptData;

  FriendTransactionLedger decoded;
  size_t offset = kHeaderBytes;
  for (uint8_t i = 0; i < source[5]; ++i) {
    FriendTransactionRecord& record = decoded.records_[decoded.count_];
    memcpy(record.transactionId, source + offset,
           sizeof(record.transactionId)); offset += sizeof(record.transactionId);
    memcpy(record.peerPublicKey, source + offset,
           sizeof(record.peerPublicKey)); offset += sizeof(record.peerPublicKey);
    memcpy(record.peerName, source + offset,
           sizeof(record.peerName)); offset += sizeof(record.peerName);
    record.peerName[sizeof(record.peerName) - 1] = '\0';
    record.kind = static_cast<FriendTransactionKind>(source[offset++]);
    record.stage = static_cast<FriendTransactionStage>(source[offset++]);
    record.flags = source[offset++];
    record.floodsUsed = source[offset++];
    record.directAttempts = source[offset++];
    record.createdAt = get32(source + offset); offset += 4;
    record.updatedAt = get32(source + offset); offset += 4;
    record.expiresAt = get32(source + offset); offset += 4;
    if (!recordValid(record) || decoded.find(record.transactionId))
      return ResultCode::CorruptData;
    ++decoded.count_;
  }
  *this = decoded;
  generation = get32(source + 8);
  return ResultCode::Ok;
}

}  // namespace friendmesh
