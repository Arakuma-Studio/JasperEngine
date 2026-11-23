"""What a build ships (S98 REQ-8 to REQ-12, D3; AC9, AC10, AC11).

A project as a user creates it (its assets under assets/, Asset Stripping: Referenced Only) holds a
texture a sprite uses, one nothing uses, one a Lua string literal names, an FBX no scene uses, a
folder in Always Include and one in Exclude From Build, and a custom shader. Each test exports the
C++ source project (no compiler run) or plays, and checks what landed:
- Referenced Only ships what the build references, Lua literals and Always Include; the Build Report
  lists the rest with its reason, and build_project gives stripped_count / stripped_bytes;
- Ship All Assets ships everything but the excluded folder;
- Play start imports only the FBX files the build would ship (Ship All: every one);
- a custom shader source ships once, at its project-relative path.

A Development Build names the setting at a stripped asset's load (REQ-10, AC9's player part): a
Referenced Only project whose script loads a texture by a path it builds at runtime leaves the
texture out, and the player log says "was stripped from the build (Asset Stripping: Referenced
Only): add it to Always Include" (a real prebuilt player: JASPER_SLOW_BUILD=1).

A new project's layout (S98 REQ-36, AC32): its assets and its Lua scripts live in folders of their
own (assetsDir: assets, luaDir: scripts), and a project.yaml without the keys still means ".". The
harness keeps created projects flat for the other files (JASPER_TEST_FLAT_NEW_PROJECT=1,
docs/testing.md); this one creates them as a user gets them.
"""

from __future__ import annotations

import asyncio
import json
import os
import shutil
import struct
import tempfile
import time
import zlib
from pathlib import Path

import build_cache
import harness
from harness import Editor, skip

CI = True
SLOW_BUILD = os.environ.get("JASPER_SLOW_BUILD", "") not in ("", "0")
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200")) if SLOW_BUILD else 1800
# test_development_build_names_a_stripped_asset's player
PREBUILT_PLAYERS = ["glcore-development"]
# a new project's real defaults (the harness keeps created projects as they were before S98 otherwise)
EDITOR = {"create": True, "name": "Stripping Game", "env": {"JASPER_TEST_FLAT_NEW_PROJECT": "0"}}

FBX_FIXTURE = harness.REPO / "editor" / "tests" / "fixtures" / "rigged_cube.fbx"
S: dict = {}

LOADER = """local Loader = {}
function Loader:init()
  -- a literal path: Referenced Only keeps the file it names
  self.icon = Texture("textures/lua.png")
end
return Loader
"""


def png(seed: int) -> bytes:
    """A valid 1x1 RGBA PNG (the texture cook decodes it); seeds make the files differ."""
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    header = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes([0, seed, 0x80, 0, 0xFF]))) + chunk(b"IEND", b"")


async def source_export(ed: Editor, name: str) -> dict:
    out = ed.scratch.root / name
    job = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", backends=["opengl"],
                        overwrite=True, timeout_seconds=900)
    ed.check_eq(job["status"], "succeeded", f"{job.get('message')}")
    job["out"] = out
    return job


def cooked_textures(out: Path) -> set[str]:
    index = out / "project" / "assets" / "je-cooked" / "desktop" / "index.json"
    return set(json.loads(index.read_text())["textures"]) if index.is_file() else set()


def cooked_models(out: Path) -> set[str]:
    index = out / "project" / "assets" / "je-cooked" / "models" / "index.json"
    return set(json.loads(index.read_text())["models"]) if index.is_file() else set()


def report_of(job: dict) -> dict:
    return json.loads(Path(job["result"]["build_report"]).read_text())


async def test_new_project_strips(ed: Editor):
    """D3: a project made now ships what its build references"""
    got = (await ed.call("get_project_settings", keys=["player.asset_stripping"]))["settings"]
    ed.check_eq(got["player.asset_stripping"], "referenced_only", "a new project's Asset Stripping")
    text = (ed.project / "project.yaml").read_text()
    ed.check("assetStripping: referencedOnly" in text, f"project.yaml says so:\n{text}")


