#pragma once

#include <helpers/BaseChatMesh.h>
#include <helpers/CoreClockRatchets.h>

// BaseChatMesh's direct-message timestamp ratchet; see CoreClockRatchets.h.
namespace core_clock_ratchets {

struct TxtLastTs { typedef uint32_t BaseChatMesh::*type; friend type member(TxtLastTs); };
template struct Grant<TxtLastTs, &BaseChatMesh::_txt_last_ts>;

inline void resetTxt(BaseChatMesh& mesh) { mesh.*member(TxtLastTs()) = 0; }

}  // namespace core_clock_ratchets
