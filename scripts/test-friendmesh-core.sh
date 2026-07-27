#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
test_tmp="$(mktemp -d "${TMPDIR:-/tmp}/friendmesh-core.XXXXXX")"
trap 'rm -rf "$test_tmp"' EXIT

compiler="${CXX:-c++}"

"$compiler" \
  -std=c++11 \
  -Wall -Wextra -Werror -pedantic \
  -fsanitize=undefined -fno-omit-frame-pointer \
  -I"$repo_root/src" \
  "$repo_root/tests/friendmesh_core_test.cpp" \
  "$repo_root/src/friendmesh/FriendMeshFeatureService.cpp" \
  "$repo_root/src/friendmesh/app/FriendMeshDevelopmentRuntime.cpp" \
  "$repo_root/src/friendmesh/core/FriendMeshCoreTypes.cpp" \
  "$repo_root/src/friendmesh/core/FriendMeshDomain.cpp" \
  "$repo_root/src/friendmesh/core/FriendMeshEvent.cpp" \
  "$repo_root/src/friendmesh/chat/FriendMeshChat.cpp" \
  "$repo_root/src/friendmesh/chat/FriendMeshDevelopmentStorage.cpp" \
  "$repo_root/src/friendmesh/chat/FriendMeshSync.cpp" \
  "$repo_root/src/friendmesh/navigation/FriendMeshNavigation.cpp" \
  "$repo_root/src/friendmesh/navigation/FriendMeshGroupCoordination.cpp" \
  "$repo_root/src/friendmesh/navigation/FriendMeshMeshCorePositionAdapter.cpp" \
  "$repo_root/src/friendmesh/people/FriendMeshBlePresence.cpp" \
  "$repo_root/src/friendmesh/people/FriendMeshChannelInvite.cpp" \
  "$repo_root/src/friendmesh/people/FriendMeshFriendRequest.cpp" \
  "$repo_root/src/friendmesh/people/FriendMeshChannelRoster.cpp" \
  "$repo_root/src/friendmesh/people/FriendMeshMembership.cpp" \
  "$repo_root/src/friendmesh/people/FriendMeshTrustedContacts.cpp" \
  "$repo_root/src/friendmesh/safety/FriendMeshSafety.cpp" \
  -o "$test_tmp/friendmesh-core-test"

"$test_tmp/friendmesh-core-test"

# Keep the device integration's non-optimistic acceptance contract visible to
# host CI even though UITask/MyMesh depend on Arduino and cannot join this host
# executable. These checks catch accidental removal of either ACK-gated half.
grep -q 'onFriendMeshFriendAcceptanceAcknowledged' "$repo_root/src/AbstractUITask.h"
grep -q 'friend-save=deferred' "$repo_root/src/ui-touch/UITask.cpp"
grep -q 'REQUESTEE ACK confirmed' "$repo_root/src/ui-touch/UITask.cpp"
grep -q 'ACK send id=' "$repo_root/src/MyMesh.cpp"
grep -q 'ANR RAW RX' "$repo_root/src/MyMesh.cpp"
grep -q 'ANR DISPATCH enter' "$repo_root/src/MyMesh.cpp"
grep -q 'ANR DISPATCH exit' "$repo_root/src/MyMesh.cpp"
grep -q 'ANR CALLBACK enter' "$repo_root/src/MyMesh.cpp"
grep -q 'ANR RADIO TX complete' "$repo_root/src/MyMesh.cpp"
grep -q 'INBOX replace old=' "$repo_root/src/ui-touch/UITask.cpp"
grep -q 'ACCEPT reject=unknown' "$repo_root/src/MyMesh.cpp"
grep -q 'OUTGOING completed' "$repo_root/src/MyMesh.cpp"
