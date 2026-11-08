#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The Title Designer benchmark (docs/specs/S82-title-designer.md R68, R69, AC31).

    uv run --no-project --with-requirements tools/mcp/requirements.txt \\
        python tools/perf/title_designer_bench.py [--json out.json] [--check] [--label host]

It drives headless editors through MCP (tools/mcp/harness.py; JASPER_EDITOR picks the editor, as
for the MCP tests) and builds its reference documents with edit_title ops, so it needs no fixture:

- bench_2d_rims: one text layer with 6 rims and 8 effects;
- bench_40_layers: 40 layers (texts and shapes, each with an effect);
- bench_256_layers: the document limit, 256 layers.

Measured (the window's own numbers, focus_window `state.canvas` and `state.timings`):

- Draft vs Full: the canvas render time of each reference document in an editor whose Title
  Designer quality is Full, and in one whose quality is Draft, at the same views (several zooms,
  so S80's node cache does not answer); R68 wants Draft <= 1/4 of Full;
- the window's ImGui work per frame (headless, rendering excluded) with bench_40_layers open,
  Layers and Properties shown: <= 1.5 ms;
- the search index of the selected bench_2d_rims text layer: built in <= 3 ms, a query in <= 1 ms;
- the Layers tree of bench_256_layers against bench_40_layers: within 10 %.

The numbers are of the machine it runs on: --label names it in the result (e.g. "host"). --check
exits 1 when a budget is missed; the spec records misses in its Deviations with the numbers.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import statistics
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "mcp"))

from harness import Editor  # noqa: E402

BUDGETS = {"draft_over_full": 0.25, "frame_ms": 1.5, "index_ms": 3.0, "query_ms": 1.0, "tree_256_over_40": 1.10}
ZOOMS = [1.0, 1.05, 1.1, 1.15, 1.2]
EFFECTS = ["outer_glow", "drop_shadow", "inner_glow", "inner_shadow", "gradient_overlay", "outline", "color_overlay", "outer_glow"]


