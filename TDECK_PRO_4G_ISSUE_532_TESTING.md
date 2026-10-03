# T-Deck Pro 4G issue #532 testing

These post-flash checks verify the setup-wizard keyboard fallback and CST3530
touch fix on a LilyGo T-Deck Pro 4G.

## Choose the hardware revision

Flash `LilyGo_TDeck_Pro_v1_0_companion_radio_touch` on V1.0 hardware and
`LilyGo_TDeck_Pro_v1_1_companion_radio_touch` on V1.1 hardware. Do not use the
V1.1 image on V1.0: GPIO45 is touch reset on V1.0 but frontlight PWM on V1.1,
so the wrong image can hold the touch controller in reset. V1.0 has no
frontlight; V1.1 identifies a DRV2605 at I2C address `0x5A`.

Run the checks below separately on each available PCB revision and include the
revision and firmware target in the report.

## Boot check

With a 115200-baud serial monitor attached, reboot the device and record this
line:

```text
[BOOT] T-Deck Pro e-paper ready touch=CST3530
```

`CST3530` is expected on the affected hardware. `CST328` is also a valid Pro
revision. `missing` is a failure and should be reported with the complete boot
log.

## Open setup

If setup is already displayed, test from that screen. On an already configured
device, open:

```text
Settings > Device > Run setup again
```

This preserves the existing flash contents while reproducing the screen from
issue #532.

## Keyboard-only test

Do not touch the screen during this section.

1. On Welcome, press a WASDZ navigation key. A focus outline must appear.
2. Use `A`/`D` to focus **Get Started**, then press Return. Step 1 must open.
3. Focus the name field and press Return to enter edit mode.
4. Type several letters. They must appear in the name field.
5. Press Return to leave edit mode, navigate to **Next**, and press Return.
6. On the region list, use `W`/`Z` to move and Return to select a region.
7. Navigate to **Next** and press Return. Step 3 must open.
8. Navigate to **Finish** and press Return. The wizard must close normally.

Pass criteria: setup can be completed without touch, ordinary keys respond,
Return activates controls, and typing works in the name field.

## Touch-only test

Open **Run setup again** a second time and do not use the keyboard.

1. Tap **Get Started**. The name step must open on the first deliberate tap.
2. Tap the prefilled name field and confirm it receives focus; leave its text unchanged.
3. Tap **Next** using touch.
4. Scroll the region list in both directions, select a region, and tap **Next**.
5. Tap **Back**, confirm the region page returns, then advance again.
6. Tap **Finish**.
7. Repeat ten taps on controls, including taps immediately after an e-paper
   refresh. Each deliberate tap must register once, with no phantom taps.

Pass criteria: touch works throughout setup, scrolling remains usable, and no
tap is lost merely because the CST3530 interrupt pulse ended before the UI poll.

## Regression checks

1. Outside setup, hold Space until the device locks, then unlock with GPIO0.
2. Open a normal text field, type, erase with Backspace, and leave edit mode.
3. Reboot and confirm normal startup, keyboard input, and touch still work.
4. Confirm the serial log contains no I2C, touch, watchdog, or crash errors.

When reporting results, include the detected controller (`CST3530`, `CST328`,
or `missing`), which section failed, the exact step, and the boot log.