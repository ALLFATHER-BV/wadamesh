#pragma once

#include "friendmesh/core/FriendMeshDomain.h"

namespace friendmesh {

// Development-facing view of a contact already owned by WadaMesh/MeshCore.
// A production adapter will derive the stable reference under the shared
// security design; this layer never copies or owns MeshCore contact storage.
struct ExistingContactSnapshot {
  Id128 meshIdentity;
  char label[kMaxFriendLabelBytes + 1];
  uint32_t lastAuthenticatedAt;
  bool available;
};

class ExistingContactDirectory {
 public:
  virtual ~ExistingContactDirectory() = default;
  virtual ResultCode lookup(const Id128& meshIdentity,
                            ExistingContactSnapshot& snapshot) const = 0;
};

class TrustedContactRepository {
 public:
  TrustedContactRepository(DomainStore& domain,
                           const ExistingContactDirectory& contacts)
      : domain_(domain), contacts_(contacts) {}

  ResultCode importCandidate(const Id128& friendId,
                             const Id128& meshIdentity,
                             const Id128& signingIdentity,
                             bool emergencyContactAllowed);
  ResultCode verify(const Id128& friendId, uint32_t authenticatedAt);
  ResultCode setBlocked(const Id128& friendId, bool blocked);
  const FriendRecord* byId(const Id128& friendId) const {
    return domain_.friendById(friendId);
  }

 private:
  DomainStore& domain_;
  const ExistingContactDirectory& contacts_;
};

}  // namespace friendmesh
