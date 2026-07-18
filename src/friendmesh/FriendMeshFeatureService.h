#pragma once

#include <stdint.h>

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

struct StatusSnapshot {
  LifecycleState lifecycle;
  TransmitState transmit;
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

  // This is intentionally a hard gate for the foundation phase. Enabling
  // transmit later requires a reviewed API and cannot happen by changing state.
  bool canTransmit() const { return false; }

 private:
  StatusSnapshot status_ = {
      LifecycleState::NotConfigured,
      TransmitState::DisabledByFoundation,
      false,
      false,
      false,
      false,
  };
};

extern FriendMeshFeatureService featureService;

}  // namespace friendmesh