async def test_prepare(ed: Editor):
    if (await ed.call("get_project_settings", keys=["editor.assets_dir"]))["settings"]["editor.assets_dir"] != "assets":
        await ed.call("set_project_settings", values={"editor.assets_dir": "assets"})
    assets = ed.project / "assets"
    for name, seed in (("used", 1), ("unused", 2), ("lua", 3)):
        (assets / "textures").mkdir(parents=True, exist_ok=True)
        (assets / "textures" / f"{name}.png").write_bytes(png(seed))
    (assets / "always").mkdir(parents=True, exist_ok=True)
    (assets / "always" / "extra.png").write_bytes(png(4))
    (assets / "dev").mkdir(parents=True, exist_ok=True)
    (assets / "dev" / "debug.png").write_bytes(png(5))
    (assets / "models").mkdir(parents=True, exist_ok=True)
    (assets / "models" / "unused.fbx").write_bytes(FBX_FIXTURE.read_bytes())
    (assets / "shaders").mkdir(parents=True, exist_ok=True)
    (assets / "shaders" / "Plain.vert").write_text("void main() {}\n")
    (assets / "shaders" / "Plain.frag").write_text("void main() {}\n")
    (ed.project / "scripts").mkdir(parents=True, exist_ok=True)
    (ed.project / "scripts" / "Loader.lua").write_text(LOADER)
    await ed.call("sync_files")

    scene = await ed.call("create_scene", name="Main", type="2d", path="assets/scenes/Main", close_previous=True, unsaved="discard")
    S["scene"] = scene["scene"]["id"]
    await ed.call("create_entity", scene=S["scene"], type="sprite", name="Board", values={
        "sprite": {"width": 64, "height": 64},
        "mesh": {"submeshes[0].material.baseColorTexture": "assets/textures/used.png"},
    })
    await ed.call("save_scene", scene=S["scene"])
    await ed.call("edit_build_scenes", action="add", scene=S["scene"])
    await ed.call("edit_build_scenes", action="set_start", scene=S["scene"])
    await ed.call("set_project_settings", values={
        "graphics.runtime_shaders": False,
        "player.asset_stripping": "referenced_only",
        "player.always_include": ["assets/always/**"],
        "player.exclude_from_build": ["assets/dev/**"],
    })


async def drop_editor_import(ed: Editor, glb: Path) -> None:
    """The editor's own model cook (its previews, S53) imports a new FBX by itself: wait for that
    import, then delete it, so what a later step imports is that step's"""
    for _ in range(200):
        if glb.is_file():
            break
        await ed.call("wait", frames=5)
    await ed.call("wait", frames=10)
    glb.unlink(missing_ok=True)


async def test_play_imports_only_shipped_fbx(ed: Editor):
    """AC11: Referenced Only with an FBX nothing uses: Play start leaves it unimported; under Ship All
    Assets Play imports it, as before"""
    glb = ed.project / ".jasperengine" / "imported" / "models" / "unused.fbx.glb"
    await drop_editor_import(ed, glb)
    await ed.call("play", action="start", scene=S["scene"], timeout_seconds=120)
    await ed.call("play", action="stop")
    ed.check(not glb.exists(), f"Play imported the FBX the build would not ship: {glb}")
    await ed.call("set_project_settings", values={"player.asset_stripping": "ship_all"})
    try:
        await ed.call("play", action="start", scene=S["scene"], timeout_seconds=120)
        await ed.call("play", action="stop")
        ed.check(glb.is_file(), "under Ship All Assets Play imports every FBX")
    finally:
        await ed.call("set_project_settings", values={"player.asset_stripping": "referenced_only"})


async def test_referenced_only_ships_what_the_build_references(ed: Editor):
    """AC9: the sprite's texture, the Lua literal's and Always Include ship; the unused texture, the
    unused FBX and the excluded folder are listed with their reason"""
    await drop_editor_import(ed, ed.project / ".jasperengine" / "imported" / "models" / "unused.fbx.glb")
    job = await source_export(ed, "export-referenced")
    out = job["out"]
    textures = cooked_textures(out)
    for shipped in ("textures/used.png", "textures/lua.png", "always/extra.png"):
        ed.check_in(shipped, textures, "it ships (cooked)")
    for stripped in ("textures/unused.png", "dev/debug.png"):
        ed.check(stripped not in textures, f"{stripped} does not ship: {sorted(textures)}")
    ed.check("models/unused.fbx" not in cooked_models(out), f"the unused FBX does not ship: {sorted(cooked_models(out))}")
    ed.check(not (ed.project / ".jasperengine" / "imported" / "models" / "unused.fbx.glb").exists(),
             "the export did not import the FBX it leaves out (REQ-12)")

    report = report_of(job)
    left = {item["path"]: item["reason"] for item in report.get("stripped", [])}
    ed.check_eq(left, {
        "assets/textures/unused.png": "unreferenced",
        "assets/models/unused.fbx": "unreferenced",
        "assets/dev/debug.png": "excluded: assets/dev/**",
    }, "the Build Report lists what was left out and why")
    sizes = {item["path"]: item["bytes"] for item in report["stripped"]}
    ed.check_eq(sizes["assets/models/unused.fbx"], FBX_FIXTURE.stat().st_size, "with its size")
    result = job["result"]
    ed.check_eq(result["stripped_count"], 3, f"{result}")
    ed.check_eq(result["stripped_bytes"], sum(sizes.values()), f"{result}")
    # (structuredContent's own next, where a client shows it)
    ed.check_in("always_include", job.get("next", ""), "the result names the fix for a computed path")


