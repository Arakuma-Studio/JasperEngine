"""S27 Project Hub for agents (docs/specs/S27-editor-tools.md AC16-AC18; jasper://docs/editor-tools):

- list_project_templates lists the four built-in templates;
- create_project {template} makes each into a project that loads without errors, opens Main,
  exports (mode source, Linux) without errors and runs its script in Play;
- import_project takes a zip holding a project one folder down and refuses one with a `..`
  entry, leaving nothing behind;
- the hub's start-up conditions: the editor's GUI (not `mcp`, which never shows the hub) started
  with --hub, with a missing last project, and with Show Project Hub at Startup = Always shows
  the hub (the project tools then answer "No project is open"), and `--project` never does.

The full Desktop build of a template project compiles the engine (minutes): JASPER_SLOW_BUILD=1
runs it for the 3D template.
"""

import asyncio
import contextlib
import io
import json
import os
import subprocess
import tempfile
import time
import zipfile
from pathlib import Path

import harness
import yaml
from harness import Editor, free_port, wait_for_port

CI = True
# the 3D template's Desktop compile: a fixed compile-tree path and ccache (harness.compile_env)
EDITOR = {"create": True, "name": "Hub", "env": harness.compile_env("test_hub")}
TIMEOUT = 1800

TEMPLATES = ("empty", "3d", "2d", "2d-pixel-art", "ui")
# what each template's script leaves to read in Play
SCRIPT_PROBES = {
    "3d": "return GameObject(scene, entity('Cube')).rotation ~= nil",
    "2d": "return GameObject(scene, entity('Player')).position ~= nil",
    "2d-pixel-art": "return GameObject(scene, entity('Player')).position ~= nil",
}


async def test_list_templates(ed: Editor):
    got = await ed.call("list_project_templates")
    ids = [t["id"] for t in got["templates"]]
    ed.check_eq(ids[:5], list(TEMPLATES), f"the built-in templates first, in order: {ids}")
    for t in got["templates"][:5]:
        ed.check(t["built_in"] is True and t["name"] and t["description"], f"{t}")
        ed.check_eq(t["start_scene"], "Main")


