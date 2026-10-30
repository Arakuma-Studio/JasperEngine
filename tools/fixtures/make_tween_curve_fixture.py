#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Writes tools/fixtures/tween-curve (docs/specs/S27-editor-tools.md AC7), the smoke test's project
of a Tween eased along a curve: an editor-made project (driven over MCP, tools/mcp/harness.py) whose
scene TweenCurve moves a cube with a Position Tween, Ease = Curve (the bump 0 -> 1 -> 0, Clamped
Auto keys: the curve of tools/mcp/tests/test_curves.py and tools/test-projects/tween-curve), and a
Lua script on the tween that checks every frame that the tween's value is its curve at its time
and the cube at 10 x that value, logging five samples. smoke-expect.txt lists the lines the
exported game must log (tools/smoke-test.sh), so the curve survives the export (the Factory's C++
literal) and evaluates as in the editor's Play:

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/fixtures/make_tween_curve_fixture.py
    tools/smoke-test.sh tools/fixtures/tween-curve
"""

from __future__ import annotations

import asyncio
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "mcp"))

from harness import Editor  # noqa: E402

BUMP = [[0, 0, 0, 0, "clampedAuto"], [0.5, 1, 0, 0, "clampedAuto"], [1, 0, 0, 0, "clampedAuto"]]

CHECKER = """---@class TweenCurveCheck : ScriptInstance
-- On the Position Tween: its value must be its curve at its time, and the target at 10 x the value
local TweenCurveCheck = { properties = {} }

local SAMPLES = { 0.1, 0.3, 0.5, 0.7, 0.9 }

function TweenCurveCheck:init()
    self.tween = PositionTween(self.scene, self.entity)
    self.target = GameObject(self.scene, self.tween.target)
    self.next = 1
    self.failed = false
    local peak = self.tween.curve:getKey(2)
    if self.tween.curve.length ~= 3 or peak == nil or peak.time ~= 0.5 or peak.value ~= 1 then
        Log.error("[gate] tween_curve: the exported curve is not the bump")
        self.failed = true
    end
    RegisterEngineEvent(self, "onUpdate")
end

function TweenCurveCheck:onUpdate()
    if self.failed or self.next > #SAMPLES then return end
    local state = self.tween:getTweenComponent()
    local expected = self.tween.curve:evaluate(state.time)
    local x = self.target.position.x
    if math.abs(state.value - expected) > 1e-5 or math.abs(x - 10 * state.value) > 1e-3 then
        Log.error("[gate] tween_curve mismatch at time " .. state.time .. ": value " .. state.value ..
                  ", curve " .. expected .. ", x " .. x)
        self.failed = true
        return
    end
    while self.next <= #SAMPLES and state.time >= SAMPLES[self.next] do
        Log.print("[gate] tween_curve sample " .. self.next .. " ok")
        self.next = self.next + 1
    end
    if self.next > #SAMPLES then
        Log.print("[gate] tween_curve 5 samples: value = curve(time), x = 10 value")
    end
end

return TweenCurveCheck
"""

EXPECT = """# tools/smoke-test.sh: lines the exported game logs (tools/fixtures/make_tween_curve_fixture.py)
[gate] tween_curve sample 1 ok
[gate] tween_curve sample 5 ok
[gate] tween_curve 5 samples: value = curve(time), x = 10 value
"""


async def main() -> int:
    target = REPO / "tools" / "fixtures" / "tween-curve"
    async with Editor(create=True, name="Tween Curve", label="fixture-tween-curve") as ed:
        made = await ed.call("create_scene", name="TweenCurve", type="3D", path="TweenCurve", close_previous=True, unsaved="discard")
        sid = made["scene"]["id"]
        await ed.call("edit_build_scenes", action="set_start", scene=sid)
        cube = (await ed.call("create_entity", scene=sid, type="cube", name="Mover"))["entity_id"]
        anim = await ed.call("edit_animation", scene=sid, target=cube, autoplay=True,
                             tweens=[{"type": "position", "from": [0, 0, 0], "to": [10, 0, 0], "duration": 1, "ease": "linear"}])
        tween = anim["actions"][0]["action"]["id"]
        await ed.call("set_properties", scene=sid, entity=tween, values={"tween": {"function": "curve", "curve": BUMP}})
        await ed.call("create_script", kind="lua", name="TweenCurveCheck", content=CHECKER, scene=sid, entity=tween)
        await ed.call("save_project")
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(ed.project, target, ignore=shutil.ignore_patterns(".jasperengine", "build"))
    (target / "smoke-expect.txt").write_text(EXPECT)
    print(f"[fixture] wrote {target}: {sorted(str(p.relative_to(target)) for p in target.rglob('*') if p.is_file())}")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
