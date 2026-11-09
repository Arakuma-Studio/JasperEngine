#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The smoke test's golden check (tools/smoke-test.sh): a game's own frame against a project's golden.

    python3 tools/ci/smoke_golden.py <project>/smoke-golden.json game.png [diff.png]

smoke-golden.json: {"image": "<png beside it>", "tolerance": 24, "max_fraction": 0.01}. The frame is
box-downscaled to the golden's size when they differ (a window larger than the golden), then compared
with tools/mcp/pngdiff.py's rule: at most `max_fraction` of the pixels with a channel over
`tolerance`/255. Exit 0 when they match; the difference image goes to diff.png. Standard library only.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mcp"))

import pngdiff  # noqa: E402


def main(argv: list[str]) -> int:
    if len(argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    spec_path, frame_path = Path(argv[1]), Path(argv[2])
    spec = json.loads(spec_path.read_text())
    golden = pngdiff.decode((spec_path.parent / spec["image"]).read_bytes())
    frame = pngdiff.decode(frame_path.read_bytes())
    if (frame[0], frame[1]) != (golden[0], golden[1]):
        frame = pngdiff.downscale(frame, golden[0], golden[1])
    ok, fraction, diff = pngdiff.compare(golden, frame, spec.get("tolerance", 24), spec.get("max_fraction", 0.01))
    if len(argv) == 4 and diff:
        Path(argv[3]).write_bytes(diff)
    verdict = "ok" if ok else "FAIL"
    print(f"{verdict}: {frame_path} against {spec['image']}: {fraction * 100:.2f} % of pixels over {spec.get('tolerance', 24)}/255 "
          f"(at most {spec.get('max_fraction', 0.01) * 100:.2f} %)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
