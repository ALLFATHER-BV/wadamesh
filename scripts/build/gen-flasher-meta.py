#!/usr/bin/env python3
# Generate the web-flasher metadata that must roll with every release:
#   - version.json            : {tag, channel, notes[]} — the site shows this ("what's new")
#   - manifest-tdeck.json      : esp-web-tools manifest (its "version" drives the
#   - manifest-heltec-v4-tft.json   install dialog, so it must equal the tag)
#
# These live next to the rolling bins in a per-channel feed dir on
# firmware.wadamesh.com (latest/ for stable, latest-beta/ for the test channel),
# so the flasher always reflects the current release without a site redeploy. The
# manifests themselves point at the IMMUTABLE per-tag bins (releases/TOUCH for
# stable, releases/BETA for beta) to dodge the latest/*.bin 4h edge cache.
#
# Usage: gen-flasher-meta.py <tag> <outdir> [notes_file] [channel]
#   notes_file: one note per non-empty line (plain text); optional ("" to skip).
#   channel:    "stable" (default) or "beta" — picks the immutable archive dir
#               (releases/TOUCH vs releases/BETA) the manifests point at, and is
#               recorded in version.json so the site/flasher can label the channel.
import sys, json, os

tag = sys.argv[1]
outdir = sys.argv[2]
notes_file = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else None
channel = sys.argv[4] if len(sys.argv) > 4 else "stable"
archive = "BETA" if channel == "beta" else "TOUCH"

notes = []
if notes_file and os.path.exists(notes_file):
    notes = [ln.strip() for ln in open(notes_file) if ln.strip() and not ln.startswith("#")]

os.makedirs(outdir, exist_ok=True)

with open(os.path.join(outdir, "version.json"), "w") as f:
    json.dump({"tag": tag, "channel": channel, "notes": notes}, f, indent=2)