async def make_documents(ed: Editor) -> dict[str, str]:
    docs = {"rims": "assets/bench/bench_2d_rims.titleart", "forty": "assets/bench/bench_40_layers.titleart",
            "many": "assets/bench/bench_256_layers.titleart"}
    await ed.call("tool_groups", enable=["title_art", "editor_ux", "profiling"])
    await ed.call("set_project_settings", values={"title_art.auto_bake": False})

    await ed.call("edit_title", path=docs["rims"], create={"text": "BENCH"}, bake="never")
    ops = [{"op": "set", "layer": "Title", "values": {"size": 220}}]
    for i in range(6):
        ops.append({"op": "add_rim", "layer": "Title", "values": {"width": 3 + i, "material.fill.color": f"#{(i * 40) % 256:02x}4080"}})
    for effect in EFFECTS:
        ops.append({"op": "add_effect", "layer": "Title", "type": effect})
    await ed.call("edit_title", path=docs["rims"], ops=ops, bake="never")

    for key, count in (("forty", 40), ("many", 256)):
        await ed.call("edit_title", path=docs[key], create={"text": "L0"}, bake="never")
        ops = []
        for i in range(1, count):
            x, y = (i % 8) * 200 - 700, (i // 8) % 5 * 180 - 360
            if i % 2:
                layer = {"type": "text", "name": f"Text {i}", "text": f"L{i}", "size": 90, "transform": {"position": [x, y]},
                         "effects": [{"type": EFFECTS[i % len(EFFECTS)]}]}
            else:
                layer = {"type": "shape", "name": f"Shape {i}", "transform": {"position": [x, y]}, "effects": [{"type": "drop_shadow"}]}
            ops.append({"op": "add_layer", "from": layer})
        await ed.call("edit_title", path=docs[key], ops=ops, bake="never")
    return docs


async def state(ed: Editor, **keys) -> dict:
    return (await ed.call("focus_window", window="title_designer", title_designer=keys))["state"]


async def settled(ed: Editor, timeout: float = 900.0) -> dict:
    """The state once the canvas shows a finished render of the current view."""
    waited = 0.0
    while waited < timeout:
        s = await state(ed)
        canvas = s.get("canvas", {})
        if not s.get("busy") and canvas.get("has_image"):
            return s
        await asyncio.sleep(0.5)
        waited += 0.5
    raise RuntimeError(f"the canvas did not settle in {timeout} s: {s}")


async def render_times(ed: Editor, path: str) -> list[float]:
    """The canvas render time at each of ZOOMS (the first view warms the shaders up)."""
    await state(ed, path=path, select="", panel="properties", search="", zoom="fit", background="black")
    await settled(ed)
    times = []
    for zoom in ZOOMS:
        await state(ed, zoom=zoom)
        await asyncio.sleep(0.3)
        s = await settled(ed)
        times.append(float(s["last_render_ms"]))
    return times


async def frame_ms(ed: Editor, frames: int = 150) -> dict:
    """The window's own work per frame, idle, over `frames` frames (each call runs between frames)."""
    for _ in range(frames):
        await ed.call("editor_status")
    t = (await state(ed))["timings"]
    return {"avg": t["frame_ms_avg"], "max": t["frame_ms_max"], "frames": t["frames"]}


async def scopes(ed: Editor) -> list:
    """The window's profiler scopes ("TD ...") of the median frame of a short capture."""
    capture = await ed.call("capture_profile", frames=60, gpu=False)
    rows = await ed.call("get_cpu_profile", capture_id=capture.get("capture_id"), view="flat", filter="TD ", frame="median", max_rows=120)
    return rows.get("rows", rows)


async def ui_numbers(ed: Editor, docs: dict[str, str], profile: bool) -> dict:
    out: dict = {}
    # ImGui work with the 40-layer document, Layers then Properties shown
    await state(ed, path=docs["forty"], select="Text 1", panel="layers", search="", zoom="fit")
    await settled(ed)
    out["frame_layers_40"] = await frame_ms(ed)
    if profile:
        out["scopes_layers_40"] = await scopes(ed)
    await state(ed, panel="properties")
    out["frame_properties_40"] = await frame_ms(ed)
    if profile:
        out["scopes_properties_40"] = await scopes(ed)
    # the 256-layer tree against the 40-layer one, Layers shown
    await state(ed, path=docs["many"], select="Text 1", panel="layers", search="", zoom="fit")
    await settled(ed)
    out["frame_layers_256"] = await frame_ms(ed)
    if profile:
        out["scopes_layers_256"] = await scopes(ed)
        await state(ed, panel="properties")
        out["scopes_properties_256"] = await scopes(ed)
    # the search index of the 6-rim, 8-effect text layer, then a query
    await state(ed, path=docs["rims"], select="Title", panel="properties", search="", zoom="fit")
    await settled(ed)
    index, query = [], []
    for word in ("glow", "shadow", "rim width", "outline", "fill color"):
        await state(ed, search="")
        await ed.call("edit_title", path=docs["rims"], ops=[{"op": "set", "layer": "Title", "values": {"size": 220 + len(index)}}], bake="never")
        t = (await state(ed, panel="properties", search=word))["timings"]
        # the results draw (and the index builds) on the next frames
        for _ in range(40):
            await asyncio.sleep(0.25)
            t = (await state(ed))["timings"]
            if t["index_entries"] > 0 and t["query_ms"] > 0.0:
                break
        index.append(t["index_ms"])
        query.append(t["query_ms"])
    out["index_ms"] = index
    out["query_ms"] = query
    out["index_entries"] = t["index_entries"]
    out["window_rect"] = (await state(ed)).get("rect")
    return out


async def run_quality(quality: str, ui: bool, profile: bool = False, renders: bool = True) -> dict:
    settings = {"title_designer": {"open": True, "quality": quality}}
    async with Editor(create=True, name=f"TitleBench{quality.title()}", settings=settings, call_timeout=1200.0) as ed:
        docs = await make_documents(ed)
        result = {"quality": quality, "render_ms": {}}
        for key in ("rims", "forty") if renders else ():
            result["render_ms"][key] = await render_times(ed, docs[key])
        if ui:
            result["ui"] = await ui_numbers(ed, docs, profile)
        return result


def summarise(full: dict, draft: dict, label: str) -> dict:
    out = {"bench": "title-designer", "label": label, "budgets": BUDGETS, "full": full, "draft": draft, "results": {}, "missed": []}
    r = out["results"]
    for key in ("rims", "forty"):
        f, d = statistics.median(full["render_ms"][key]), statistics.median(draft["render_ms"][key])
        r[f"draft_over_full_{key}"] = d / f if f > 0 else None
        r[f"full_ms_{key}"] = f
        r[f"draft_ms_{key}"] = d
    ui = full["ui"]
    r["frame_ms"] = max(ui["frame_layers_40"]["avg"], ui["frame_properties_40"]["avg"])
    r["index_ms"] = statistics.median(ui["index_ms"])
    r["query_ms"] = statistics.median(ui["query_ms"])
    r["tree_256_over_40"] = ui["frame_layers_256"]["avg"] / ui["frame_layers_40"]["avg"] if ui["frame_layers_40"]["avg"] > 0 else None
    checks = [("draft_over_full_rims", "draft_over_full"), ("draft_over_full_forty", "draft_over_full"), ("frame_ms", "frame_ms"),
              ("index_ms", "index_ms"), ("query_ms", "query_ms"), ("tree_256_over_40", "tree_256_over_40")]
    for name, budget in checks:
        value = r[name]
        if value is None or value > BUDGETS[budget]:
            out["missed"].append({"measure": name, "value": value, "budget": BUDGETS[budget]})
    return out


async def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", help="write the result here")
    parser.add_argument("--check", action="store_true", help="exit 1 when a budget is missed")
    parser.add_argument("--label", default="host", help="the machine the numbers are of")
    parser.add_argument("--profile", action="store_true", help="also record the window's profiler scopes (TD ...) per scenario")
    parser.add_argument("--ui-only", action="store_true", help="skip the Draft vs Full renders")
    args = parser.parse_args()
    if args.ui_only:
        ui = await run_quality("draft", ui=True, profile=args.profile, renders=False)
        text = json.dumps(ui, indent=2)
        if args.json:
            Path(args.json).write_text(text + "\n")
        print(text[:20000])
        return 0
    full = await run_quality("full", ui=True, profile=args.profile)
    draft = await run_quality("draft", ui=False)
    result = summarise(full, draft, args.label)
    text = json.dumps(result, indent=2)
    if args.json:
        Path(args.json).write_text(text + "\n")
    print(json.dumps(result["results"], indent=2))
    for miss in result["missed"]:
        print(f"MISSED {miss['measure']}: {miss['value']} (budget {miss['budget']})")
    if not result["missed"]:
        print("every budget met")
    return 1 if args.check and result["missed"] else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
