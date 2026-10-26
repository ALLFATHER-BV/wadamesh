// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test for what a new message sounds and buzzes (#730, src/ui-touch/NotifyAlert.h,
// used by UITask::newMsgImpl and the T-Display P4's vibrate task).
//
//   c++ -std=c++17 -Wall -Wextra -I src test/test_notify_alert.cpp -o /tmp/test_notify_alert \
//       && /tmp/test_notify_alert
#include <assert.h>
#include <stdio.h>

#include "ui-touch/NotifyAlert.h"

using namespace NotifyAlert;

static In base() {
  In in{};
  in.snd_mentions = in.snd_direct = in.snd_messages = true;
  in.vibrate_on = true;
  return in;
}

int main() {
  // The three events, everything on: their own slot, chime and buzz.
  { In in = base(); Out o = decide(in); assert(o.slot == MSG && o.sound && o.vib); }
  { In in = base(); in.is_dm = true; Out o = decide(in); assert(o.slot == DM && o.sound && o.vib); }
  { In in = base(); in.is_mention = true; Out o = decide(in); assert(o.slot == MEN && o.sound && o.vib); }

  // Do Not Disturb: neither, for every event.
  for (int ev = 0; ev < 3; ++ev) {
    In in = base(); in.dnd = true; in.is_dm = ev == 1; in.is_mention = ev == 2;
    Out o = decide(in); assert(!o.sound && !o.vib);
  }
  // Master Sound off: the buzz still comes ("even with Sound off"), the chime does not.
  { In in = base(); in.sound_quiet = true; Out o = decide(in); assert(o.slot == MSG && !o.sound && o.vib); }
  { In in = base(); in.sound_quiet = true; in.is_dm = true; Out o = decide(in); assert(o.slot == DM && !o.sound && o.vib); }
  // Vibrate off: the chime only. Both off: nothing.
  { In in = base(); in.vibrate_on = false; Out o = decide(in); assert(o.sound && !o.vib); }
  { In in = base(); in.vibrate_on = false; in.sound_quiet = true; Out o = decide(in); assert(o.slot == NONE && !o.sound && !o.vib); }

  // The per-event switches and channel mutes gate the buzz as they gate the chime.
  { In in = base(); in.snd_messages = false; Out o = decide(in); assert(o.slot == NONE && !o.vib); }
  { In in = base(); in.cmute = MUTE_MSG; Out o = decide(in); assert(o.slot == NONE && !o.vib); }
  { In in = base(); in.is_dm = true; in.snd_direct = false; Out o = decide(in); assert(o.slot == NONE && !o.vib); }
  // A muted DM switch does not fall back to the message slot.
  { In in = base(); in.is_dm = true; in.snd_direct = false; in.snd_messages = true; assert(decide(in).slot == NONE); }
  // A mention with mentions off (or muted) is a plain message, if messages are on.
  { In in = base(); in.is_mention = true; in.snd_mentions = false; assert(decide(in).slot == MSG); }
  { In in = base(); in.is_mention = true; in.cmute = MUTE_MEN; assert(decide(in).slot == MSG); }
  { In in = base(); in.is_mention = true; in.cmute = MUTE_MEN | MUTE_MSG; assert(decide(in).slot == NONE); }

  // The pattern plays once for a message, twice for a DM, three times for a mention.
  assert(vibBeats(MSG) == 1 && vibBeats(DM) == 2 && vibBeats(MEN) == 3);

  printf("test_notify_alert: ok\n");
  return 0;
}
