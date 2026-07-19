#include "FriendMeshTrustedContacts.h"

namespace friendmesh {

ResultCode TrustedContactRepository::importCandidate(
    const Id128& friendId, const Id128& meshIdentity,
    const Id128& signingIdentity, bool emergencyContactAllowed) {
  if (idIsZero(friendId) || idIsZero(meshIdentity) || idIsZero(signingIdentity)) {
    return ResultCode::InvalidId;
  }
  ExistingContactSnapshot existing = {};
  const ResultCode found = contacts_.lookup(meshIdentity, existing);
  if (found != ResultCode::Ok) return found;
  if (!existing.available || !idsEqual(existing.meshIdentity, meshIdentity)) {
    return ResultCode::Conflict;
  }

  FriendRecord candidate = {};
  candidate.id = friendId;
  candidate.identity.meshIdentity = meshIdentity;
  candidate.identity.signingIdentity = signingIdentity;
  if (!copyBoundedText(candidate.label, sizeof(candidate.label), existing.label,
                       kMaxFriendLabelBytes)) {
    return ResultCode::InvalidText;
  }
  candidate.verification = VerificationState::Pending;
  candidate.emergencyContactAllowed = emergencyContactAllowed;
  candidate.lastAuthenticatedAt = existing.lastAuthenticatedAt;
  return domain_.addFriend(candidate);
}

ResultCode TrustedContactRepository::verify(const Id128& friendId,
                                            uint32_t authenticatedAt) {
  FriendRecord* record = domain_.mutableFriendById(friendId);
  if (!record) return ResultCode::NotFound;
  const ResultCode updated = domain_.setFriendVerification(
      friendId, VerificationState::Verified, authenticatedAt);
  if (updated != ResultCode::Ok) return updated;
  if (record->firstVerifiedAt == 0) record->firstVerifiedAt = authenticatedAt;
  return ResultCode::Ok;
}

ResultCode TrustedContactRepository::setBlocked(const Id128& friendId,
                                                bool blocked) {
  return domain_.setFriendBlocked(friendId, blocked);
}

}  // namespace friendmesh
