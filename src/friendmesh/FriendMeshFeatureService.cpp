#include "FriendMeshFeatureService.h"

namespace friendmesh {

FriendMeshFeatureService featureService;

void FriendMeshFeatureService::begin() {
  // Reassert the safe baseline on every boot. This phase has no persistent
  // FriendMesh state and intentionally performs no filesystem/NVS access.
  status_ = {
      LifecycleState::NotConfigured,
      TransmitState::DisabledByFoundation,
      false,
      false,
      false,
      false,
  };
}

StatusSnapshot FriendMeshFeatureService::status() const {
  return status_;
}

}  // namespace friendmesh
