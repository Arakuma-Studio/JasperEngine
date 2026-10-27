#!/usr/bin/env python3
"""Check an Android screenshot for real rendering, and against its golden image.

    python3 tools/android-screen-check.py <screen.png> [--golden golden.png] [--update]
        [--width 270] [--tolerance 24] [--max-fraction 0.03] [--min-detail 0.02]

tools/android-emulator-test.sh runs it on the host for every screenshot it takes with --golden.

1. Content: the screen fails as BLANK when one colour (within 8 per channel of the most common
   pixel) covers all but --min-detail of it: a black screen, or a frame that is only the clear
   colour (nothing the scene drew reached the screen).
2. Golden: the screenshot, box-filtered to --width pixels wide (height by its aspect: 270 x 600
   for the emulator's 1080 x 2400), is compared with the golden by tools/mcp/pngdiff.py: a pixel
   differs when a channel differs by more than --tolerance, and at most --max-fraction of the
   pixels may. The scaled screenshot is written next to it as <name>.scaled.png, and the
   differing pixels (red over a grey copy) as <name>.diff.png.
   --update (or JASPER_UPDATE_GOLDEN=1) writes the scaled screenshot as the golden instead, once
   the content check passed; a missing golden fails.

Prints one line per check (CONTENT / GOLDEN, ok or the reason). Exit 0 when every check passed,
1 otherwise, 2 when an image cannot be read.
"""

from __future__ import annotations

import argparse
import os
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "mcp"))
import pngdiff  # noqa: E402

# a pixel belongs to the dominant colour within this much per channel (dithering, banding)
DOMINANT_TOLERANCE = 8


def dominant_fraction(img) -> tuple[float, tuple[int, int, int]]:
    """(fraction of the pixels within DOMINANT_TOLERANCE of the most common colour, that colour)."""
    width, height, rgba = img
    count = width * height
    if count == 0:
        return 1.0, (0, 0, 0)
    colours = Counter(bytes(rgba[i:i + 3]) for i in range(0, count * 4, 4))
    mode = colours.most_common(1)[0][0]
    tol = DOMINANT_TOLERANCE
    close = sum(n for colour, n in colours.items()
                if abs(colour[0] - mode[0]) <= tol and abs(colour[1] - mode[1]) <= tol and abs(colour[2] - mode[2]) <= tol)
    return close / count, (mode[0], mode[1], mode[2])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("screen")
    parser.add_argument("--golden")
    parser.add_argument("--update", action="store_true", default=os.environ.get("JASPER_UPDATE_GOLDEN") == "1")
    parser.add_argument("--width", type=int, default=270)
    parser.add_argument("--tolerance", type=int, default=24)
    parser.add_argument("--max-fraction", type=float, default=0.03)
    parser.add_argument("--min-detail", type=float, default=0.02)
    args = parser.parse_args(argv)

    screen_path = Path(args.screen)
    try:
        screen = pngdiff.load(screen_path)
    except (OSError, ValueError) as e:
        print(f"CONTENT  cannot read {screen_path}: {e}")
        return 2

    failed = False
    width, height, _ = screen
    fraction, colour = dominant_fraction(screen)
    if 1.0 - fraction < args.min_detail:
        print(f"CONTENT  BLANK: {fraction:.1%} of the {width}x{height} screen is rgb{colour} "
              f"(less than {args.min_detail:.0%} drawn over it: a black or clear-colour-only frame)")
        failed = True
    else:
        print(f"CONTENT  ok: {1.0 - fraction:.1%} of the {width}x{height} screen differs from its dominant rgb{colour}")

    if not args.golden:
        return 1 if failed else 0

    # (a screen narrower than --width is compared as it is)
    scaled_width = min(args.width, width)
    scaled_height = max(1, round(height * scaled_width / width))
    scaled = pngdiff.downscale(screen, scaled_width, scaled_height) if scaled_width < width else screen
    stem = screen_path.with_suffix("")
    scaled_path = stem.with_name(stem.name + ".scaled.png")
    scaled_path.write_bytes(pngdiff.encode(scaled))
    golden_path = Path(args.golden)

    if args.update:
        if failed:
            print(f"GOLDEN   not updated: the screen failed its content check")
            return 1
        golden_path.parent.mkdir(parents=True, exist_ok=True)
        golden_path.write_bytes(pngdiff.encode(scaled))
        print(f"GOLDEN   updated {golden_path} ({scaled_width}x{scaled_height})")
        return 0

    if not golden_path.is_file():
        print(f"GOLDEN   MISSING {golden_path} (JASPER_UPDATE_GOLDEN=1 writes it from this screen)")
        return 1
    try:
        golden = pngdiff.load(golden_path)
    except (OSError, ValueError) as e:
        print(f"GOLDEN   cannot read {golden_path}: {e}")
        return 2
    if (golden[0], golden[1]) != (scaled[0], scaled[1]):
        print(f"GOLDEN   SIZE: the screen scales to {scaled[0]}x{scaled[1]}, {golden_path} is {golden[0]}x{golden[1]}")
        return 1
    ok, differing, diff = pngdiff.compare(scaled, golden, args.tolerance, args.max_fraction)
    diff_path = stem.with_name(stem.name + ".diff.png")
    diff_path.write_bytes(diff)
    if ok:
        print(f"GOLDEN   ok: {differing:.2%} of the pixels differ from {golden_path} "
              f"(tolerance {args.tolerance}, at most {args.max_fraction:.0%})")
    else:
        print(f"GOLDEN   DIFFERS: {differing:.2%} of the pixels differ from {golden_path} "
              f"(tolerance {args.tolerance}, at most {args.max_fraction:.0%}): {diff_path}")
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
