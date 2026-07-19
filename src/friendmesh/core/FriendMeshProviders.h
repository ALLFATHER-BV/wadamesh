#pragma once

#include "FriendMeshEvent.h"

namespace friendmesh {

enum class ProviderState : uint8_t {
  Unavailable = 0,
  DevelopmentOnly,
  Locked,
  Ready,
  Degraded,
  RecoveryRequired,
};

struct LocationFix {
  int32_t latitudeE7;
  int32_t longitudeE7;
  uint32_t capturedAt;
  uint16_t accuracyMeters;
  bool valid;
};

class StorageProvider {
 public:
  virtual ~StorageProvider() {}
  virtual ProviderState state() const = 0;
  virtual ResultCode put(uint32_t handle, const uint8_t* data, size_t length) = 0;
  virtual ResultCode get(uint32_t handle, uint8_t* data, size_t capacity,
                         size_t& length) = 0;
  virtual ResultCode remove(uint32_t handle) = 0;
};

class SecurityProvider {
 public:
  virtual ~SecurityProvider() {}
  virtual ProviderState state() const = 0;
  virtual ResultCode sign(const uint8_t* message, size_t messageLength,
                          uint8_t* signature, size_t signatureCapacity,
                          size_t& signatureLength) = 0;
  virtual ResultCode verify(const Id128& signer, const uint8_t* message,
                            size_t messageLength, const uint8_t* signature,
                            size_t signatureLength) = 0;
};

class TransportProvider {
 public:
  virtual ~TransportProvider() {}
  virtual ProviderState state() const = 0;
  virtual ResultCode send(const EventHeader& event, const uint8_t* payload,
                          size_t payloadLength) = 0;
};

class ClockProvider {
 public:
  virtual ~ClockProvider() {}
  virtual uint32_t now() const = 0;
  virtual bool timeIsTrusted() const = 0;
};

class LocationProvider {
 public:
  virtual ~LocationProvider() {}
  virtual LocationFix currentFix() const = 0;
};

class NotificationSink {
 public:
  virtual ~NotificationSink() {}
  virtual void onEventStateChanged(const EventHeader& event, ResultCode result) = 0;
};

struct FeatureProviders {
  StorageProvider* storage;
  SecurityProvider* security;
  TransportProvider* transport;
  ClockProvider* clock;
  LocationProvider* location;
  NotificationSink* notifications;
};

}  // namespace friendmesh
