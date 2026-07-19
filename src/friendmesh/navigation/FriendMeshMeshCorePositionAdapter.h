#pragma once

#include "FriendMeshNavigation.h"

namespace friendmesh {

// Narrow boundary between WadaMesh/MeshCore's advertised contact coordinates
// (signed microdegrees) and FriendMesh's navigation model (signed E7).
struct MeshCorePositionInput {
  uint8_t publicKey[32];
  int32_t latitudeE6;
  int32_t longitudeE6;
  uint32_t observedAt;
  uint32_t receivedAt;
};

ResultCode adaptMeshCorePosition(const MeshCorePositionInput& input,
                                 PositionRecord& output);

}  // namespace friendmesh