async def test_create_from_templates(ed: Editor):
    root = ed.scratch.root / "from-templates"
    for template in TEMPLATES:
        target = root / f"Game-{template}"
        made = await ed.call("create_project", path=str(target), name=f"Game {template}", template=template, unsaved="discard")
        ed.check_eq(made["template"], template)
        ed.check((target / "project.yaml").is_file(), f"{template}: project.yaml")
        ed.check(not (target / "template.json").exists() and not (target / "thumbnail.png").exists(),
                 f"{template}: the template's own files stay out")
        status = await ed.call("editor_status")
        ed.check_eq(Path(status["project"]["path"]).resolve(), target.resolve())
        ed.check_eq(status["project"]["name"], f"Game {template}")
        tree = await ed.call("get_scene_tree", format="json")
        ed.check_eq(tree["scene_name"], "Main", f"{template} opens Main")
        console = await ed.call("read_console", levels=["error"])
        errors = [e for e in console.get("entries", []) if e.get("level") == "error"]
        ed.check(not errors, f"{template} loads without errors: {errors[:5]}")
        project = yaml.safe_load((target / "project.yaml").read_text())
        # a template with no audio node of its own starts as any new project (S38 R8)
        ed.check_eq(project.get("audio", {}).get("outputStage"), "linear", f"{template}: the new-project audio")
        mixer = project["audio"]["mixer"]
        ed.check((target / project.get("assetsDir", ".") / mixer).is_file(), f"{template}: {mixer}")
        ed.check(project.get("versionControl", {}).get("lfs", False) is True or "lfs: true" in (target / "project.yaml").read_text(),
                 f"{template}: a new project gets Git LFS (S29 R21)")
        # mesh detail levels are on in every new project, with the cross-fade (S54 R1, AC9)
        lod_keys = ["mesh_lod_enabled", "mesh_lod_cross_fade", "mesh_lod_cross_fade_duration"]
        lod = (await ed.call("get_scene_settings", names=lod_keys))["settings"]
        ed.check_eq([lod["mesh_lod_enabled"], lod["mesh_lod_cross_fade"]], [True, True], f"{template}: LOD and Cross Fade on")
        ed.check(abs(lod["mesh_lod_cross_fade_duration"] - 0.5) < 1e-6, f"{template}: Cross-fade Duration 0.5 s: {lod}")
        if template == "3d":
            await ed.call("set_scene_settings", settings={"mesh_lod_cross_fade": False, "mesh_lod_cross_fade_duration": 1.25})
            changed = (await ed.call("get_scene_settings", names=lod_keys))["settings"]
            ed.check_eq(changed["mesh_lod_cross_fade"], False)
            ed.check(abs(changed["mesh_lod_cross_fade_duration"] - 1.25) < 1e-6, f"{changed}")
            await ed.call("undo")
            undone = (await ed.call("get_scene_settings", names=lod_keys))["settings"]
            ed.check_eq([undone["mesh_lod_cross_fade"], undone["mesh_lod_cross_fade_duration"]], [True, 0.5],
                        "one undo step restores both settings")
            biases = (await ed.call("get_project_settings", keys=["quality.lod_bias", "quality.shadow_lod_bias"]))["settings"]
            ed.check_eq([biases["quality.lod_bias"], biases["quality.shadow_lod_bias"]], [1.0, 1.0], "the LOD biases' defaults")
        names = [n["name"] for n in tree["nodes"]]
        if template == "empty":
            ed.check("Directional Light" not in names and "Skybox" not in names, f"empty: only a camera: {names}")
        if template == "3d":
            ed.check(all(n in names for n in ("Directional Light", "Skybox", "Ground", "Cube")), f"3d: {names}")
            ed.check((target / "materials" / "Ground.material").is_file() or any(target.rglob("Ground.material")), "3d: the Ground material")
        if template == "2d":
            ed.check("Player" in names and any(target.rglob("player.png")), f"2d: the player sprite: {names}")
            ed.check(project.get("canvasWidth") == 1280 and project.get("canvasHeight") == 720, "2d: 1280x720")
        if template == "2d-pixel-art":
            ed.check("Player" in names and "Main Camera" in names, f"2d-pixel-art: the player and its camera: {names}")
            ed.check(project.get("canvasWidth") == 320 and project.get("canvasHeight") == 180, "2d-pixel-art: 320x180")
        if template == "ui":
            ed.check(any(target.rglob("main_menu.html")) and any(target.rglob("main_menu.css")), "ui: the menu's files")
            ed.check(project.get("canvasWidth") == 1920 and project.get("canvasHeight") == 1080, "ui: 1920x1080")
        # exports cleanly
        out = ed.scratch.root / f"export-{template}"
        r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=900)
        ed.check(r["status"] == "succeeded" and not r["result"]["errors"], f"{template} exports: {json.dumps(r)[:600]}")
        # the script runs
        await ed.call("play", action="start", fixed_fps=60, timeout_seconds=300)
        try:
            await ed.call("wait", frames=10)
            console = await ed.call("read_console", levels=["error"])
            errors = [e for e in console.get("entries", []) if e.get("level") == "error"]
            ed.check(not errors, f"{template} plays without errors: {errors[:5]}")
            if template in SCRIPT_PROBES:
                values = (await ed.call("eval_lua", code=SCRIPT_PROBES[template]))["values"]
                ed.check(values == [True], f"{template}: {values}")
            if template == "3d":
                a = (await ed.call("get_runtime_entity", entity="Cube"))
                await ed.call("wait", frames=30)
                b = (await ed.call("get_runtime_entity", entity="Cube"))
                ed.check(harness.find_key(a, "rotation") != harness.find_key(b, "rotation"), "the Rotator turns the cube")
        finally:
            await ed.call("play", action="stop")
        if template == "3d" and os.environ.get("JASPER_SLOW_BUILD") == "1":
            # a job (the first build compiles the engine): polled until it ends
            r = await ed.call("build_project", mode="desktop", output_dir=str(ed.scratch.root / "export-3d-desktop"),
                              build_jobs=3, wait=False)
            job = r
            deadline = time.monotonic() + 3600
            while job.get("status") == "running" and time.monotonic() < deadline:
                await asyncio.sleep(5)
                job = await ed.call("get_job", job_id=r["job_id"])
            ed.check(job.get("status") == "succeeded" and not job.get("result", {}).get("errors"),
                     f"3d desktop build: {json.dumps(job)[:600]}")
    await ed.check_error("create_project", {"path": str(root / "Bad"), "template": "nope"}, contains="list_project_templates")
    # a non-empty target is refused
    (root / "Busy").mkdir(parents=True)
    (root / "Busy" / "file.txt").write_text("x")
    await ed.check_error("create_project", {"path": str(root / "Busy"), "template": "empty", "unsaved": "discard"})


