# ThinkNode M9: Keyboard & D-pad Guide

The Elecrow ThinkNode M9 has **no touchscreen** — every screen, every setting,
and every chat is driven by the d-pad, the dedicated function keys, and the
QWERTY keyboard. If you're coming from the T-Deck or Heltec V4 (touch-first),
this page is the five-minute tour that makes the board feel native.

## The inputs

- **D-pad** — four arrows around a centre button (**OK**). Arrows move the
  on-screen focus highlight; OK activates whatever is focused. **Holding OK**
  is the board's long-press: it fires the held item's long-press action (a
  chat message's context menu, the SD row's hold-to-format) and unlocks the
  lock screen.
- **Function row** — dedicated keys for Chats, Home, Mentions, Advert, Map,
  Back, Mic and Ctrl (see the table below).
- **QWERTY keyboard** — the keyboard controller resolves Shift/symbol layers
  itself, so keys always deliver their final character. It latches one key at
  a time, which is why there are no chords on this board — long-press carries
  the second layer instead. Start typing in a chat and the composer focuses
  itself; any key wakes the screen from idle.

## The one rule: Back closes what you see

**Back** always closes the *top* layer, one per press: map pan mode → an open
dropdown → the power menu → the Control Center → a full-screen app → a dialog
→ any popup → an open chat → back out of the page. Nothing ever closes
invisibly beneath something else. The only exception is deliberate: progress
overlays (SD format, bulk delete) block all keys until the operation finishes.

## Function keys

| Key | Press | Hold |
|---|---|---|
| **MSG** | Jump to the Chats tab (closes an open app first) | — |
| **HOME** | Peel one layer off an open app; on the Home tab, toggle the app drawer; otherwise jump Home | — |
| **@ (Mentions)** | Open the Mentions screen | — |
| **ADV** | Open the Send Advert page | **Toggle GPS on/off** |
| **MAP** | Jump to the Map tab — press again *on* the map to toggle pan mode | — |
| **BACK** | Close the top layer (see above) | — |
| **CTRL** | Open the Control Center (quick toggles, incl. the keyboard light: off / on / auto) | — |
| **MIC** | Nothing yet — deliberately reserved | — |
| **OK / Enter** | Activate the focused item; send a message; run a terminal command; newline in the editor | **Long-press the focused item / unlock the lock screen** |

## Map pan mode

On the Map tab, press **MAP** again: the arrows now pan the map instead of
moving focus. **MAP** or **BACK** exits. Pan mode switches itself off whenever
you leave the map or anything opens over it, so a stale pan can never eat a
key press later. Auto-follow pauses while you pan and resumes when you exit.
Entering the map fresh always starts in normal navigation.

## Typing & editing

Inside a text field the left/right arrows move the caret — and when the caret
is already at the edge of the text, the same press steps focus *out* of the
field, so you're never stuck. Enter sends (toggleable under Settings), Back
leaves the field. In the Terminal, Enter runs the command; in the text editor
it inserts a newline.

## Lock screen

**Hold OK** to unlock — the keyboard's hardware long-press stands in for the
hold-to-unlock other boards do by touch. With "Flash on new message" enabled,
an incoming message wakes (or lock-reveals) the screen and pulses the keyboard
backlight.

## Inside apps

Store apps (Snake and friends) get the d-pad natively: arrows steer, OK taps
the centre — start, steer, and tap-to-retry all work. Display-only apps
(Airtime, RF Monitor) keep normal navigation: arrows move between the app's
own buttons or scroll the feed. **Back and Home always escape an app** — no
app can trap the keys, even while the screen is locked.

## Recent improvements (beta_66 branch)

If you last used the M9 on an earlier beta, the notable navigation changes:

- **Back restored and made consistent** — it had regressed to doing nothing
  outside text fields, and could close hidden popups beneath the Control
  Center. Both fixed; the close-what-you-see rule above now holds everywhere.
- **Apps can't strand you** — opening Advert/Mentions over a running app used
  to orphan it with no key path back; app permission dialogs are now
  answerable with the d-pad.
- **Map pan cleans up after itself** (see above — it used to leak across tab
  jumps).
- **Keyboard light "On" applies at boot**, not after the first dim cycle.
- **Terminal chat shows incoming replies**, not just your own sent lines.
- **GPS works from a cold boot** — the toggle (and ADV-hold) no longer depends
  on a boot-time detection race.
- **Spectrum analyzer** sweeps roughly twice as fast, survives read glitches
  without flattening the trace, and waits for an in-flight transmission
  instead of cutting it off when opened mid-send.

The engineering record behind these lives in
[`variants/thinknode_m9/M9_PORT.md`](variants/thinknode_m9/M9_PORT.md).
