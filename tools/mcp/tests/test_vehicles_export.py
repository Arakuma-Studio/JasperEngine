"""S96 WP-A AC-32: a car in a built game. A prebuilt player loads the cooked Wheel Colliders, builds
the vehicle and drives it: a Lua marker line in the player's log after 2 s of motor torque says the
car moved forward with its wheels on the ground.

The run builds its prebuilt glcore runtime once: set JASPER_SLOW_BUILD=1 to run it.
"""

from __future__ import annotations

import asyncio
import os

import build_cache
from harness import Editor, skip

CI = True
EDITOR = {"create": True, "name": "Vehicles Export", "env": build_cache.player_env("glcore", "release")}
PREBUILT_PLAYERS = ["glcore-release"]

TIMEOUT = 1500

DRIVER = """local Driver = {}

function Driver:init()
    self.time = 0
    self.reported = false
    self.start = nil
    RegisterEngineEvent(self, "onFixedUpdate")
end

function Driver:onFixedUpdate()
    self.time = self.time + Time.fixedDeltaTime
    local car = GameObject.find("Car")
    if self.time > 0.5 and not self.start then
        self.start = car.position.z
    end
    for _, name in ipairs({"WheelRL", "WheelRR"}) do
        GameObject.find(name):getWheelCollider().motorTorque = self.start and 600 or 0
    end
    if self.start and self.time > 2.5 and not self.reported then
        self.reported = true
        local wheel = GameObject.find("WheelFL"):getWheelCollider()
        print(string.format("vehicle export moved %.2f grounded %s", car.position.z - self.start, tostring(wheel.isGrounded)))
    end
end

return Driver
"""

CORNERS = {"WheelFL": (-0.8, -0.25, 1.3), "WheelFR": (0.8, -0.25, 1.3), "WheelRL": (-0.8, -0.25, -1.3), "WheelRR": (0.8, -0.25, -1.3)}


async def test_prebuilt_player_drives_the_car(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (the run builds its prebuilt runtime once)")
    sid = (await ed.call("create_scene", name="Drive", type="3D", path="scenes/Drive"))["scene"]["id"]
    floor = (await ed.call("create_entity", scene=sid, name="Floor", type="cube", position=[0, -0.5, 0], scale=[200, 1, 200]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=floor, body_type="static", add=[{"shape": "box"}])
    car = (await ed.call("create_entity", scene=sid, name="Car", type="empty", position=[0, 1, 0]))["entity_id"]
    await ed.call("edit_colliders", scene=sid, entity=car, body_type="dynamic", add=[{"shape": "box", "size": [1.6, 0.5, 3.6], "fit": False}])
    for name, position in CORNERS.items():
        wheel = (await ed.call("create_entity", scene=sid, name=name, parent=car, position=list(position)))["entity_id"]
        await ed.call("add_component", scene=sid, entity=wheel, component="WheelCollider", values={"WheelCollider": {"radius": 0.35}})
    camera = (await ed.call("create_entity", scene=sid, name="View", type="camera", position=[0, 6, -12]))["entity_id"]
    await ed.call("set_properties", scene=sid, entity=camera, values={"camera": {"target": [0, 0, 0]}})
    await ed.call("create_script", kind="lua", name="Driver", content=DRIVER, scene=sid, entity=car)
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    scene_id = next(s["id"] for s in saved["scenes_in_build"] if s["name"] == "Drive")
    await ed.call("edit_build_scenes", action="set_start", scene=scene_id)
    await ed.call("set_project_settings", values={"graphics.runtime_shaders": False})

    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-vehicles"
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    job = r
    while job["status"] == "running":
        await asyncio.sleep(5.0)
        job = await ed.call("get_job", job_id=r["job_id"])
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")

    raw = await ed.call_raw("run_build", {"after_ready": True, "fixed_fps": 60, "quit_after": 240, "screenshot": False})
    ed.check(not raw.is_error, raw.text)
    ed.check(not raw.value["crashed"], f"the player crashed: {raw.value}")
    log = raw.value["log"]
    lines = log if isinstance(log, list) else str(log).splitlines()
    marker = next((line for line in lines if "vehicle export moved" in line), None)
    ed.check(marker is not None, f"the driver's marker line: {lines[-40:]}")
    moved = float(marker.split("moved ")[1].split(" ")[0])
    ed.check(moved > 1.0, f"the exported car drove forward: {marker}")
    ed.check("grounded true" in marker, f"its wheels on the ground: {marker}")
