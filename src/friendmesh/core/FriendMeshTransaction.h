#pragma once

#include <stddef.h>
#include <stdint.h>

#include "FriendMeshCoreTypes.h"

namespace friendmesh {

constexpr size_t kFriendTransactionIdBytes = 8;
constexpr size_t kFriendTransactionPeerKeyBytes = 32;
constexpr size_t kFriendTransactionPeerNameBytes = 32;
constexpr size_t kFriendTransactionCapacity = 16;
constexpr uint8_t kFriendTransactionFloodLimit = 2;
constexpr size_t kFriendTransactionJournalMaxBytes = 1536;

enum class FriendTransactionKind : uint8_t {
  Request = 1,
  Accept = 2,
  Decline = 3,
  Remove = 4,
  GroupMutation = 5,
  Sync = 6,
  Safety = 7,
};

enum class FriendTransactionStage : uint8_t {
  Prepared = 1,
  DirectSent = 2,
  AwaitingResponse = 3,
  ResponseReceived = 4,
  ConfirmationSent = 5,
  RetryingDirect = 6,
  FloodFallback = 7,
  Complete = 8,
  Failed = 9,
  Declined = 10,
  Expired = 11,
  Interrupted = 12,
};

enum FriendTransactionFlags : uint8_t {
  FriendTransactionInitiator = 1U << 0,
  FriendTransactionAllowFlood = 1U << 1,
  FriendTransactionDurable = 1U << 2,
};

struct FriendTransactionRecord {
  uint8_t transactionId[kFriendTransactionIdBytes];
  uint8_t peerPublicKey[kFriendTransactionPeerKeyBytes];
  char peerName[kFriendTransactionPeerNameBytes];
  FriendTransactionKind kind;
  FriendTransactionStage stage;
  uint8_t flags;
  uint8_t floodsUsed;
  uint8_t directAttempts;
  uint32_t createdAt;
  uint32_t updatedAt;
  uint32_t expiresAt;
};

bool friendTransactionKindValid(FriendTransactionKind kind);
bool friendTransactionStageValid(FriendTransactionStage stage);
bool friendTransactionStageTerminal(FriendTransactionStage stage);
bool friendTransactionCanFlood(uint8_t floodsUsed);
bool friendTransactionMayFlood(const FriendTransactionRecord& record);
uint8_t friendTransactionDirectAttemptLimit(FriendTransactionKind kind);

class FriendTransactionLedger {
 public:
  FriendTransactionLedger();

  void clear();
  size_t size() const { return count_; }
  const FriendTransactionRecord* at(size_t index) const;
  const FriendTransactionRecord* find(
      const uint8_t transactionId[kFriendTransactionIdBytes]) const;
  FriendTransactionRecord* mutableFind(
      const uint8_t transactionId[kFriendTransactionIdBytes]);

  ResultCode begin(
      const uint8_t transactionId[kFriendTransactionIdBytes],
      FriendTransactionKind kind, const uint8_t* peerPublicKey,
      const char* peerName, uint8_t flags, uint32_t createdAt,
      uint32_t expiresAt);
  ResultCode transition(
      const uint8_t transactionId[kFriendTransactionIdBytes],
      FriendTransactionStage stage, uint32_t updatedAt);
  ResultCode bindPeer(
      const uint8_t transactionId[kFriendTransactionIdBytes],
      const uint8_t peerPublicKey[kFriendTransactionPeerKeyBytes],
      const char* peerName, uint32_t updatedAt);
  ResultCode recordDirectAttempt(
      const uint8_t transactionId[kFriendTransactionIdBytes],
      uint32_t updatedAt);
  ResultCode recordFlood(
      const uint8_t transactionId[kFriendTransactionIdBytes],
      uint32_t updatedAt);
  size_t expire(uint32_t now, bool timeTrusted);
  size_t pruneTerminal(uint32_t now, bool timeTrusted,
                       uint32_t retentionSeconds);
  ResultCode remove(
      const uint8_t transactionId[kFriendTransactionIdBytes]);

  ResultCode encode(uint32_t generation, uint8_t* destination,
                    size_t capacity, size_t& written) const;
  ResultCode decode(const uint8_t* source, size_t length,
                    uint32_t& generation);

 private:
  FriendTransactionRecord records_[kFriendTransactionCapacity];
  size_t count_;
};

}  // namespace friendmesh