async def test_ship_all_ships_all_but_the_excluded(ed: Editor):
    await ed.call("set_project_settings", values={"player.asset_stripping": "ship_all"})
    try:
        job = await source_export(ed, "export-ship-all")
    finally:
        await ed.call("set_project_settings", values={"player.asset_stripping": "referenced_only"})
    textures = cooked_textures(job["out"])
    for shipped in ("textures/used.png", "textures/unused.png", "textures/lua.png", "always/extra.png"):
        ed.check_in(shipped, textures, "Ship All Assets ships it")
    ed.check("dev/debug.png" not in textures, "Exclude From Build applies under Ship All Assets too")
    ed.check_in("models/unused.fbx", cooked_models(job["out"]), "every FBX ships under Ship All Assets")
    ed.check_eq(job["result"]["stripped_count"], 1, f"{job['result']}")


async def test_shader_shipped_once(ed: Editor):
    """AC10: with assetsDir: assets a custom shader ships once, at its project-relative path (where the
    runtime reads a fork's customShader), not also at its assets-relative one"""
    job = await source_export(ed, "export-shader")
    assets = job["out"] / "project" / "assets"
    copies = sorted(p.relative_to(assets).as_posix() for p in assets.rglob("Plain.frag"))
    ed.check_eq(copies, ["assets/shaders/Plain.frag"], "one copy of the shader source")


# ---- a Development Build names a stripped asset (REQ-10) ----

COMPUTED = """local Computed = {}
function Computed:init()
  -- a path built at runtime: no literal names the file, so Referenced Only leaves it out
  local name = "textures/" .. "hidden" .. ".png"
  self.texture = Texture(name)
  self.texture:load()
end
return Computed
"""


def hidden_png() -> bytes:
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
    if not SLOW_BUILD:
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Development Build player (the run builds its prebuilt runtime once)")
    async with Editor(create=True, name="Stripped Build Test", env=build_cache.player_env("glcore", "development")) as ed:
        runtime = await build_cache.player_runtime(ed, "glcore", "development")
        (ed.project / "textures").mkdir(parents=True, exist_ok=True)
        (ed.project / "textures" / "hidden.png").write_bytes(hidden_png())
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


# ---- a new project's layout (REQ-36) ----


async def layout_roots(ed: Editor) -> tuple[str, str]:
    got = (await ed.call("get_project_settings", keys=["editor.assets_dir", "scripting.lua_dir"]))["settings"]
    return got["editor.assets_dir"], got["scripting.lua_dir"]


async def test_new_project_layout(ed: Editor):
    ed.check_eq(await layout_roots(ed), ("assets", "scripts"), "the project uses them")
    text = (ed.project / "project.yaml").read_text()
    ed.check("assetsDir: assets" in text and "luaDir: scripts" in text, f"project.yaml names both folders:\n{text}")
    ed.check((ed.project / "assets").is_dir() and (ed.project / "scripts").is_dir(), "both folders exist")
    ed.check((ed.project / "assets" / "audio" / "Main.mixer").is_file(), "the new project's mixer is an asset under assets/")

    # an FBX under assets imports into the cache by its path under assets
    models = ed.project / "assets" / "models"
    models.mkdir(parents=True, exist_ok=True)
    shutil.copy(harness.REPO / "editor" / "tests" / "fixtures" / "rigged_cube.fbx", models / "cube.fbx")
    await ed.call("sync_files", paths=["assets/models/cube.fbx"])
    await ed.call("reimport_models", paths=["assets/models/cube.fbx"], timeout=600)
    imported = ed.project / ".jasperengine" / "imported" / "models" / "cube.fbx.glb"
    ed.check(imported.is_file(), f"the import lands at {imported.relative_to(ed.project)}: "
             f"{sorted(str(p.relative_to(ed.project)) for p in (ed.project / '.jasperengine').rglob('*.glb'))}")

    # a script made through MCP goes to the Lua folder
    await ed.call("create_script", kind="lua", name="Mover", directory="scripts")
    ed.check((ed.project / "scripts" / "Mover.lua").is_file(), "create_script writes under scripts/")


async def test_old_project_without_keys_stays_flat(ed: Editor):
    root = Path(tempfile.mkdtemp(prefix="jasper-old-layout-"))
    try:
        project = root / "project"
        shutil.copytree(harness.REPO / "tools" / "fixtures" / "tween-curve", project)
        yaml = project / "project.yaml"
        yaml.write_text("".join(line for line in yaml.read_text().splitlines(True)
                                if not line.startswith(("assetsDir:", "luaDir:"))))
        before = yaml.read_text()
        async with Editor(project=project, label="old-layout") as old:
            ed.check_eq(await layout_roots(old), (".", "."), "a project.yaml without the keys keeps the project root")
            ed.check(old.project and (old.project / "TweenCurveCheck.lua").is_file(), "its files are where they were")
            ed.check_eq((old.project / "project.yaml").read_text(), before, "opening it writes no new key")
    finally:
        shutil.rmtree(root, ignore_errors=True)
