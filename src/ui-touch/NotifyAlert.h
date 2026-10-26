// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

// What a new message sounds and buzzes (UITask::newMsgImpl, #730), pure so it is
// host-tested (test/test_notify_alert.cpp). The master Sound switch silences the chime
// only; Do Not Disturb silences chime and vibration. Which event it is (and the
// per-event switches and per-channel mutes) picks the slot both use; the vibration
// plays its beat once for a message, twice for a DM, three times for an @-mention.
namespace NotifyAlert {

enum : int { NONE = -1, MSG = 0, DM = 1, MEN = 2 };   // == TOUCH_SND_MSG / _DM / _MEN
static constexpr uint8_t MUTE_MSG = 0x1, MUTE_MEN = 0x2;   // == TOUCH_CHMUTE_MSG / _MEN

struct In {
  bool is_mention, is_dm;
  uint8_t cmute;                        // the channel's mute flags (0 for a DM)
  bool snd_mentions, snd_direct, snd_messages;   // Settings > Sound > Play a sound for
  bool sound_quiet;                     // the master Sound switch is off
  bool dnd;                             // Do Not Disturb is on
  bool vibrate_on;                      // Settings > Sound > Vibrate (boards with a motor)
};
struct Out { int slot; bool sound, vib; };

inline Out decide(const In& in) {
  Out o{NONE, false, false};
  const bool want_sound = !in.sound_quiet && !in.dnd;
  const bool want_vib = in.vibrate_on && !in.dnd;
  if (!want_sound && !want_vib) return o;
  if (in.is_mention && in.snd_mentions && !(in.cmute & MUTE_MEN)) o.slot = MEN;
  else if (in.is_dm) { if (in.snd_direct) o.slot = DM; }
  else if (in.snd_messages && !(in.cmute & MUTE_MSG)) o.slot = MSG;
  o.sound = o.slot >= 0 && want_sound;
  o.vib = o.slot >= 0 && want_vib;
  return o;
}

inline int vibBeats(int slot) { return slot == MEN ? 3 : slot == DM ? 2 : 1; }

}  // namespace NotifyAlert
