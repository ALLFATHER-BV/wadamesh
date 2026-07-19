#pragma once

#include <stdint.h>

#include "core/FriendMeshProviders.h"

namespace friendmesh {

// Secret-free lifecycle states suitable for diagnostics and, later, small
// additions to existing WadaMesh surfaces. The foundation phase deliberately
// cannot advance beyond NotConfigured.
enum class LifecycleState : uint8_t {
  NotConfigured = 0,
  Locked,
  Ready,
  Degraded,
};

enum class TransmitState : uint8_t {
  DisabledByFoundation = 0,
};

enum class StatusReason : uint8_t {
  FoundationOnly = 0,
  StorageUnavailable,
  SecurityUnavailable,
  TransportUnavailable,
  ClockUnavailable,
  LocationUnavailable,
  Locked,
  RecoveryRequired,
  PolicyBlocked,
  Ready,
};

struct StatusSnapshot {
  LifecycleState lifecycle;
  TransmitState transmit;
  StatusReason reason;
  bool storageReady;
  bool identityReady;
  bool protocolReady;
  bool userConsent;
};

class FriendMeshFeatureService {
 public:
  // Establishes an in-memory, read-only baseline. It does not inspect or write
  // storage, create identity material, alter the radio, or register UI.
  void begin();

  StatusSnapshot status() const;

  // Providers are injected once their implementation phase begins. The shared
  // feature/domain code depends on these interfaces rather than Arduino,
  // WadaMesh UI, filesystem, or radio globals.
  void setProviders(const FeatureProviders& providers);
  const FeatureProviders& providers() const { return providers_; }

  // This is intentionally a hard gate for the foundation phase. Enabling
  // transmit later requires a reviewed API and cannot happen by changing state.
  bool canTransmit() const { return false; }

 private:
  StatusSnapshot status_ = {
      LifecycleState::NotConfigured,
      TransmitState::DisabledByFoundation,
      StatusReason::FoundationOnly,
      false,
      false,
      false,
      false,
  };
  FeatureProviders providers_ = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
};

extern FriendMeshFeatureService featureService;

}  // namespace friendmesh
