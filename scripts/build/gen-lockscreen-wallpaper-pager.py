#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regenerate the T-Pager lock wallpaper from the shared lock artwork.

The Pager uses a native 480x222 wallpaper so LVGL does not enlarge and crop the
shared 320x240 image.  This script derives both the native-height background
and the left-hand artwork from that shared wallpaper.  No source artwork or
font rendering is recreated here.
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass
from pathlib import Path


PAGER_WIDTH = 480
PAGER_HEIGHT = 222

# These are the layout values used by the original native Pager composite.
ART_SCALE = 0.9
ART_OFFSET_X = -42.0
ART_OFFSET_Y = 26.0

# The shared wallpaper has clean background margins on both sides.  RGB565
# quantization varies individual pixels, so average both margins per row before
# extracting the foreground delta.
SHARED_BACKGROUND_MARGIN = 40
BACKGROUND_NOISE_THRESHOLD = 7.0


@dataclass(frozen=True)
class Rgb565Image:
    width: int
    height: int
    pixels: list[int]


def parse_rgb565_header(path: Path) -> Rgb565Image:
    text = path.read_text(encoding="utf-8")
    width_match = re.search(r"#define\s+\w+_W\s+(\d+)", text)
    height_match = re.search(r"#define\s+\w+_H\s+(\d+)", text)
    if not width_match or not height_match:
        raise ValueError(f"could not find dimensions in {path}")

    width = int(width_match.group(1))
    height = int(height_match.group(1))
    body = text[text.index("{") : text.rindex("}")]
    pixels = [int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{4})", body)]
    if len(pixels) != width * height:
        raise ValueError(
            f"{path} contains {len(pixels)} pixels; expected {width * height}"
        )
    return Rgb565Image(width, height, pixels)


def rgb565_to_rgb(pixel: int) -> tuple[float, float, float]:
    return (
        ((pixel >> 11) & 0x1F) * 255.0 / 31.0,
        ((pixel >> 5) & 0x3F) * 255.0 / 63.0,
        (pixel & 0x1F) * 255.0 / 31.0,
    )


def rgb_to_rgb565(rgb: tuple[float, float, float]) -> int:
    red = round(max(0.0, min(255.0, rgb[0])) * 31.0 / 255.0)
    green = round(max(0.0, min(255.0, rgb[1])) * 63.0 / 255.0)
    blue = round(max(0.0, min(255.0, rgb[2])) * 31.0 / 255.0)
    return (red << 11) | (green << 5) | blue


def shared_foreground_delta(
    image: Rgb565Image,
) -> tuple[
    list[tuple[float, float, float]],
    list[tuple[float, float, float]],
]:
    rgb = [rgb565_to_rgb(pixel) for pixel in image.pixels]
    margins = list(range(SHARED_BACKGROUND_MARGIN)) + list(
        range(image.width - SHARED_BACKGROUND_MARGIN, image.width)
    )
    row_background: list[tuple[float, float, float]] = []
    for y in range(image.height):
        samples = [rgb[y * image.width + x] for x in margins]
        row_background.append(
            tuple(sum(sample[channel] for sample in samples) / len(samples) for channel in range(3))
        )

    delta: list[tuple[float, float, float]] = []
    for y in range(image.height):
        background = row_background[y]
        for x in range(image.width):
            pixel = rgb[y * image.width + x]
            difference = tuple(pixel[channel] - background[channel] for channel in range(3))
            if max(abs(channel) for channel in difference) < BACKGROUND_NOISE_THRESHOLD:
                difference = (0.0, 0.0, 0.0)
            delta.append(difference)
    return row_background, delta


def bilinear_row(
    rows: list[tuple[float, float, float]], y: float
) -> tuple[float, float, float]:
    y = max(0.0, min(len(rows) - 1.0, y))
    y0 = int(y)
    y1 = min(len(rows) - 1, y0 + 1)
    fraction = y - y0
    return tuple(
        rows[y0][channel] * (1.0 - fraction) + rows[y1][channel] * fraction
        for channel in range(3)
    )


