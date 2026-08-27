# Testing Lua audio playback

This guide tests issue #315 with a real WAV or MP3. The examples use the
T-Deck environment and the provided `scripts/audio_test.lua` app.

## Build and flash

Build and upload the branch as usual:

```sh
pio run -e LilyGo_TDeck_companion_radio_touch
pio run -e LilyGo_TDeck_companion_radio_touch -t upload
```

Keep the serial monitor available for `[LUAAPP]` or storage errors:

```sh
pio device monitor -b 115200
```

## Quick MP3 test from SD

1. Use a FAT32 SD card and create these paths on it:

   ```text
   /Music/test.mp3
   /meshcomod/apps/audio_test.lua
   ```

2. Copy `scripts/audio_test.lua` from this repository to the second path. Rename
   your MP3 to `test.mp3`, or edit `SD_PATH` near the top of the Lua file.
3. Insert the card before boot, then reboot the T-Deck.
4. Enable **Settings > Sound** and set volume above zero.
5. Open **Apps > audio_test**. The screen should report
   `audio=true`, `wav=true`, and `mp3=true`.
6. Press **Play**. Expected status:

   ```text
   State: playing
   Source: sd  Format: mp3
   ```

7. Exercise **Pause**, **Resume**, and **Stop**. Pressing **Play** while already
   playing must restart/replace the current track without overlapping it.
8. Start playback and leave the app. Audio must stop immediately when the app
   closes.

The direct SD API used by the test is:

```lua
local ok, err = wada.audio.play("sd:/Music/test.mp3")
```

`sd:` paths are absolute from the physical card root. Empty segments,
backslashes, trailing slashes, `.` and `..` are rejected.

## Test app-local/internal storage

A plain filename is resolved inside the current app's private directory. For
`audio_test.lua`, the logical files are:

```text
/apps/audio_test.lua
/apps/audio_test.d/test.mp3
```

On T-Deck and Pager, internal app storage is SPIFFS. The least destructive way
to populate it is the repository's serial uploader:

1. Power off, remove the SD card, and reboot. This forces T-Deck app storage to
   the internal SPIFFS backend.
2. Make a short internal-storage fixture. T-Deck has a 3.375 MB SPIFFS partition
   shared with settings and history, so use the full song for SD testing and a
   small clip here:

   ```sh
   ffmpeg -y -i "/path/to/your/file.mp3" \
     -t 10 -ac 1 -ar 22050 -b:a 48k \
     /tmp/wadamesh-audio-test.mp3
   ```

3. Find the serial port with `pio device list`.
4. Install `pyserial` once if needed:

   ```sh
   python3 -m pip install pyserial
   ```

5. Upload the app and short MP3. Replace the port as needed:

   ```sh
   python3 scripts/sideload_app.py \
     --port /dev/cu.usbmodem101 \
     --remote /apps/audio_test.lua \
     scripts/audio_test.lua

   python3 scripts/sideload_app.py \
     --port /dev/cu.usbmodem101 \
     --remote /apps/audio_test.d/test.mp3 \
     --reboot \
     /tmp/wadamesh-audio-test.mp3
   ```

6. Open **Apps > audio_test** after reboot. With no card inserted, the app
   selects `test.mp3`; press **Play**.
7. Expected status is `Source: app  Format: mp3`.

The app-local API is simply:

```lua
local ok, err = wada.audio.play("test.mp3")
```

Tanmatsu uses internal FFat and T-Display P4 uses internal LittleFS. The same
serial destination `/apps/audio_test.d/test.mp3` follows the active internal
backend, so no Lua code changes are needed. Direct `sd:` playback is shown only
when `wada.sys.caps().audio_sd` is true.

Do not use `uploadfs` on a device with data you need to keep: a filesystem-image
upload replaces the internal storage partition, including settings and history.

## Error and lifecycle checks

- Disable Sound, then press **Play**: expect `Command: muted`.
- Rename the file or remove the card: expect `not found` or `no sd`.
- Try a non-WAV/MP3 extension: expect `unsupported format`.
- Remove the card during SD playback only after the baseline test. Playback must
  enter `error` or stop without rebooting; reinserting the card should allow a
  later play after the normal SD recovery delay.
- Receive a mesh notification during playback: it must not start a second audio
  stream over the media track.
- Repeatedly open/play/close the app: each close must release the worker, codec,
  amplifier, file handle, and storage lease.

## Host decoder regression test

The standalone test validates the pinned decoder and the same rolling 16 KB
input algorithm used by firmware:

```sh
c++ -std=c++17 -O2 test/test_minimp3_decoder.cpp -o /tmp/test_minimp3_decoder
/tmp/test_minimp3_decoder /path/to/your/file.mp3
```

A successful run prints decoded frames, samples per channel, sample rate,
channels, and bitrate changes. The implementation has also been checked with
CBR, VBR, leading ID3v2, trailing ID3v1, and trailing APEv2 metadata.

## Supported formats

- WAV: PCM, 16-bit, mono or stereo, 8-48 kHz.
- MP3: MPEG Layer III, mono or stereo, 8-48 kHz, CBR or VBR, with common ID3 and
  APEv2 metadata.

Stereo is downmixed to the device's mono speaker path. `status()` reports
`stopped`, `playing`, `paused`, `ended`, or `error`.
