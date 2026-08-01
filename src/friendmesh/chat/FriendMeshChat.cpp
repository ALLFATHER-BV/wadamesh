#include "FriendMeshChat.h"

#include <string.h>

namespace friendmesh {

namespace {

bool headersMatch(const EventHeader& lhs, const EventHeader& rhs) {
  return idsEqual(lhs.eventId, rhs.eventId) && idsEqual(lhs.groupId, rhs.groupId) &&
         idsEqual(lhs.senderId, rhs.senderId) && lhs.type == rhs.type &&
         lhs.membershipEpoch == rhs.membershipEpoch &&
         lhs.senderSequence == rhs.senderSequence &&
         lhs.createdAt == rhs.createdAt && lhs.expiresAt == rhs.expiresAt &&
         lhs.payloadLength == rhs.payloadLength &&
         memcmp(lhs.payloadHash, rhs.payloadHash, sizeof(lhs.payloadHash)) == 0;
}

void encodeId(uint8_t* destination, const Id128& id) {
  memcpy(destination, id.bytes, kIdSize);
}

Id128 decodeId(const uint8_t* source) {
  Id128 id = {};
  memcpy(id.bytes, source, kIdSize);
  return id;
}

}  // namespace

ChatService::ChatService(DomainStore& domain, StorageProvider& payloads,
                         EventJournal& journal, OutboxStore& outboxStore)
    : domain_(domain),
      payloads_(payloads),
      journal_(journal),
      outboxStore_(outboxStore),
      messageCount_(0),
      reactionCount_(0),
      conversationCount_(0),
      conflictCount_(0) {
  memset(messages_, 0, sizeof(messages_));
  memset(reactions_, 0, sizeof(reactions_));
  memset(conversations_, 0, sizeof(conversations_));
  memset(conflicts_, 0, sizeof(conflicts_));
}

bool ChatService::memberAllowed(const EventHeader& event) const {
  const GroupRecord* group = domain_.groupById(event.groupId);
  const GroupMember* member = domain_.groupMember(event.groupId, event.senderId);
  const FriendRecord* friendRecord = domain_.friendById(event.senderId);
  return group && group->security == GroupSecurityState::ReadyForDevelopment &&
         event.membershipEpoch == group->membershipEpoch && member && friendRecord &&
         !friendRecord->blockedLocally && member->state == MemberState::Approved &&
         member->grantState == GrantState::Active &&
         member->grantedEpoch == group->membershipEpoch;
}

ConversationState* ChatService::mutableConversation(const Id128& groupId,
                                                    uint32_t activityAt) {
  for (size_t i = 0; i < conversationCount_; ++i) {
    if (idsEqual(conversations_[i].groupId, groupId)) {
      if (activityAt > conversations_[i].lastActivityAt) {
        conversations_[i].lastActivityAt = activityAt;
      }
      return &conversations_[i];
    }
  }
  if (conversationCount_ >= kMaxGroups) return nullptr;
  ConversationState& next = conversations_[conversationCount_++];
  next = {};
  next.groupId = groupId;
  next.lastActivityAt = activityAt;
  return &next;
}

const ConversationState* ChatService::conversation(const Id128& groupId) const {
  for (size_t i = 0; i < conversationCount_; ++i) {
    if (idsEqual(conversations_[i].groupId, groupId)) return &conversations_[i];
  }
  return nullptr;
}

const ChatMessageEntry* ChatService::message(const Id128& eventId) const {
  for (size_t i = 0; i < messageCount_; ++i) {
    if (idsEqual(messages_[i].event.eventId, eventId)) return &messages_[i];
  }
  return nullptr;
}

ChatMessageEntry* ChatService::mutableMessage(const Id128& eventId) {
  for (size_t i = 0; i < messageCount_; ++i) {
    if (idsEqual(messages_[i].event.eventId, eventId)) return &messages_[i];
  }
  return nullptr;
}

const ReactionRecord* ChatService::reactionAt(size_t index) const {
  return index < reactionCount_ ? &reactions_[index] : nullptr;
}

const ChatConflict* ChatService::conflictAt(size_t index) const {
  return index < conflictCount_ ? &conflicts_[index] : nullptr;
}

ResultCode ChatService::quarantine(const EventHeader& event,
                                   ChatConflictReason reason, uint32_t at) {
  if (conflictCount_ >= kMaxChatConflicts) return ResultCode::CapacityReached;
  ChatConflict& conflict = conflicts_[conflictCount_++];
  conflict = {};
  conflict.eventId = event.eventId;
  conflict.observed = event;
  conflict.reason = reason;
  conflict.quarantinedAt = at;
  ConversationState* state = mutableConversation(event.groupId, at);
  if (state) state->incompleteHistory = true;
  return ResultCode::Quarantined;
}

bool ChatService::duplicateMessageMatches(const ChatMessageEntry& existing,
                                          const EventHeader& event,
                                          const uint8_t* payload,
                                          size_t length,
                                          const Id128& replyTo) const {
  if (!headersMatch(existing.event, event) ||
      event.payloadLength != length + kChatEnvelopeHeaderBytes ||
      !idsEqual(existing.replyTo, replyTo)) {
    return false;
  }
  uint8_t stored[kMaxEventPayloadBytes] = {};
  size_t storedLength = 0;
  if (payloads_.get(existing.event.payloadHandle, stored, sizeof(stored),
                    storedLength) != ResultCode::Ok) {
    return false;
  }
  return storedLength == event.payloadLength &&
         memcmp(stored, replyTo.bytes, kIdSize) == 0 &&
         memcmp(stored + kChatEnvelopeHeaderBytes, payload, length) == 0;
}

ResultCode ChatService::queuePersisted(const EventHeader& event) {
  if (outbox_.size() >= outbox_.capacity()) return ResultCode::CapacityReached;
  OutboxEntry entry = {};
  entry.event = event;
  entry.state = OutboxState::Queued;
  entry.lastResult = ResultCode::Ok;
  entry.persistenceConfirmed = true;
  const ResultCode stored = outboxStore_.upsert(entry);
  if (stored != ResultCode::Ok) return stored;
  const ResultCode restored = outbox_.restore(entry);
  if (restored != ResultCode::Ok) {
    outboxStore_.remove(event.eventId);
  }
  return restored;
}

ResultCode ChatService::persistAndRecord(const EventHeader& event,
                                         const uint8_t* payload, size_t length,
                                         bool outgoing) {
  if (length != event.payloadLength || (length > 0 && !payload)) {
    return ResultCode::InvalidArgument;
  }
  if (outgoing && outbox_.size() >= outbox_.capacity()) {
    return ResultCode::CapacityReached;
  }
  if (length > 0) {
    const ResultCode stored = payloads_.put(event.payloadHandle, payload, length);
    if (stored != ResultCode::Ok) return stored;
  }
  const ResultCode journaled = journal_.append(event);
  if (journaled != ResultCode::Ok) {
    if (length > 0) payloads_.remove(event.payloadHandle);
    return journaled;
  }
  const ResultCode recorded = history_.append(event);
  if (recorded != ResultCode::Ok) return recorded;
  if (outgoing) return queuePersisted(event);
  return ResultCode::Ok;
}

ResultCode ChatService::createMessage(const EventHeader& event,
                                      const uint8_t* text, size_t length,
                                      const Id128& replyTo) {
  if (event.type != EventType::ChatMessage) return ResultCode::InvalidArgument;
  const ResultCode valid = validateEventHeader(event);
  if (valid != ResultCode::Ok) return valid;
  if (!memberAllowed(event)) return ResultCode::Unauthorized;
  if (message(event.eventId)) return ResultCode::Duplicate;
  if (messageCount_ >= kMaxChatMessages) return ResultCode::CapacityReached;
  if (!idIsZero(replyTo)) {
    const ChatMessageEntry* parent = message(replyTo);
    if (!parent || !idsEqual(parent->event.groupId, event.groupId)) {
      return ResultCode::NotFound;
    }
  }
  ConversationState* conversationState =
      mutableConversation(event.groupId, event.createdAt);
  if (!conversationState) return ResultCode::CapacityReached;
  if (!text || length == 0 ||
      event.payloadLength != length + kChatEnvelopeHeaderBytes) {
    return ResultCode::InvalidArgument;
  }
  uint8_t envelope[kMaxEventPayloadBytes] = {};
  encodeId(envelope, replyTo);
  memcpy(envelope + kChatEnvelopeHeaderBytes, text, length);
  const ResultCode persisted =
      persistAndRecord(event, envelope, event.payloadLength, true);
  if (persisted != ResultCode::Ok) return persisted;

  ChatMessageEntry& next = messages_[messageCount_++];
  next = {};
  next.event = event;
  next.record.eventId = event.eventId;
  next.record.groupId = event.groupId;
  next.record.senderId = event.senderId;
  next.record.textHandle = event.payloadHandle;
  next.record.textLength = static_cast<uint32_t>(length);
  next.record.originalCreatedAt = event.createdAt;
  next.record.locallyReceivedAt = event.createdAt;
  next.record.delivery = DeliveryState::Queued;
  next.replyTo = replyTo;
  next.visibility = ChatVisibility::Active;
  next.outgoing = true;
  next.read = true;
  return ResultCode::Ok;
}

ResultCode ChatService::receiveMessage(const EventHeader& event,
                                       const uint8_t* text, size_t length,
                                       uint32_t receivedAt,
                                       bool senderClockTrusted,
                                       const Id128& replyTo) {
  if (event.type != EventType::ChatMessage) return ResultCode::InvalidArgument;
  const ResultCode valid = validateEventHeader(event);
  if (valid != ResultCode::Ok) return valid;
  if (!memberAllowed(event)) return ResultCode::Unauthorized;
  const ChatMessageEntry* existing = message(event.eventId);
  if (existing) {
    return duplicateMessageMatches(*existing, event, text, length, replyTo)
               ? ResultCode::Duplicate
               : quarantine(event, ChatConflictReason::PayloadConflict,
                            receivedAt);
  }
  if (messageCount_ >= kMaxChatMessages) return ResultCode::CapacityReached;
  ConversationState* conversationState =
      mutableConversation(event.groupId, receivedAt);
  if (!conversationState) return ResultCode::CapacityReached;
  if (!text || length == 0 ||
      event.payloadLength != length + kChatEnvelopeHeaderBytes) {
    return ResultCode::InvalidArgument;
  }
  if (!idIsZero(replyTo)) {
    const ChatMessageEntry* parent = message(replyTo);
    if (!parent || !idsEqual(parent->event.groupId, event.groupId)) {
      return quarantine(event, ChatConflictReason::BrokenReference, receivedAt);
    }
  }
  uint8_t envelope[kMaxEventPayloadBytes] = {};
  encodeId(envelope, replyTo);
  memcpy(envelope + kChatEnvelopeHeaderBytes, text, length);
  const ResultCode persisted =
      persistAndRecord(event, envelope, event.payloadLength, false);
  if (persisted == ResultCode::Duplicate) {
    return quarantine(event, ChatConflictReason::StorageHandleConflict,
                      receivedAt);
  }
  if (persisted != ResultCode::Ok) {
    conversationState->incompleteHistory = true;
    return persisted;
  }

  ChatMessageEntry& next = messages_[messageCount_++];
  next = {};
  next.event = event;
  next.record.eventId = event.eventId;
  next.record.groupId = event.groupId;
  next.record.senderId = event.senderId;
  next.record.textHandle = event.payloadHandle;
  next.record.textLength = static_cast<uint32_t>(length);
  next.record.originalCreatedAt = event.createdAt;
  next.record.locallyReceivedAt = receivedAt;
  next.record.delivery = DeliveryState::Delivered;
  next.record.delayed = receivedAt > event.createdAt &&
                        receivedAt - event.createdAt >
                            kDelayedMessageThresholdSeconds;
  next.record.senderClockUntrusted = !senderClockTrusted;
  next.visibility = ChatVisibility::Active;
  next.replyTo = replyTo;
  next.read = false;
  next.mutedAtReceipt = conversationState->muted;
  if (conversationState->unreadCount != UINT16_MAX) {
    ++conversationState->unreadCount;
  }
  return ResultCode::Ok;
}

ResultCode ChatService::readMessageText(const Id128& eventId,
                                        uint8_t* destination, size_t capacity,
                                        size_t& length) const {
  length = 0;
  const ChatMessageEntry* entry = message(eventId);
  if (!entry) return ResultCode::NotFound;
  if (entry->visibility == ChatVisibility::Deleted) return ResultCode::InvalidState;
  uint8_t envelope[kMaxEventPayloadBytes] = {};
  size_t envelopeLength = 0;
  const ResultCode loaded = payloads_.get(entry->event.payloadHandle, envelope,
                                          sizeof(envelope), envelopeLength);
  if (loaded != ResultCode::Ok) return loaded;
  if (envelopeLength < kChatEnvelopeHeaderBytes ||
      envelopeLength - kChatEnvelopeHeaderBytes != entry->record.textLength) {
    return ResultCode::CorruptData;
  }
  if (!destination || capacity < entry->record.textLength) {
    return ResultCode::CapacityReached;
  }
  memcpy(destination, envelope + kChatEnvelopeHeaderBytes,
         entry->record.textLength);
  length = entry->record.textLength;
  return ResultCode::Ok;
}

ResultCode ChatService::applyReaction(const EventHeader& event,
                                      const Id128& messageId,
                                      ReactionKind reaction, bool outgoing,
                                      uint32_t receivedAt) {
  if (event.type != EventType::ChatReaction || idIsZero(messageId) ||
      reaction == ReactionKind::None || event.payloadLength != kIdSize + 1) {
    return ResultCode::InvalidArgument;
  }
  if (!memberAllowed(event)) return ResultCode::Unauthorized;
  const ChatMessageEntry* target = message(messageId);
  if (!target || !idsEqual(target->event.groupId, event.groupId) ||
      target->visibility != ChatVisibility::Active) {
    return quarantine(event, ChatConflictReason::BrokenReference, receivedAt);
  }
  size_t replacementIndex = kMaxChatReactions;
  for (size_t i = 0; i < reactionCount_; ++i) {
    if (idsEqual(reactions_[i].eventId, event.eventId)) return ResultCode::Duplicate;
    if (idsEqual(reactions_[i].messageId, messageId) &&
        idsEqual(reactions_[i].senderId, event.senderId)) {
      replacementIndex = i;
    }
  }
  if (replacementIndex == kMaxChatReactions &&
      reactionCount_ >= kMaxChatReactions) {
    return ResultCode::CapacityReached;
  }
  uint8_t payload[kIdSize + 1] = {};
  encodeId(payload, messageId);
  payload[kIdSize] = static_cast<uint8_t>(reaction);
  const ResultCode persisted =
      persistAndRecord(event, payload, sizeof(payload), outgoing);
  if (persisted != ResultCode::Ok) return persisted;

  if (replacementIndex != kMaxChatReactions) {
    reactions_[replacementIndex].eventId = event.eventId;
    reactions_[replacementIndex].reaction = reaction;
    reactions_[replacementIndex].createdAt = event.createdAt;
    return ResultCode::Ok;
  }
  ReactionRecord& next = reactions_[reactionCount_++];
  next = {};
  next.eventId = event.eventId;
  next.messageId = messageId;
  next.senderId = event.senderId;
  next.reaction = reaction;
  next.createdAt = event.createdAt;
  return ResultCode::Ok;
}

ResultCode ChatService::applyDeletion(const EventHeader& event,
                                      const Id128& messageId, bool outgoing,
                                      uint32_t receivedAt) {
  if (event.type != EventType::ChatDeleted || idIsZero(messageId) ||
      event.payloadLength != kIdSize) {
    return ResultCode::InvalidArgument;
  }
  if (!memberAllowed(event)) return ResultCode::Unauthorized;
  ChatMessageEntry* target = mutableMessage(messageId);
  if (!target || !idsEqual(target->event.groupId, event.groupId)) {
    return quarantine(event, ChatConflictReason::BrokenReference, receivedAt);
  }
  if (target->visibility == ChatVisibility::Deleted) return ResultCode::Duplicate;
  const GroupMember* deleter = domain_.groupMember(event.groupId, event.senderId);
  if (!idsEqual(event.senderId, target->event.senderId) &&
      (!deleter || deleter->role != MemberRole::Admin)) {
    return ResultCode::Unauthorized;
  }
  uint8_t payload[kIdSize] = {};
  encodeId(payload, messageId);
  const ResultCode persisted =
      persistAndRecord(event, payload, sizeof(payload), outgoing);
  if (persisted != ResultCode::Ok) return persisted;
  target->visibility = ChatVisibility::Deleted;
  target->record.delivery = DeliveryState::Deleted;
  const ResultCode removed = payloads_.remove(target->event.payloadHandle);
  if (removed != ResultCode::Ok && removed != ResultCode::NotFound) {
    ConversationState* state = mutableConversation(event.groupId, receivedAt);
    if (state) state->incompleteHistory = true;
  }
  return ResultCode::Ok;
}

ResultCode ChatService::setMuted(const Id128& groupId, bool muted) {
  if (!domain_.groupById(groupId)) return ResultCode::NotFound;
  ConversationState* state = mutableConversation(groupId, 0);
  if (!state) return ResultCode::CapacityReached;
  state->muted = muted;
  return ResultCode::Ok;
}

ResultCode ChatService::markRead(const Id128& groupId) {
  ConversationState* state = mutableConversation(groupId, 0);
  if (!state) return ResultCode::CapacityReached;
  state->unreadCount = 0;
  for (size_t i = 0; i < messageCount_; ++i) {
    if (idsEqual(messages_[i].event.groupId, groupId)) messages_[i].read = true;
  }
  return ResultCode::Ok;
}

ResultCode ChatService::markHistoryIncomplete(const Id128& groupId,
                                              bool incomplete) {
  ConversationState* state = mutableConversation(groupId, 0);
  if (!state) return ResultCode::CapacityReached;
  state->incompleteHistory = incomplete;
  return ResultCode::Ok;
}

void ChatService::updateMessageDelivery(const Id128& eventId,
                                        DeliveryState delivery) {
  ChatMessageEntry* entry = mutableMessage(eventId);
  if (entry) entry->record.delivery = delivery;
}

ResultCode ChatService::updateOutbox(const OutboxEntry& next) {
  const ResultCode persisted = outboxStore_.upsert(next);
  if (persisted != ResultCode::Ok) return persisted;
  return outbox_.replacePersisted(next);
}

ResultCode ChatService::beginTransmit(const Id128& eventId) {
  const OutboxEntry* current = outbox_.find(eventId);
  if (!current) return ResultCode::NotFound;
  if (current->state != OutboxState::Queued &&
      current->state != OutboxState::RetryWaiting) {
    return ResultCode::InvalidState;
  }
  OutboxEntry next = *current;
  next.state = OutboxState::Transmitting;
  if (next.attemptCount != UINT16_MAX) ++next.attemptCount;
  const ResultCode result = updateOutbox(next);
  if (result == ResultCode::Ok) updateMessageDelivery(eventId, DeliveryState::Transmitting);
  return result;
}

ResultCode ChatService::markRelayedOrObserved(const Id128& eventId) {
  const OutboxEntry* current = outbox_.find(eventId);
  if (!current) return ResultCode::NotFound;
  if (current->state != OutboxState::Transmitting) return ResultCode::InvalidState;
  OutboxEntry next = *current;
  next.state = OutboxState::RelayedOrObserved;
  const ResultCode result = updateOutbox(next);
  if (result == ResultCode::Ok) {
    updateMessageDelivery(eventId, DeliveryState::RelayedOrObserved);
  }
  return result;
}

ResultCode ChatService::markDelivered(const Id128& eventId) {
  const OutboxEntry* current = outbox_.find(eventId);
  if (!current) return ResultCode::NotFound;
  if (current->state != OutboxState::Transmitting &&
      current->state != OutboxState::RelayedOrObserved) {
    return ResultCode::InvalidState;
  }
  OutboxEntry next = *current;
  next.state = OutboxState::Delivered;
  const ResultCode result = updateOutbox(next);
  if (result == ResultCode::Ok) updateMessageDelivery(eventId, DeliveryState::Delivered);
  return result;
}

ResultCode ChatService::scheduleRetry(const Id128& eventId,
                                      uint32_t nextAttemptAt,
                                      ResultCode reason) {
  const OutboxEntry* current = outbox_.find(eventId);
  if (!current) return ResultCode::NotFound;
  if (current->state != OutboxState::Transmitting &&
      current->state != OutboxState::RelayedOrObserved) {
    return ResultCode::InvalidState;
  }
  OutboxEntry next = *current;
  next.state = OutboxState::RetryWaiting;
  next.nextAttemptAt = nextAttemptAt;
  next.lastResult = reason;
  return updateOutbox(next);
}

ResultCode ChatService::cancelOutgoing(const Id128& eventId) {
  const OutboxEntry* current = outbox_.find(eventId);
  if (!current) return ResultCode::NotFound;
  if (current->state == OutboxState::Delivered ||
      current->state == OutboxState::Expired) {
    return ResultCode::InvalidState;
  }
  OutboxEntry next = *current;
  next.state = OutboxState::Cancelled;
  return updateOutbox(next);
}

ResultCode ChatService::removeTerminalOutgoing(const Id128& eventId) {
  const OutboxEntry* current = outbox_.find(eventId);
  if (!current) return ResultCode::NotFound;
  if (current->state != OutboxState::Delivered &&
      current->state != OutboxState::Failed &&
      current->state != OutboxState::Expired &&
      current->state != OutboxState::Cancelled) {
    return ResultCode::InvalidState;
  }
  const ResultCode removed = outboxStore_.remove(eventId);
  if (removed != ResultCode::Ok) return removed;
  return outbox_.removeTerminal(eventId);
}

const OutboxEntry* ChatService::nextReady(uint32_t now) {
  const size_t count = outbox_.size();
  for (size_t i = 0; i < count; ++i) {
    const OutboxEntry* current = outbox_.at(i);
    if (!current || !eventIsExpired(current->event, now) ||
        current->state == OutboxState::Delivered ||
        current->state == OutboxState::Cancelled ||
        current->state == OutboxState::Failed ||
        current->state == OutboxState::Expired) {
      continue;
    }
    OutboxEntry expired = *current;
    expired.state = OutboxState::Expired;
    if (updateOutbox(expired) != ResultCode::Ok) return nullptr;
    updateMessageDelivery(expired.event.eventId, DeliveryState::Expired);
  }
  return outbox_.nextReady(now);
}

ResultCode ChatService::restoreEvent(const EventHeader& event,
                                     uint32_t restoredAt) {
  uint8_t payload[kMaxEventPayloadBytes] = {};
  size_t length = 0;
  ResultCode payloadResult = ResultCode::Ok;
  if (event.payloadLength > 0) {
    payloadResult = payloads_.get(event.payloadHandle, payload, sizeof(payload),
                                  length);
    if (payloadResult == ResultCode::Ok && length != event.payloadLength) {
      payloadResult = ResultCode::Incomplete;
    }
    if (payloadResult != ResultCode::Ok) {
      ConversationState* state = mutableConversation(event.groupId, restoredAt);
      if (state) state->incompleteHistory = true;
    }
  }
  const ResultCode historyResult = history_.append(event);
  if (historyResult != ResultCode::Ok) return historyResult;
  if (event.type == EventType::ChatMessage) {
    if (messageCount_ >= kMaxChatMessages) return ResultCode::CapacityReached;
    ChatMessageEntry& next = messages_[messageCount_++];
    next = {};
    next.event = event;
    next.record.eventId = event.eventId;
    next.record.groupId = event.groupId;
    next.record.senderId = event.senderId;
    next.record.textHandle = event.payloadHandle;
    next.record.textLength =
        event.payloadLength >= kChatEnvelopeHeaderBytes
            ? event.payloadLength - kChatEnvelopeHeaderBytes
            : 0;
    next.record.originalCreatedAt = event.createdAt;
    next.record.locallyReceivedAt = restoredAt;
    next.record.delivery = DeliveryState::Incomplete;
    next.visibility = ChatVisibility::Active;
    next.read = true;
    if (payloadResult == ResultCode::Ok &&
        length >= kChatEnvelopeHeaderBytes) {
      next.replyTo = decodeId(payload);
    }
    mutableConversation(event.groupId, restoredAt);
  } else if (event.type == EventType::ChatReaction && payloadResult == ResultCode::Ok &&
             length == kIdSize + 1) {
    const Id128 messageId = decodeId(payload);
    for (size_t i = 0; i < reactionCount_; ++i) {
      if (idsEqual(reactions_[i].messageId, messageId) &&
          idsEqual(reactions_[i].senderId, event.senderId)) {
        reactions_[i].eventId = event.eventId;
        reactions_[i].reaction = static_cast<ReactionKind>(payload[kIdSize]);
        reactions_[i].createdAt = event.createdAt;
        return ResultCode::Ok;
      }
    }
    if (reactionCount_ >= kMaxChatReactions) return ResultCode::CapacityReached;
    ReactionRecord& nextReaction = reactions_[reactionCount_++];
    nextReaction = {};
    nextReaction.eventId = event.eventId;
    nextReaction.messageId = messageId;
    nextReaction.senderId = event.senderId;
    nextReaction.reaction = static_cast<ReactionKind>(payload[kIdSize]);
    nextReaction.createdAt = event.createdAt;
  } else if (event.type == EventType::ChatDeleted && length == kIdSize) {
    ChatMessageEntry* target = mutableMessage(decodeId(payload));
    if (!target) return ResultCode::Incomplete;
    target->visibility = ChatVisibility::Deleted;
    target->record.delivery = DeliveryState::Deleted;
  }
  return payloadResult;
}

ResultCode ChatService::restoreFromJournals(uint32_t restoredAt) {
  for (size_t i = 0; i < journal_.size(); ++i) {
    const EventHeader* event = journal_.at(i);
    if (!event) return ResultCode::CorruptData;
    const ResultCode restored = restoreEvent(*event, restoredAt);
    if (restored != ResultCode::Ok && restored != ResultCode::Incomplete &&
        restored != ResultCode::CorruptData && restored != ResultCode::NotFound) {
      return restored;
    }
  }
  for (size_t i = 0; i < outboxStore_.size(); ++i) {
    const OutboxEntry* entry = outboxStore_.at(i);
    if (!entry) return ResultCode::CorruptData;
    const ResultCode restored = outbox_.restore(*entry);
    if (restored != ResultCode::Ok) return restored;
    switch (entry->state) {
      case OutboxState::Queued:
      case OutboxState::RetryWaiting:
        updateMessageDelivery(entry->event.eventId, DeliveryState::Queued);
        break;
      case OutboxState::Transmitting:
        updateMessageDelivery(entry->event.eventId, DeliveryState::Transmitting);
        break;
      case OutboxState::RelayedOrObserved:
        updateMessageDelivery(entry->event.eventId,
                              DeliveryState::RelayedOrObserved);
        break;
      case OutboxState::Delivered:
        updateMessageDelivery(entry->event.eventId, DeliveryState::Delivered);
        break;
      case OutboxState::Expired:
        updateMessageDelivery(entry->event.eventId, DeliveryState::Expired);
        break;
      case OutboxState::Failed:
        updateMessageDelivery(entry->event.eventId, DeliveryState::Failed);
        break;
      case OutboxState::Cancelled:
      case OutboxState::Empty:
        break;
    }
  }
  return ResultCode::Ok;
}

}  // namespace friendmesh
