"""Lua's Noise and NoiseGraph in Play (T2 R67, R68; AC45)."""

import json
from pathlib import Path

from harness import Editor

CI = True
EDITOR = {"create": True, "name": "Noise Lua Test"}
TIMEOUT = 600

SCRIPT = """
local NoiseLua = {}
local function report(line) print("[noise-lua] " .. line) end

function NoiseLua:init()
    RegisterEngineEvent(self, "onUpdate")
end

function NoiseLua:start()
    report("perlin " .. string.format("%.6f", Noise.perlin(1.5, 2.5, 7)))
    report("perlin3 " .. string.format("%.6f", Noise.perlin3(1.5, 0.25, 2.5, 7)))
    report("cellular " .. string.format("%.6f", Noise.cellular(1.5, 2.5, 7, "f2_minus_f1")))
    report("fbm " .. string.format("%.6f", Noise.fbm(1.5, 2.5, 7, 6, 2.0, 0.5, "perlin")))
    local graph = NoiseGraph.load("graphs/Plains.noisegraph")
    report("loaded " .. tostring(graph ~= nil))
    local grid = graph:sampleGrid{x = 0, z = 0, width = 64, depth = 64, step = 1 / 64, seed = 7}
    report("grid " .. grid.hash)
    local low, high = graph:rangeOf("height")
    report("range " .. tostring(low <= grid.min and grid.max <= high))
    local bad = NoiseGraph.load("graphs/Alpine.noisegraph")
    report("bake_only " .. tostring(bad == nil))
    local slope = NoiseGraph.load("graphs/Mountains.noisegraph")
    local ok, message = pcall(function() return slope:sample(0.5, 0.5, 1, "layer:1") end)
    report("grid_only " .. tostring(not ok) .. " " .. tostring(message))
    self.task = graph:sampleGridAsync{x = 0, z = 0, width = 64, depth = 64, step = 1 / 64, seed = 7}
end

function NoiseLua:onUpdate()
    if self.task then
        report("progress " .. string.format("%.3f", self.task.progress))
    end
    if self.task and self.task.done then
        report("async " .. self.task:result().hash)
        self.task = nil
    end
end

return NoiseLua
"""


async def test_noise_lua_in_play(ed: Editor):
    root = Path(ed.project)
    graphs = root / "graphs"   # a created project's assets are its root: a game reads "graphs/..." from there
    graphs.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[3] / "editor" / "resources" / "noisegraphs"
    for name, preset in (("Plains", "plains"), ("Alpine", "alpine"), ("Mountains", "mountains")):
        (graphs / f"{name}.noisegraph").write_text((repo / f"{preset}.noisegraph").read_text())
    made = await ed.call("create_scene", name="NoiseLua", type="3D", path="scenes/NoiseLua")
    sid = made["scene"] if "scene" in made else made["scene_id"]
    holder = (await ed.call("create_entity", scene=sid, name="Holder", type="empty"))["entity_id"]
    await ed.call("create_script", kind="lua", name="NoiseLua", content=SCRIPT, scene=sid, entity=holder)
    preview = await ed.call("preview_noise_graph", preset="plains", size=64, seed=7, images="none")
    # A game's sampleGridAsync runs on Background, as it must on a web player's pump (R68). In the editor that lane
    # is still full of its own start-up shader compiles on this host (one gate slot): wait them out, as no player
    # has them, so the run measures the game's work.
    for _ in range(600):
        lanes = (await ed.call("get_job_system_stats"))["lanes"]["Background"]
        if lanes["queued"] == 0 and lanes["running"] == 0:
            break
        await ed.call("wait", seconds=0.2)
    await ed.call("play", action="start", scene=sid)
    lines = {}
    errors = []
    try:
        for _ in range(600):
            await ed.call("wait", seconds=0.05)
            for entry in await ed.console_since("last"):
                text = entry.get("message", "") if isinstance(entry, dict) else str(entry)
                if isinstance(entry, dict) and entry.get("level") == "error" and len(errors) < 5:
                    errors.append(text[:300])
                if "[noise-lua] " in text:
                    key, _, value = text.split("[noise-lua] ", 1)[1].partition(" ")
                    lines[key] = value
            if "async" in lines:
                break
    finally:
        await ed.call("play", action="stop")
    ed.check_eq(lines.get("loaded"), "true", json.dumps(lines))
    for key in ("perlin", "perlin3", "cellular", "fbm"):
        ed.check(key in lines, f"{key}: {lines}")
    ed.check_eq(lines.get("range"), "true", "the grid is within the range")
    ed.check_eq(lines.get("bake_only"), "true", "a bake-only graph loads as nil")
    ed.check(lines.get("grid_only", "").startswith("true") and "n" in lines["grid_only"], f"sample on a grid-only graph names the node: {lines.get('grid_only')}")
    # Same bits as the tool and the golden table's grid
    ed.check_eq(lines.get("grid"), preview["outputs"]["height"], "Lua's grid is the tool's")
    ed.check_eq(lines.get("async"), lines.get("grid"), f"the async grid has the same bits (errors in Play: {errors}; last progress {lines.get('progress')})")
