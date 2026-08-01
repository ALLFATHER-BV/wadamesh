#pragma once

#include "friendmesh/core/FriendMeshProviders.h"
#include "friendmesh/core/FriendMeshOutbox.h"

namespace friendmesh {

constexpr size_t kDevelopmentInternalPayloadRecords = 4;
constexpr size_t kDevelopmentExpansionPayloadRecords = 12;
constexpr size_t kDevelopmentJournalRecords = kMaxHistoryEntries;

enum class PayloadPlacement : uint8_t {
  None = 0,
  Internal,
  Expansion,
};

class DevelopmentPayloadStorage : public StorageProvider {
 public:
  DevelopmentPayloadStorage();

  ProviderState state() const override;
  ResultCode put(uint32_t handle, const uint8_t* data, size_t length) override;
  ResultCode get(uint32_t handle, uint8_t* data, size_t capacity,
                 size_t& length) override;
  ResultCode remove(uint32_t handle) override;

  void setInternalReadOnly(bool readOnly) { internalReadOnly_ = readOnly; }
  void setExpansionAvailable(bool available) { expansionAvailable_ = available; }
  void setExpansionReadOnly(bool readOnly) { expansionReadOnly_ = readOnly; }
  void setFailWrites(bool fail) { failWrites_ = fail; }
  void corrupt(uint32_t handle);

  PayloadPlacement placement(uint32_t handle) const;
  size_t internalUsed() const;
  size_t expansionUsed() const;

 private:
  struct Slot {
    uint32_t handle;
    uint16_t length;
    uint32_t checksum;
    bool used;
    uint8_t data[kMaxEventPayloadBytes];
  };

  Slot internal_[kDevelopmentInternalPayloadRecords];
  Slot expansion_[kDevelopmentExpansionPayloadRecords];
  bool internalReadOnly_;
  bool expansionAvailable_;
  bool expansionReadOnly_;
  bool failWrites_;

  static uint32_t checksum(const uint8_t* data, size_t length);
  Slot* find(uint32_t handle);
  const Slot* find(uint32_t handle) const;
  Slot* freeSlot(Slot* slots, size_t count);
  ResultCode writeSlot(Slot& slot, uint32_t handle, const uint8_t* data,
                       size_t length);
};

class EventJournal {
 public:
  virtual ~EventJournal() {}
  virtual ProviderState state() const = 0;
  virtual ResultCode append(const EventHeader& event) = 0;
  virtual size_t size() const = 0;
  virtual const EventHeader* at(size_t index) const = 0;
};

class DevelopmentEventJournal : public EventJournal {
 public:
  DevelopmentEventJournal();

  ProviderState state() const override;
  ResultCode append(const EventHeader& event) override;
  size_t size() const override { return count_; }
  const EventHeader* at(size_t index) const override;

  void setReadOnly(bool readOnly) { readOnly_ = readOnly; }
  void setFailWrites(bool fail) { failWrites_ = fail; }

 private:
  EventHeader entries_[kDevelopmentJournalRecords];
  size_t count_;
  bool readOnly_;
  bool failWrites_;
};

class OutboxStore {
 public:
  virtual ~OutboxStore() {}
  virtual ProviderState state() const = 0;
  virtual ResultCode upsert(const OutboxEntry& entry) = 0;
  virtual ResultCode remove(const Id128& eventId) = 0;
  virtual size_t size() const = 0;
  virtual const OutboxEntry* at(size_t index) const = 0;
};

class DevelopmentOutboxStore : public OutboxStore {
 public:
  DevelopmentOutboxStore();

  ProviderState state() const override;
  ResultCode upsert(const OutboxEntry& entry) override;
  ResultCode remove(const Id128& eventId) override;
  size_t size() const override { return count_; }
  const OutboxEntry* at(size_t index) const override;

  void setReadOnly(bool readOnly) { readOnly_ = readOnly; }
  void setFailWrites(bool fail) { failWrites_ = fail; }

 private:
  OutboxEntry entries_[kMaxOutboxEntries];
  bool used_[kMaxOutboxEntries];
  size_t count_;
  bool readOnly_;
  bool failWrites_;
};

}  // namespace friendmesh