def bilinear_delta(
    delta: list[tuple[float, float, float]],
    width: int,
    height: int,
    x: float,
    y: float,
) -> tuple[float, float, float]:
    if x < 0.0 or x > width - 1 or y < 0.0 or y > height - 1:
        return (0.0, 0.0, 0.0)

    x0 = int(x)
    y0 = int(y)
    x1 = min(width - 1, x0 + 1)
    y1 = min(height - 1, y0 + 1)
    fx = x - x0
    fy = y - y0
    samples = (
        delta[y0 * width + x0],
        delta[y0 * width + x1],
        delta[y1 * width + x0],
        delta[y1 * width + x1],
    )
    return tuple(
        (samples[0][channel] * (1.0 - fx) + samples[1][channel] * fx) * (1.0 - fy)
        + (samples[2][channel] * (1.0 - fx) + samples[3][channel] * fx) * fy
        for channel in range(3)
    )


def compose(shared: Rgb565Image) -> list[int]:
    row_background, shared_delta = shared_foreground_delta(shared)
    rgb: list[list[list[float]]] = []

    for y in range(PAGER_HEIGHT):
        background_y = (y + 0.5) * shared.height / PAGER_HEIGHT - 0.5
        background = bilinear_row(row_background, background_y)
        row: list[list[float]] = []
        for x in range(PAGER_WIDTH):
            source_x = (x - ART_OFFSET_X + 0.5) / ART_SCALE - 0.5
            source_y = (y - ART_OFFSET_Y + 0.5) / ART_SCALE - 0.5
            foreground = bilinear_delta(
                shared_delta,
                shared.width,
                shared.height,
                source_x,
                source_y,
            )
            row.append(
                [background[channel] + foreground[channel] for channel in range(3)]
            )
        rgb.append(row)

    # Re-quantize the completed composition once, diffusing error onto the
    # display's actual 5/6/5 grid instead of introducing flat color bands.
    output: list[int] = []
    for y in range(PAGER_HEIGHT):
        for x in range(PAGER_WIDTH):
            old = rgb[y][x]
            pixel = rgb_to_rgb565(tuple(old))
            quantized = rgb565_to_rgb(pixel)
            error = [old[channel] - quantized[channel] for channel in range(3)]
            output.append(pixel)
            for dx, dy, weight in (
                (1, 0, 7.0 / 16.0),
                (-1, 1, 3.0 / 16.0),
                (0, 1, 5.0 / 16.0),
                (1, 1, 1.0 / 16.0),
            ):
                next_x = x + dx
                next_y = y + dy
                if 0 <= next_x < PAGER_WIDTH and next_y < PAGER_HEIGHT:
                    for channel in range(3):
                        rgb[next_y][next_x][channel] += error[channel] * weight
    return output


def render_header(pixels: list[int]) -> str:
    lines = [
        "// Auto-generated by scripts/build/gen-lockscreen-wallpaper-pager.py.",
        "// Native 480x222 T-LoRa Pager wallpaper derived from the shared",
        "// WADAMESH RGB565 artwork while preserving the Pager-specific layout.",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define LOCKSCREEN_WALLPAPER_PAGER_W {PAGER_WIDTH}",
        f"#define LOCKSCREEN_WALLPAPER_PAGER_H {PAGER_HEIGHT}",
        "",
        f"static const uint16_t lockscreen_wallpaper_pager_rgb565[{len(pixels)}] = {{",
    ]
    for start in range(0, len(pixels), 16):
        values = ",".join(f"0x{pixel:04x}" for pixel in pixels[start : start + 16])
        lines.append(f"  {values},")
    lines.extend(("};", ""))
    return "\n".join(lines)


def main() -> None:
    repo_root = Path(__file__).resolve().parents[2]
    default_assets = repo_root / "src" / "ui-touch" / "assets"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--shared",
        type=Path,
        default=default_assets / "lockscreen_wallpaper_rgb565.h",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=default_assets / "lockscreen_wallpaper_pager_rgb565.h",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail instead of writing when the generated Pager header is stale",
    )
    args = parser.parse_args()

    shared = parse_rgb565_header(args.shared)
    generated = render_header(compose(shared))
    current = args.output.read_text(encoding="utf-8") if args.output.exists() else ""
    if args.check:
        if current != generated:
            raise SystemExit(f"{args.output} is stale; regenerate it with this script")
        return
    args.output.write_text(generated, encoding="utf-8")


if __name__ == "__main__":
    main()
