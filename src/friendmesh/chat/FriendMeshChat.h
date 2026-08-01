#pragma once

#include "FriendMeshDevelopmentStorage.h"
#include "friendmesh/core/FriendMeshDomain.h"
#include "friendmesh/core/FriendMeshEventHistory.h"
#include "friendmesh/core/FriendMeshFeatureModels.h"

namespace friendmesh {

constexpr size_t kMaxChatMessages = kMaxHistoryEntries;
constexpr size_t kMaxChatReactions = kMaxHistoryEntries * 2;
constexpr size_t kMaxChatConflicts = 16;
constexpr uint32_t kDelayedMessageThresholdSeconds = 300;
constexpr size_t kChatEnvelopeHeaderBytes = kIdSize;

enum class ChatVisibility : uint8_t {
  Active = 0,
  Deleted,
  Quarantined,
};

enum class ChatConflictReason : uint8_t {
  EventIdMismatch = 0,
  SenderSequenceConflict,
  PayloadConflict,
  StorageHandleConflict,
  BrokenReference,
};

struct ChatMessageEntry {
  EventHeader event;
  ChatRecord record;
  Id128 replyTo;
  ChatVisibility visibility;
  bool outgoing;
  bool read;
  bool mutedAtReceipt;
};

struct ConversationState {
  Id128 groupId;
  uint16_t unreadCount;
  uint32_t lastActivityAt;
  bool muted;
  bool incompleteHistory;
};

struct ChatConflict {
  Id128 eventId;
  EventHeader observed;
  ChatConflictReason reason;
  uint32_t quarantinedAt;
};

class ChatService {
 public:
  ChatService(DomainStore& domain, StorageProvider& payloads,
              EventJournal& journal, OutboxStore& outboxStore);

  ResultCode createMessage(const EventHeader& event, const uint8_t* text,
                           size_t length, const Id128& replyTo = {});
  ResultCode receiveMessage(const EventHeader& event, const uint8_t* text,
                            size_t length, uint32_t receivedAt,
                            bool senderClockTrusted,
                            const Id128& replyTo = {});
  ResultCode applyReaction(const EventHeader& event, const Id128& messageId,
                           ReactionKind reaction, bool outgoing,
                           uint32_t receivedAt);
  ResultCode applyDeletion(const EventHeader& event, const Id128& messageId,
                           bool outgoing, uint32_t receivedAt);

  ResultCode setMuted(const Id128& groupId, bool muted);
  ResultCode markRead(const Id128& groupId);
  ResultCode markHistoryIncomplete(const Id128& groupId, bool incomplete);

  ResultCode beginTransmit(const Id128& eventId);
  ResultCode markRelayedOrObserved(const Id128& eventId);
  ResultCode markDelivered(const Id128& eventId);
  ResultCode scheduleRetry(const Id128& eventId, uint32_t nextAttemptAt,
                           ResultCode reason);
  ResultCode cancelOutgoing(const Id128& eventId);
  ResultCode removeTerminalOutgoing(const Id128& eventId);
  const OutboxEntry* nextReady(uint32_t now);

  ResultCode restoreFromJournals(uint32_t restoredAt);

  size_t messageCount() const { return messageCount_; }
  size_t reactionCount() const { return reactionCount_; }
  size_t conflictCount() const { return conflictCount_; }
  const ChatMessageEntry* message(const Id128& eventId) const;
  ResultCode readMessageText(const Id128& eventId, uint8_t* destination,
                             size_t capacity, size_t& length) const;
  const ReactionRecord* reactionAt(size_t index) const;
  const ChatConflict* conflictAt(size_t index) const;
  const ConversationState* conversation(const Id128& groupId) const;
  const EventHistory<kMaxHistoryEntries>& history() const { return history_; }
  const OutboxQueue<kMaxOutboxEntries>& outbox() const { return outbox_; }

 private:
  DomainStore& domain_;
  StorageProvider& payloads_;
  EventJournal& journal_;
  OutboxStore& outboxStore_;
  EventHistory<kMaxHistoryEntries> history_;
  OutboxQueue<kMaxOutboxEntries> outbox_;
  ChatMessageEntry messages_[kMaxChatMessages];
  ReactionRecord reactions_[kMaxChatReactions];
  ConversationState conversations_[kMaxGroups];
  ChatConflict conflicts_[kMaxChatConflicts];
  size_t messageCount_;
  size_t reactionCount_;
  size_t conversationCount_;
  size_t conflictCount_;

  bool memberAllowed(const EventHeader& event) const;
  ConversationState* mutableConversation(const Id128& groupId,
                                         uint32_t activityAt);
  ChatMessageEntry* mutableMessage(const Id128& eventId);
  ResultCode persistAndRecord(const EventHeader& event, const uint8_t* payload,
                              size_t length, bool outgoing);
  ResultCode queuePersisted(const EventHeader& event);
  ResultCode updateOutbox(const OutboxEntry& next);
  ResultCode quarantine(const EventHeader& event, ChatConflictReason reason,
                        uint32_t at);
  bool duplicateMessageMatches(const ChatMessageEntry& existing,
                               const EventHeader& event, const uint8_t* payload,
                               size_t length, const Id128& replyTo) const;
  ResultCode restoreEvent(const EventHeader& event, uint32_t restoredAt);
  void updateMessageDelivery(const Id128& eventId, DeliveryState delivery);
};

}  // namespace friendmesh