def zip_bytes(entries: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in entries.items():
            z.writestr(name, data)
    return buf.getvalue()


async def test_import_zip(ed: Editor):
    source = ed.scratch.root / "from-templates" / "Game-2d"
    if not (source / "project.yaml").exists():
        made = ed.scratch.root / "zip-source"
        await ed.call("create_project", path=str(made), name="Zip Source", template="2d", unsaved="discard")
        source = made
    await ed.call("save_project")
    entries = {}
    for f in source.rglob("*"):
        rel = f.relative_to(source)
        if f.is_file() and rel.parts[0] not in (".jasperengine", "build"):
            entries[f"Packed/{rel.as_posix()}"] = f.read_bytes()
    archive = ed.scratch.root / "packed.zip"
    archive.write_bytes(zip_bytes(entries))
    into = ed.scratch.root / "imports"
    into.mkdir()
    got = await ed.call("import_project", source=str(archive), path=str(into), timeout_seconds=300)
    imported = Path(got["path"])
    ed.check((imported / "project.yaml").is_file() and got["opened"] is True, f"imported and opened: {got}")
    status = await ed.call("editor_status")
    ed.check_eq(Path(status["project"]["path"]).resolve(), imported.resolve())
    ed.check(not any(p.name.endswith(".importing") for p in into.iterdir()), "no staging folder left")
    # refused archives leave nothing
    before = sorted(p.name for p in into.iterdir())
    evil = ed.scratch.root / "evil.zip"
    evil.write_bytes(zip_bytes({"Game/project.yaml": b"name: x\n", "../escape.txt": b"x"}))
    await ed.check_error("import_project", {"source": str(evil), "path": str(into)})
    no_project = ed.scratch.root / "none.zip"
    no_project.write_bytes(zip_bytes({"readme.txt": b"no project here"}))
    await ed.check_error("import_project", {"source": str(no_project), "path": str(into)}, contains="project.yaml")
    ed.check_eq(sorted(p.name for p in into.iterdir()), before, "nothing left behind")
    ed.check(not (ed.scratch.root / "escape.txt").exists(), "no entry escaped")
    await ed.check_error("import_project", {"source": "file:///tmp/repo.git", "path": str(into)})
    await ed.check_error("import_project", {"source": "-oProxyCommand=touch /tmp/pwned", "path": str(into)})


# ---------------------------------------------------------------------------- start-up conditions

class GuiEditor:
    """The editor's GUI (no `mcp` subcommand) under a private Xvfb, reached over its HTTP MCP server."""

    def __init__(self, root: Path, args: list[str], settings: dict):
        self.root, self.args, self.settings = root, args, settings
        self.proc: subprocess.Popen | None = None
        self.xvfb: harness.Xvfb | None = None
        self.ed: Editor | None = None

    async def __aenter__(self) -> Editor:
        port = free_port()
        cwd = self.root / f"gui-{port}"
        home = cwd / "home"
        home.mkdir(parents=True)
        settings = {"window": {"width": 1280, "height": 720},
                    "mcp": {"http_enabled": True, "http_port": port, "http_token": harness.HTTP_TOKEN}}
        settings.update(self.settings)
        (cwd / "settings.yaml").write_text(yaml.safe_dump(settings, sort_keys=False))
        self.xvfb = harness.Xvfb()
        display = self.xvfb.start()
        env = {k: v for k, v in os.environ.items() if not k.startswith("XDG_") and k not in ("WAYLAND_DISPLAY", "JASPER_MCP_URL")}
        env.update({"DISPLAY": display, "HOME": str(home), "LIBGL_ALWAYS_SOFTWARE": "1"})
        self.log = open(cwd / "editor.log", "w")
        self.proc = subprocess.Popen(harness.editor_command() + self.args, cwd=str(cwd), env=env,
                                     stdout=self.log, stderr=subprocess.STDOUT, start_new_session=True)
        await wait_for_port(port, timeout=180)
        self.ed = Editor(url=f"http://127.0.0.1:{port}/mcp", token=harness.HTTP_TOKEN)
        await self.ed.__aenter__()
        return self.ed

    async def __aexit__(self, *exc) -> None:
        if self.ed:
            with contextlib.suppress(Exception):
                await self.ed.__aexit__(None, None, None)
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                await asyncio.to_thread(self.proc.wait, 60)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                await asyncio.to_thread(self.proc.wait, 30)
        if self.xvfb:
            self.xvfb.stop()
        self.log.close()


async def hub_showing(ed: Editor) -> bool:
    """The project tools refuse while the hub shows over no project."""
    for _ in range(100):
        r = await ed.call_raw("get_scene_tree", {})
        if not r.is_error:
            return False
        if "No project is open" in r.text:
            return True
        await asyncio.sleep(0.3)   # the editor still starting
    raise harness.TestFailure(f"neither the hub nor a project: {r.text}")


async def test_hub_conditions(ed: Editor):
    # create_project over MCP makes a project beside the current one or under the default export
    # folder only (upstream's round 9): the projects go beside this editor's, and the GUI editors'
    # folder is their default export folder (the hub's create_project, with no project open)
    base = ed.project.parent
    root = Path(tempfile.mkdtemp(prefix="jasper-hub-", dir=base))
    project = base / f"{root.name}-Existing"
    await ed.call("create_project", path=str(project), name="Existing", template="empty", unsaved="discard")
    await ed.call("save_project")
    # this editor lets go of it (the project lock): the GUI editors below open it
    await ed.call("create_project", path=str(base / f"{root.name}-Elsewhere"), name="Elsewhere", template="empty", unsaved="discard")
    last = {"last_project_path": str(project), "recent_projects": [{"path": str(project), "name": "Existing"}],
            "export": {"default_dir": str(root)}}

    async with GuiEditor(root, ["--hub"], last) as gui:
        gui.check(await hub_showing(gui), "--hub shows the hub")
        status = await gui.call("editor_status")
        gui.check(status is not None, "editor_status answers over the hub")
        listed = await gui.call("list_project_templates")
        gui.check(len(listed["templates"]) >= 4, "list_project_templates over the hub")
        made = await gui.call("create_project", path=str(root / "FromHub"), name="From Hub", template="2d", unsaved="discard")
        gui.check_eq(made["template"], "2d")
        gui.check(not await hub_showing(gui), "a created project closes the hub")

    async with GuiEditor(root, [], {"last_project_path": str(root / "Gone"), "recent_projects": [str(root / "Gone")]}) as gui:
        gui.check(await hub_showing(gui), "a missing last project shows the hub (no temp project)")

    async with GuiEditor(root, [], {**last, "editor": {"project_hub_at_startup": "always"}}) as gui:
        gui.check(await hub_showing(gui), "Show Project Hub at Startup = Always")

    async with GuiEditor(root, ["--project", str(project)], {**last, "editor": {"project_hub_at_startup": "always"}}) as gui:
        gui.check(not await hub_showing(gui), "--project never shows the hub")
        status = await gui.call("editor_status")
        gui.check_eq(Path(status["project"]["path"]).resolve(), project.resolve())

    async with GuiEditor(root, [], last) as gui:
        gui.check(not await hub_showing(gui), "When No Project with the last project there: it opens")
