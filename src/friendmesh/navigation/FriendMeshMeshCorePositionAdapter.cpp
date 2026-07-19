#include "FriendMeshMeshCorePositionAdapter.h"

#include <string.h>

#include "FriendMeshNavigation.h"

namespace friendmesh {

ResultCode adaptMeshCorePosition(const MeshCorePositionInput& input,
                                 PositionRecord& output) {
  output = {};
  bool publicKeyIsZero = true;
  for (size_t i = 0; i < sizeof(input.publicKey); ++i) {
    if (input.publicKey[i] != 0) {
      publicKeyIsZero = false;
      break;
    }
  }
  // WadaMesh uses 0,0 as its no-position sentinel. Avoid turning an absent
  // contact location into a real marker in the Gulf of Guinea.
  if (publicKeyIsZero ||
      (input.latitudeE6 == 0 && input.longitudeE6 == 0)) {
    return ResultCode::InvalidArgument;
  }
  const int64_t latitudeE7 = static_cast<int64_t>(input.latitudeE6) * 10;
  const int64_t longitudeE7 = static_cast<int64_t>(input.longitudeE6) * 10;
  if (latitudeE7 < INT32_MIN || latitudeE7 > INT32_MAX ||
      longitudeE7 < INT32_MIN || longitudeE7 > INT32_MAX ||
      !positionCoordinatesValid(static_cast<int32_t>(latitudeE7),
                                static_cast<int32_t>(longitudeE7))) {
    return ResultCode::InvalidArgument;
  }
  // This is a functional bridge until FriendMesh signing identities are bound
  // in Phase 7. It is intentionally deterministic and never treated as a
  // cryptographic identity assertion.
  memcpy(output.subjectId.bytes, input.publicKey, kIdSize);
  if (idIsZero(output.subjectId)) return ResultCode::InvalidId;
  output.latitudeE7 = static_cast<int32_t>(latitudeE7);
  output.longitudeE7 = static_cast<int32_t>(longitudeE7);
  output.capturedAt = input.observedAt;
  output.receivedAt = input.receivedAt;
  output.accuracyMeters = 0;
  output.source = PositionSource::LastKnown;
  output.valid = true;
  output.hiddenByPolicy = false;
  return ResultCode::Ok;
}

}  // namespace friendmesh
