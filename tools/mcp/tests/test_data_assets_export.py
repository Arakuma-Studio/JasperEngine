"""Data Assets in an exported game (docs/specs/S95-lua-runtime-gaps.md REQ-24, AC-21 "Export"): a
Desktop build with the run's prebuilt glcore player ships the .dataasset files and the index of the
data classes (je-dataassets.json), and the player, run headless, reads them: a script property typed
with the class, a list of them, DataAsset.loadAll and a reference by GUID to an asset moved after it
was referenced (the cook writes its current path).

Set JASPER_SLOW_BUILD=1 to run it: it uses the run's prebuilt glcore runtime (tools/mcp/build_cache.py).
"""

import asyncio
import os
import subprocess
import time

import build_cache
from harness import REPO, Editor, skip

PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Data Assets Export", "env": build_cache.player_env("glcore", "release")}
CI = True

TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))

ITEM = """---@dataasset
local ItemData = { properties = {
    { name = "displayName", type = "string", default = "Item" },
    { name = "damage", type = "int", default = 5 },
    { name = "upgrade", type = "ItemData" },
} }
function ItemData:describe() return self.displayName .. ":" .. self.damage end
return ItemData
"""

SHOP = """local Shop = { properties = {
    { name = "featured", type = "ItemData" },
    { name = "stock", type = "list", of = "ItemData" },
} }
function Shop:start()
    local all = DataAsset.loadAll("ItemData")
    local moved = DataAsset.load("MOVED_PATH")
    print("datamarker " .. self.featured:describe() .. " " .. #self.stock .. " " .. #all .. " " ..
          self.featured.upgrade:describe() .. " " .. tostring(rawequal(self.featured.upgrade, moved)))
end
return Shop
"""


async def wait_job(ed: Editor, job: dict, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while job.get("status") == "running":
        if time.monotonic() > deadline:
            ed.fail(f"job {job['job_id']} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)
        job = await ed.call("get_job", job_id=job["job_id"], log_lines=10)
    return job


async def test_desktop_build_reads_data_assets(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (the run builds its prebuilt runtime once)")
    await ed.call("create_script", kind="lua", name="ItemData", content=ITEM)
    axe = await ed.call("edit_data_asset", action="create", path="assets/items/axe.dataasset", **{"class": "ItemData"},
                        values={"displayName": "Axe", "damage": 7})
    sword = await ed.call("edit_data_asset", action="create", path="assets/items/sword.dataasset", **{"class": "ItemData"},
                          values={"displayName": "Sword", "damage": 12, "upgrade": axe["guid"]})
    # moved after the sword referenced it: the GUID finds it, the cook writes its new path
    await ed.call("create_folder", path="assets/items/moved")
    await ed.call("move_asset", path="assets/items/axe.dataasset", to="assets/items/moved/axe.dataasset")

    made = await ed.call("create_scene", name="DataExport", type="3D", path="scenes/DataExport")
    scene = made["scene"]["id"]
    shop = (await ed.call("create_entity", scene=scene, type="empty", name="Shop"))["entity_id"]
    moved_path = (await ed.call("edit_data_asset", action="get", path="assets/items/moved/axe.dataasset"))["path"]
    # Lua loads by the path relative to the assets folder, the project's root here
    await ed.call("create_script", kind="lua", name="Shop", content=SHOP.replace("MOVED_PATH", moved_path), entity=shop, scene=scene)
    await ed.call("set_script_properties", scene=scene, entity=shop, values={"featured": sword["guid"], "stock": [sword["guid"], axe["guid"]]})
    saved = await ed.call("save_scene", scene=scene)
    await ed.call("edit_build_scenes", action="add", scene=saved["scene"]["path"])
    await ed.call("edit_build_scenes", action="set_start", scene=saved["scene"]["path"])

    runtime = await build_cache.player_runtime(ed, "glcore", "release")
    out = ed.scratch.root / "export-data-assets"
    job = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
    job = await wait_job(ed, job, TIMEOUT - 600)
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")

    run = await asyncio.to_thread(
        subprocess.run, [str(REPO / "tools" / "with-xvfb.sh"), job["result"]["executable"], "--headless", "--fixed-fps", "60",
                         "--after-ready", "--quit-after", "30"],
        cwd=out, capture_output=True, text=True, timeout=900)
    log = run.stdout + run.stderr
    (ed.scratch.root / "data-assets-player.log").write_text(log)
    ed.check_eq(run.returncode, 0, f"the player quits after its frames: {log[-2000:]}")
    ed.check("datamarker Sword:12 2 2 Axe:7 true" in log, f"the player reads the shipped Data Assets: {log[-2000:]}")
