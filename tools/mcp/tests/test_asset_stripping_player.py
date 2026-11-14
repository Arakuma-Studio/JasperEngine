"""A Development Build names the setting at a stripped asset's load (S98 REQ-10, AC9's player part).

A Referenced Only project whose script loads a texture by a path it builds at runtime: the build
leaves the texture out, and the Development Build's player log says "was stripped from the build
(Asset Stripping: Referenced Only): add it to Always Include" when the script loads it. Needs a
real prebuilt player (JASPER_SLOW_BUILD=1; the run builds the development runtime once).
"""

from __future__ import annotations

import asyncio
import os
import struct
import time
import zlib

import build_cache
from harness import Editor, skip

CI = True
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200")) if os.environ.get("JASPER_SLOW_BUILD", "") not in ("", "0") else 900
PREBUILT_PLAYERS = ["glcore-development"]

COMPUTED = """local Computed = {}
function Computed:init()
  -- a path built at runtime: no literal names the file, so Referenced Only leaves it out
  local name = "textures/" .. "hidden" .. ".png"
  self.texture = Texture(name)
  self.texture:load()
end
return Computed
"""


def png() -> bytes:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    header = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(b"\x00\xff\x80\x00\xff")) + chunk(b"IEND", b"")


async def wait_job(ed: Editor, job_id: str, timeout: float, poll: float = 5.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)


async def test_development_build_names_a_stripped_asset():
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Development Build player (the run builds its prebuilt runtime once)")
    async with Editor(create=True, name="Stripped Build Test", env=build_cache.player_env("glcore", "development")) as ed:
        runtime = await build_cache.player_runtime(ed, "glcore", "development")
        (ed.project / "textures").mkdir(parents=True, exist_ok=True)
        (ed.project / "textures" / "hidden.png").write_bytes(png())
        await ed.call("sync_files")
        made = await ed.call("create_entity", name="Loader", type="box")
        await ed.call("create_script", kind="lua", name="Computed", directory="scripts", content=COMPUTED, entity=made["entity_id"])
        s = await ed.call("save_project", untitled_scene_dir="scenes")
        await ed.call("edit_build_scenes", action="set_start", scene=s["scenes_in_build"][0]["id"])
        await ed.call("set_project_settings", values={
            "graphics.runtime_shaders": False,
            "player.development_build": True,
            "player.asset_stripping": "referenced_only",
        })
        out = ed.scratch.root / "export-stripped-dev"
        r = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
        job = r if r["status"] != "running" else await wait_job(ed, r["job_id"], TIMEOUT - 300)
        ed.check_eq(job["status"], "succeeded", f"{job}")
        ed.check_eq((job["result"]["method_used"], job["result"]["player_id"]), ("prebuilt", runtime.name), f"{job}")
        ed.check(job["result"]["stripped_count"] >= 1, f"the texture was left out: {job['result']}")
        raw = await ed.call_raw("run_build", {"after_ready": True, "fixed_fps": 60, "quit_after": 30, "screenshot": False})
        ed.check(not raw.is_error, raw.text)
        log_text = "\n".join(raw.value["log"]) if isinstance(raw.value["log"], list) else str(raw.value["log"])
        ed.check_in("textures/hidden.png was stripped from the build (Asset Stripping: Referenced Only): add it to Always Include",
                    log_text, "the Development Build names the setting")
