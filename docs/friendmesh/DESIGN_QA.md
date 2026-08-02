# Friend Compass Design QA

- source visual truth: the original generated design reference was not checked into the repository and must be reattached or replaced before visual comparison can resume
- implementation screenshot path: unavailable; the LVGL screen must be captured from a flashed T-Deck
- viewport: 320 x 240 pixels, LilyGo T-Deck
- state: both pager states for a group member with two recent position samples, an active 45-second motion prediction, and the refined page-one distance scale/readout

## Full-view comparison evidence

Blocked. The source visual was opened and used as the implementation target, but there is no rendered T-Deck capture to place beside it. Code inspection and a successful firmware build are not visual-comparison evidence.

## Focused-region comparison evidence

Blocked for the same reason. The larger compass dial, guidance arrow/readout,
page indicators, Help modal, current-position override, and both page states
need device captures before typography, spacing, colors, wrapping, and touch
affordances can be judged.

## Findings

- [P1] Rendered implementation evidence is missing.
  - Location: Friend Compass fullscreen view on the T-Deck.
  - Evidence: the source mock is available, but no screenshot of the compiled LVGL implementation exists.
  - Impact: clipping, text wrapping, layer order, pointer visibility, dots, and paging behavior cannot be validated on the physical 320 x 240 display.
  - Fix: flash the T-Deck, collect two current position samples for one group member, open Friend Compass, and capture both page states.

## Required fidelity surfaces

- Fonts and typography: blocked pending a device capture.
- Spacing and layout rhythm: blocked pending a device capture.
- Colors and visual tokens: blocked pending a device capture.
- Image quality and asset fidelity: no raster assets are used in the implementation; dial primitives still require rendered comparison.
- Copy and content: implemented with the actual two-position limit and direct-time qualifier, but wrapping and visible hierarchy are blocked pending a device capture.

## Comparison history

- Initial pass: blocked because no rendered implementation screenshot is available.
- Fixes made from device-reported behavior: removed the page-one float-format argument mismatch that corrupted the prediction seconds, bounded the displayed lead to 45 seconds, added range labels tied to the plotted-point scale, simplified movement/confidence/freshness copy, and clear stale ranges when positioning becomes unavailable. No visual comparison loop can begin until the first device capture exists.
- Post-fix visual evidence: unavailable.

## Implementation checklist

- Flash both test T-Decks with the same firmware.
- Allow the target device to provide two recent positions at least five seconds apart.
- Open the group map, select the member, and open Friend Compass.
- Capture page one, swipe to page two, and capture page two.
- Test touch swipe, both dots, trackball left/right, vertical focus movement,
  Help, and the current-position/prediction toggle.
- On the other T-Deck, verify one Compass-start alert identifies the starter,
  shows the expected distance/unit, wakes or flashes according to preferences,
  and does not create a DM bubble. Confirm the one-second refresh does not send
  repeated alerts.
- Compare those captures with the source at the same 320 x 240 viewport and correct any P0/P1/P2 mismatch.

final result: blocked
