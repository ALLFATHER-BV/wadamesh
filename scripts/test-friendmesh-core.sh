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
  "$repo_root/src/friendmesh/people/FriendMeshChannelRoster.cpp" \
  "$repo_root/src/friendmesh/people/FriendMeshMembership.cpp" \
  "$repo_root/src/friendmesh/people/FriendMeshTrustedContacts.cpp" \
  "$repo_root/src/friendmesh/safety/FriendMeshSafety.cpp" \
  -o "$test_tmp/friendmesh-core-test"

"$test_tmp/friendmesh-core-test"