# manifest filename -> (display name, merged-bin filename, chipFamily).
# chipFamily is ESP32-S3 for every board except the P4-class T-Display P4.
BOARDS = {
    "manifest-tdeck.json":            ("wadamesh - LilyGo T-Deck", "wadamesh-tdeck-merged.bin", "ESP32-S3"),
    "manifest-heltec-v4-tft.json":    ("wadamesh - Heltec V4 TFT", "wadamesh-heltec-v4-tft-merged.bin", "ESP32-S3"),
    "manifest-thinknode-m9.json":     ("wadamesh - ThinkNode M9", "wadamesh-thinknode-m9-merged.bin", "ESP32-S3"),
    "manifest-rak-tap-v2.json":       ("wadamesh - RAK WisMesh Tap V2", "wadamesh-rak-tap-v2-merged.bin", "ESP32-S3"),
    "manifest-heltec-v4-r8-tft.json": ("wadamesh - Heltec V4-R8 (experimental)", "wadamesh-heltec-v4-r8-tft-merged.bin", "ESP32-S3"),
    "manifest-tlora-pager-lr1121.json": ("wadamesh - LilyGo T-LoRa Pager (LR1121)", "wadamesh-tlora-pager-lr1121-merged.bin", "ESP32-S3"),
    "manifest-tlora-pager-sx1262.json": ("wadamesh - LilyGo T-LoRa Pager (SX1262)", "wadamesh-tlora-pager-sx1262-merged.bin", "ESP32-S3"),
    "manifest-attaky.json":           ("wadamesh - Attaky Core (experimental)", "wadamesh-attaky-merged.bin", "ESP32-S3"),
    "manifest-wio-tracker-l2.json": ("wadamesh - Seeed Wio Tracker L2 (experimental)", "wadamesh-wio-tracker-l2-merged.bin", "ESP32-S3"),
    "manifest-crowpanel-35.json": ("wadamesh - Elecrow CrowPanel Advance 3.5 (experimental)", "wadamesh-crowpanel-35-merged.bin", "ESP32-S3"),
    "manifest-tdeck-pro-v1-0.json":   ("wadamesh - LilyGo T-Deck Pro V1.0 (experimental)", "wadamesh-tdeck-pro-v1-0-merged.bin", "ESP32-S3"),
    "manifest-tdeck-pro-v1-1.json":   ("wadamesh - LilyGo T-Deck Pro V1.1 (experimental)", "wadamesh-tdeck-pro-v1-1-merged.bin", "ESP32-S3"),
    "manifest-tdeck-max.json":        ("wadamesh - LilyGo T-Deck Max (experimental)", "wadamesh-tdeck-max-merged.bin", "ESP32-S3"),
    "manifest-tdisplay-p4.json":      ("wadamesh - LilyGo T-Display P4 (AMOLED, SX1262)", "wadamesh-tdisplay-p4-merged.bin", "ESP32-P4"),
    "manifest-tdisplay-p4-lr2021.json": ("wadamesh - LilyGo T-Display P4 (AMOLED, LR2021)", "wadamesh-tdisplay-p4-lr2021-merged.bin", "ESP32-P4"),
    "manifest-tdisplay-p4-lcd.json":  ("wadamesh - LilyGo T-Display P4 (LCD, SX1262)", "wadamesh-tdisplay-p4-lcd-merged.bin", "ESP32-P4"),
    "manifest-tdisplay-p4-lcd-lr2021.json": ("wadamesh - LilyGo T-Display P4 (LCD, LR2021)", "wadamesh-tdisplay-p4-lcd-lr2021-merged.bin", "ESP32-P4"),
    # The T-Display P4 ships with one of TWO C6 co-processor firmwares and the
    # image has to match. Current V1 units carry esp-hosted (the four entries
    # above); OLDER units still have the factory ESP-AT, and on those the
    # esp-hosted image cannot reach the C6 at all. Picking the wrong one is not
    # subtle -- before b52c71f it boot-looped the board, and it still means no
    # Wi-Fi and no BLE. The label has to let someone choose without a serial
    # log, so it says what they can actually check: when it was bought.
    "manifest-tdisplay-p4-at.json":   ("wadamesh - LilyGo T-Display P4 (AMOLED, SX1262) - older unit, ESP-AT C6", "wadamesh-tdisplay-p4-at-merged.bin", "ESP32-P4"),
    "manifest-tdisplay-p4-lr2021-at.json": ("wadamesh - LilyGo T-Display P4 (AMOLED, LR2021) - older unit, ESP-AT C6", "wadamesh-tdisplay-p4-lr2021-at-merged.bin", "ESP32-P4"),
    "manifest-tdisplay-p4-lcd-at.json": ("wadamesh - LilyGo T-Display P4 (LCD, SX1262) - older unit, ESP-AT C6", "wadamesh-tdisplay-p4-lcd-at-merged.bin", "ESP32-P4"),
    "manifest-tdisplay-p4-lcd-lr2021-at.json": ("wadamesh - LilyGo T-Display P4 (LCD, LR2021) - older unit, ESP-AT C6", "wadamesh-tdisplay-p4-lcd-lr2021-at-merged.bin", "ESP32-P4"),
}
for fn, (name, binf, chip) in BOARDS.items():
    # A board that joined the matrix after this tag has no image in this feed
    # (promoting an older tag to stable). Writing a manifest for it would hand
    # the flasher a 404, so leave it out of this channel.
    if not os.path.exists(os.path.join(outdir, binf)):
        print("note: %s missing in %s -> no %s" % (binf, outdir, fn))
        continue
    manifest = {
        "name": name + ("" if channel == "stable" else " (beta)"),
        "version": tag,
        "new_install_prompt_erase": True,
        "builds": [{
            "chipFamily": chip,
            # Point at the IMMUTABLE per-tag bin, NOT the rolling feed bin. The feed
            # *.bin URLs are stable filenames cached 4h and overwritten in place, so
            # for up to 4h after a release the flasher could hand out the PREVIOUS
            # build's bytes while version.json already advertised the new tag. The
            # per-tag path is never overwritten, so its cache is always correct. The
            # manifest itself is max-age=300, so the new path propagates in <=5min.
            "parts": [{"path": f"https://firmware.wadamesh.com/releases/{archive}/{tag}/{binf}", "offset": 0}],
        }],
    }
    with open(os.path.join(outdir, fn), "w") as f:
        json.dump(manifest, f, indent=2)

print(f"wrote version.json + {len(BOARDS)} manifests for {tag} ({channel}) -> {outdir}  (notes: {len(notes)})")
