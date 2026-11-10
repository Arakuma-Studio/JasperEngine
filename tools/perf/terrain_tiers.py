#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""T3 AC45: what each terrain tier costs against High on the same scene, by S15's timers, 5 runs, median.

Prints a JSON report: per fixture and tier the terrain pass's GPU milliseconds and its ratio to High. The
High run is the control. llvmpipe's balance of fetches and ALU is not a phone's: the ratios are a guard, and
the absolute budgets are manual (docs/specs/checklists/T3.md).

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/perf/terrain_tiers.py [--runs 5] [--out report.json]
"""

import argparse
import asyncio
import json
import pathlib
import statistics
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "mcp"))

from harness import Editor  # noqa: E402

BUDGET = {
    "Terrain Baseline": {"medium": 0.9, "low": 0.6},
    "Terrain Nine": {"medium": 0.5, "low": 0.3},
}


async def terrain_pass_ms(ed, frames=30):
    report = await ed.call("get_profiler_frames", frames=frames)
    values = [pass_["ms"] for frame in report["frames"] for pass_ in frame.get("gpu", []) if "Terrain" in pass_.get("name", "") or pass_.get("name") == "Color"]
    return statistics.median(values) if values else None


async def run(args):
    report = {}
    async with Editor(create=True, name="TerrainTiers") as ed:
        for fixture, layers in (("Terrain Baseline", 4), ("Terrain Nine", 9)):
            scene = (await ed.call("create_scene", name=fixture))["id"]
            made = await ed.call("create_entity", scene=scene, type="terrain", name="T", select=False)
            entity = made["entity_id"]
            report[fixture] = {}
            for tier in ("high", "medium", "low"):
                await ed.call("set_properties", scene=scene, entity=entity, values={"terrain": {"quality": tier.upper()}})
                await ed.call("screenshot", view="game")
                runs = [await terrain_pass_ms(ed) for _ in range(args.runs)]
                report[fixture][tier] = {"ms": statistics.median(r for r in runs if r is not None) if any(r is not None for r in runs) else None, "layers": layers}
            high = report[fixture]["high"]["ms"]
            for tier in ("medium", "low"):
                ms = report[fixture][tier]["ms"]
                report[fixture][tier]["ratio"] = ms / high if ms and high else None
                report[fixture][tier]["budget"] = BUDGET[fixture][tier]
    text = json.dumps(report, indent=2)
    if args.out:
        pathlib.Path(args.out).write_text(text + "\n")
    print(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--out")
    asyncio.run(run(parser.parse_args()))


if __name__ == "__main__":
    main()
