# T-LoRa Pager: Keyboard & Rotary Encoder Guide

The LilyGo T-LoRa Pager has **no touchscreen and no trackball** — every
screen, every setting, and every chat is driven entirely by the physical
QWERTY keyboard and the rotary encoder knob. If you're coming from the
T-Deck or Heltec V4 (touch-first), this is the one board where learning a
handful of gestures up front pays off immediately. This guide covers both
radio variants (LR1121 and SX1262) — the UI and input handling are identical
on both.

## The two inputs

- **Keyboard** — a physical QWERTY matrix with a Space bar, Backspace, Enter,
  a Shift key, and an orange **Fn/Alt** key (bottom-left). No dedicated Esc,
  arrow keys, or number row — those are covered by combos below.
- **Rotary encoder** — turn to move the on-screen focus highlight; click
  (press the knob) to select. It's your only pointing device, so almost
  every screen is navigable with turn + click alone.

Everything below only fires while you're **not** actively typing into a text
field — if a field is focused and you're typing into it, letters type
normally and these combos step out of the way.

## Rotary encoder

| Gesture | Action | Keyboard equivalent |
|---|---|---|
| Turn | Move focus to the next/previous item on screen | **Fn (Alt) tapped alone** moves forward one step (NEXT only — no keyboard way to go backward) |
| Short click | Select / confirm the focused item | **Enter** |
| Hold ~1 s, then release | **Back**: closes a popup → closes an open chat → goes Home → Esc (whichever applies first) | **Backspace held ~1 s** |
| **Fn (Alt) + turn**, on a main tab | Jump directly between the 5 main tabs (Chats / Contacts / Home / Map / Settings) | none — encoder only |
| **Fn (Alt) + turn**, inside a settings page or chat | Scroll the page up/down | none — encoder only |
| Turn, with a dropdown open | Scroll through the dropdown's options | none — encoder only |
| Turn, with the accent picker open | Cycle through the accent variants (see below) | none — encoder only (Fn+Space to enter is shared) |
| Turn, with the @-mention list open | Cycle through matching contacts (see below) | none — encoder only |
| Turn, at the oldest/newest **loaded** message in an open chat | Loads more history in that direction and keeps going — see [Chat screen](#chat-screen) | none — encoder only |

The knob doubles as your only way to reach the bottom tab bar — since
there's no touch to tap an icon, **Fn+turn** while on any main tab is the
fastest way to switch sections.

## Chat screen

An open chat has no touch-only jump-to-oldest/jump-to-latest buttons on this
board — everything is reachable by turning and clicking, and the encoder
behaves a bit differently here than anywhere else in the app.

### Turning through messages

Message history is loaded lazily: only the messages currently near the
viewport actually exist as on-screen items, and older/newer ones are pulled in
as you approach them. Turning the encoder normally moves the focus highlight
one message at a time, and **at the edge of what's currently loaded, it keeps
going** — the turn instead scrolls the list to pull in the next batch of
history, then continues moving the highlight onto it. You'll only ever land
outside the message list (see below) once you've genuinely reached the very
oldest or very newest message in the whole conversation — never mid-history.

### Leaving the message list

Turning **past the oldest loaded message** (top edge), once there's truly
nothing more to load, wraps around to whatever the last focusable item on the
screen is — the channel-settings gear in an open **channel**, or the Send
button in a **DM** (a DM has no gear to reach). This is plain focus wrap, the
same as reaching the end of any other list in the app; there's no chat-specific
handling for this edge.

Turning **past the newest loaded message** (bottom edge) moves into the reply
row below the messages: quick-reply (△) → emoji → text box → Send. Turning
further past Send *is* chat-specific: in a channel it goes to the
channel-settings gear; in a DM (nothing to reach there) it returns you to the
newest message instead.

### The reply row

Below the messages, turning steps through: **quick-reply (△) → emoji →
text box → Send**, in that order, in both directions — nothing is skipped.
Turning past the △ chip (toward the messages) always jumps straight to the
newest message, regardless of channel or DM.

### Backspace and Fn (Alt) — leaving vs. catching up

These two keys are your shortcuts in and out of "reading mode" on this
screen, and they do different things depending on whether you've got unread
messages:

- **Fn (Alt) tapped alone** — jumps straight to the **newest** message and
  moves focus into the text box, ready to type a reply. This is the
  deliberate "I'm done reading, let me respond" gesture — it works regardless
  of where your focus currently is in the chat.
- **Backspace (tap)** — jumps to whichever message needs your attention:
  - If there are unread messages, it jumps to the first one — right below the
    **"NEW ----"** divider — and selects it, so you can then turn forward
    through your unread messages in order, oldest-of-the-unread first.
  - If nothing is unread, it jumps to the newest message instead (same
    destination as Fn+Alt, but leaves focus on the message itself rather than
    the text box).

Backspace's override above only applies while you're inside an open chat and
*not* actively editing the text box — the usual rule from the top of this
guide; with a field focused, Backspace deletes a character as normal.
Fn (Alt) tap's override fires the same way whether or not the text box
already has focus, since landing there is the whole point of the gesture.

## Keyboard layout

Base layer (no modifier):

```
q  w  e  r  t  y  u  i  o  p
a  s  d  f  g  h  j  k  l  [Enter]
   z  x  c  v  b  n  m
[Space]
```

Hold **Fn (Alt)** for numbers/symbols instead:

```
1  2  3  4  5  6  7  8  9  0
*  /  +  -  =  :  '  "  @
   _  $  ;  ?  !  ,  .
[Space]
```

### Shift and Caps Lock

- **Hold Shift + a letter**: that letter (or letters, for as long as you
  hold Shift) types uppercase — released, typing goes back to lowercase.
  Real momentary Shift, just like a normal keyboard.
- **Hold Fn (Alt), then press Shift**: while you're editing a text field,
  toggles **Caps Lock** on/off — stays uppercase until you repeat the chord.
  Anywhere else (not editing a field), the chord does nothing.
- Shift alone, tapped with nothing else, does nothing (as expected).
- **Hold Fn (Alt), then press Backspace**: jumps straight to the **Home**
  screen — works everywhere, including while you're actively editing a field
  (unlike Alt+Shift above, this one isn't context-dependent). Doesn't delete
  a character or trigger the plain-Backspace gestures below.

### Special keys

| Key | While editing a text field | Otherwise |
|---|---|---|
| **Enter** | Send / submit / newline | Select the focused item — or, if a chat message bubble is focused, opens its action menu (Ack / Mention / Copy / Info / Block) |
| **Backspace** (tap) | Delete a character | In an open chat: jump to your first unread message, or the newest message if nothing's unread — see [Chat screen](#chat-screen) |
| **Backspace** (hold ~1 s) | — | Same as the encoder's long-press: **Back** |
| **Backspace** (hold ~1 s) *while the screen is locked* | — | Unlock the screen |
| **Space** (tap) | Types a space | — |
| **Space** (double-tap, within 250 ms) | Switches between English and your configured secondary keyboard layout | — |
| **Space** (hold ~1 s) | — | Locks the screen (shows a "Locking…" progress bar; tapping any key cancels) |
| **Fn (Alt)** tapped alone (press+release, nothing else) | — | Moves focus to the next field/item — a keyboard-only substitute for a turn of the encoder. In an open chat: jumps to the newest message and focuses the text box instead — see [Chat screen](#chat-screen) |

The **BOOT** button (top of the device) instantly wakes the screen from
idle-dim. It does *not* unlock a screen you've manually locked with the
Space-hold gesture above — for that, hold Backspace.

## Accented characters

Typing a letter that has accent variants (all vowels, plus `n c s y t z l r`,
both cases) pops up a small row of alternatives above the field. To pick one
without touch:

1. **Fn (Alt) + Space** — jumps focus into the popup.
2. **Turn the encoder** — cycles through the variants.
3. **Enter** — confirms your choice and replaces the letter you just typed.
4. **Backspace** — dismisses the popup and keeps the plain letter.

## @-mentions

Typing `@` followed by a few letters of a contact's name pops up a matching
list in a channel/group chat. Unlike the accent picker above, this one grabs
the encoder **immediately** — no Fn+Space needed, since it only appears once
you've deliberately started typing a mention:

1. **Turn the encoder** — cycles through the matching contacts.
2. **Enter / encoder click** — confirms the highlighted one, replacing
   `@partial` with `@FullName ` in your message.
3. **Backspace** — dismisses the list without touching what you typed.

## Sliders

Any focused slider (Control Center brightness, a Settings slider, the Map
zoom bar) can't be dragged without touch — instead:

- **Q** — decrease
- **E** — increase

Each press moves it proportionally (about 20 presses end-to-end) and
persists the new value immediately, the same as releasing a drag.

## Map screen panning

While the **Map** tab is active:

- **W** — pan north
- **A** — pan west
- **S** — pan south
- **D** — pan east

Each press shifts the view a quarter-screen and reloads tiles/markers as
needed.

## Keyboard backlight

**Settings → Keyboard → Keyboard backlight** cycles through three modes
(tap to advance):

- **Off** — always dark
- **On** — always lit
- **Auto** (default) — lights up on any keypress, turns off ~3 seconds after
  the last one

Unlike the T-Deck, this is a plain on/off strip under the keys, not a
dimmable brightness curve.

## Quick reference

| Input | Action |
|---|---|
| Turn encoder (or Fn tap = NEXT only) | Move focus |
| Turn, at the loaded edge of a chat | Load more history, keep moving — only exits the list at the true oldest/newest message |
| Click encoder (or Enter) | Select / confirm |
| Hold encoder ~1s (or hold Backspace ~1s) | Back |
| Fn + turn (main tab) | Switch tabs |
| Fn + turn (page/chat) | Scroll |
| Fn tap alone | Next field (in a chat: jump to newest message + focus the text box) |
| Hold Shift + letter | Momentary uppercase |
| Fn + Shift (editing a field) | Toggle Caps Lock |
| Fn + Shift (not editing a field) | Nothing |
| Fn + Backspace (anywhere) | Jump to Home |
| Fn + Space | Enter accent picker |
| @ + letters (composer) | Auto-focuses the mention list — no Fn+Space needed |
| Enter | Select / send / message action menu |
| Backspace (tap) | Delete / in a chat: jump to first unread message, or newest if none |
| Backspace (hold 1s) | Back, or unlock if locked |
| Space (tap) | Space |
| Space (double-tap) | Switch keyboard language |
| Space (hold 1s) | Lock screen |
| Q / E (slider focused) | Decrease / increase |
| W / A / S / D (Map tab) | Pan map north/west/south/east |
