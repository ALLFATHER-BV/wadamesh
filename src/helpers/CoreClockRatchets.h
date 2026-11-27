#pragma once

#include <helpers/AutoDiscoverRTCClock.h>

// The core keeps in-RAM send-timestamp ratchets that never come down until a reboot:
// RTCClock::last_unique (getCurrentTimeUnique, which stamps adverts, logins, posts and
// ack tags) and BaseChatMesh::_txt_last_ts (direct messages, CoreChatRatchet.h). After
// a clock that ran ahead is corrected (ClockFloorRTC's trusted backward set), they keep
// counting up from the bad time, so every message still went out with the old future
// date until the next boot. The members are private and the core offers no reset, so
// reach them through the explicit-instantiation access rule (access checking does not
// apply to the names in an explicit instantiation). Drop this once the core has a
// reset of its own.
namespace core_clock_ratchets {

template <typename Tag, typename Tag::type M>
struct Grant { friend typename Tag::type member(Tag) { return M; } };

struct LastUnique { typedef uint32_t mesh::RTCClock::*type; friend type member(LastUnique); };
template struct Grant<LastUnique, &mesh::RTCClock::last_unique>;

inline void resetUnique(mesh::RTCClock& clock) { clock.*member(LastUnique()) = 0; }

// Our own per-sender ratchets on top of getCurrentTimeUnique(), which is already
// monotonic. The only way it lands more than 10 min (ClockFloorRTC::TRUSTED_BACK_CAP)
// below the last stamp is a corrected clock, and then the new time wins.
inline uint32_t nextSendTs(uint32_t ts, uint32_t& last) {
  if (ts <= last && last - ts < 10UL * 60UL) ts = last + 1;
  last = ts;
  return ts;
}

}  // namespace core_clock_ratchets
