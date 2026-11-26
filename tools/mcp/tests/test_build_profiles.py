"""Build Profiles (docs/specs/build-profiles.md): edit_build_profile and its undo steps, the active
profile in Play, build_project profile and the CLI's --profile, what a build of a profile writes
(identity, defines, Scene List, content exclusions) and that a build without a profile writes the keys
it wrote before, and the Build Settings pane through EditorModal.

The builds use the stand-in Linux runtime test_build_method.py installs (/bin/true as the player: the
content is real, nothing runs). test_profile_runs_in_a_real_player runs the run's prebuilt glcore
runtime and needs JASPER_SLOW_BUILD=1, like test_audio_events_prebuilt.py.
"""

import asyncio
import json
import os
import subprocess
import sys
from pathlib import Path

import build_cache
import harness
from harness import Editor, skip

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_build_method import RUNTIME, runtime_archive  # noqa: E402

CI = True
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Box"}
FIXTURE = Path(__file__).resolve().parent / "fixtures" / "build_profiles" / "expected-no-profile.json"
PROFILES = Path("EditorSettings") / "BuildProfiles"


async def setup(ed: Editor) -> dict:
    """three saved scenes (Menu, Level1 with the child scene Hud, Bonus), the stand-in runtime installed, and no
    profile left by an earlier test of the file (they share one project)"""
    await ed.call("tool_groups", enable=["shipping", "editor_ux"])
    for entry in (await ed.call("edit_build_profile", action="list"))["profiles"]:
        await ed.call("edit_build_profile", action="delete", name=entry["name"])
    await ed.call("edit_build_profile", action="activate", name=None)
    if (ed.project / "scenes" / "Menu.scene").exists():
        return {s["name"]: s for s in (await ed.call("list_scenes"))["scenes"]}
    await ed.call("create_scene", name="Menu", path="scenes/Menu.scene", close_previous=True, unsaved="discard")
    await ed.call("create_scene", name="Hud", path="scenes/Hud.scene", type="UI")
    await ed.call("create_scene", name="Level1", path="scenes/Level1.scene")
    await ed.call("set_child_scene", scene="Level1", child="Hud", start_active=True)
    await ed.call("create_scene", name="Bonus", path="scenes/Bonus.scene")
    await ed.call("save_project")
    scenes = {s["name"]: s for s in (await ed.call("list_scenes"))["scenes"]}
    for name in ("Menu", "Level1", "Bonus", "Hud"):
        ed.check(name in scenes, f"scene {name}: {scenes.keys()}")
    info = await ed.call("get_build_info")
    if not any(p["id"] == RUNTIME and p["installed"] for p in info["players"]):
        archive = runtime_archive(ed.scratch.root / f"jasperengine-player-{RUNTIME}.zip", info["editor_version"])
        r = await ed.call("install_player_runtime", archive=str(archive), timeout_seconds=120)
        ed.check_eq(r["status"], "succeeded", f"{r}")
    return scenes


async def build(ed: Editor, out: Path, **args) -> dict:
    r = await ed.call("build_project", output_dir=str(out), timeout_seconds=600, **args)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    return r["result"]


def key_paths(value, prefix: str = "") -> list[str]:
    """every key of a JSON document as a path (list items as []), values masked"""
    paths = []
    if isinstance(value, dict):
        for key, item in value.items():
            path = f"{prefix}.{key}" if prefix else key
            # file maps are keyed by paths, which differ per build
            paths.append(path)
            if key != "files":
                paths.extend(key_paths(item, path))
    elif isinstance(value, list):
        for item in value:
            paths.extend(key_paths(item, prefix + "[]"))
    return sorted(set(paths))


async def profile(ed: Editor, name: str, **args) -> dict:
    return await ed.call("edit_build_profile", name=name, **args)


async def test_edit_build_profile(ed: Editor):
    default = {t.name for t in await ed.tools(groups=None)}
    ed.check("edit_build_profile" not in default, "hidden until shipping is enabled")
    scenes = await setup(ed)
    ed.check("edit_build_profile" in {t.name for t in await ed.tools(groups=None)}, "listed once enabled")
    folder = ed.project / PROFILES

    async def undo_once():
        await ed.call("undo", scope="project")

    # create: one file, one undo step
    r = await profile(ed, "Demo", action="create", platform="linux", values={"product_name": "Box Demo", "defines": ["DEMO"]})
    ed.check_eq(r["files_written"], [f"{PROFILES.as_posix()}/Demo.buildprofile"])
    ed.check((folder / "Demo.buildprofile").is_file(), "written")
    await undo_once()
    ed.check(not (folder / "Demo.buildprofile").exists(), "undo removes the new file")
    await ed.call("redo", scope="project")
    ed.check((folder / "Demo.buildprofile").is_file(), "redo writes it again")

    # set: one file, one step; a refused value names its fix and writes nothing
    before = (folder / "Demo.buildprofile").read_text()
    r = await profile(ed, "Demo", action="set", values={"version_suffix": "-demo", "scenes": ["scenes/Menu.scene"]},
                      overrides={"player.window_width": 640})
    ed.check_eq(len(r["files_written"]), 1)
    ed.check_in("-demo", (folder / "Demo.buildprofile").read_text())
    await undo_once()
    ed.check_eq((folder / "Demo.buildprofile").read_text(), before, "undo restores the file")
    await ed.check_error("edit_build_profile", action="set", name="Demo", overrides={"android.package_format": "apk"},
                         contains="android.package_format is an Android key; this profile builds for Linux: use linux.* or player.* keys")
    await ed.check_error("edit_build_profile", action="set", name="Demo", overrides={"physics.max_bodies": 1000},
                         contains="overrides take player, graphics, quality, linux keys")
    ed.check_eq((folder / "Demo.buildprofile").read_text(), before, "a refused edit writes nothing")

    # duplicate, rename, delete: one step each
    r = await profile(ed, "Demo", action="duplicate", new_name="Demo Copy")
    ed.check((folder / "Demo Copy.buildprofile").is_file(), f"duplicated: {r}")
    await undo_once()
    ed.check(not (folder / "Demo Copy.buildprofile").exists(), "undo removes the copy")
    await profile(ed, "Demo", action="rename", new_name="Trial")
    ed.check((folder / "Trial.buildprofile").is_file() and not (folder / "Demo.buildprofile").exists(), "renamed")
    await undo_once()
    ed.check((folder / "Demo.buildprofile").is_file() and not (folder / "Trial.buildprofile").exists(), "undo renames it back")
    await profile(ed, "Demo", action="delete")
    ed.check(not (folder / "Demo.buildprofile").exists(), "deleted")
    await undo_once()
    ed.check_eq((folder / "Demo.buildprofile").read_text(), before, "undo puts the deleted file back")

    # activate: per user, not an undo step
    history = await ed.call("get_undo_history", scope="project")
    r = await profile(ed, "Demo", action="activate")
    ed.check_eq(r["active_profile"], "Demo")
    ed.check_eq((await ed.call("get_build_info"))["active_profile"], "Demo")
    ed.check_eq([p["name"] for p in (await ed.call("get_build_info"))["profiles"] if p["active"]], ["Demo"])
    ed.check_eq(len((await ed.call("get_undo_history", scope="project"))["entries"]), len(history["entries"]), "activate is no undo step")
    # renaming the active profile keeps it active, and undo and redo move the active name with the file
    await profile(ed, "Demo", action="rename", new_name="Trial")
    ed.check_eq((await ed.call("get_build_info"))["active_profile"], "Trial", "the active profile follows its rename")
    await undo_once()
    ed.check((folder / "Demo.buildprofile").is_file(), "undo renames it back")
    ed.check_eq((await ed.call("get_build_info"))["active_profile"], "Demo", "undo of the rename makes Demo active again")
    await ed.call("redo", scope="project")
    ed.check_eq((await ed.call("get_build_info"))["active_profile"], "Trial", "redo makes Trial active again")
    await undo_once()
    await ed.call("edit_build_profile", action="activate", name=None)
    ed.check_eq((await ed.call("get_build_info"))["active_profile"], None)

    # get: the stored values and where each value the build writes comes from
    r = await profile(ed, "Demo", action="get")
    ed.check_eq(r["values"]["product_name"], "Box Demo")
    ed.check_eq(r["resolved"]["product_name"], {"value": "Box Demo", "source": "profile"})
    ed.check_eq(r["resolved"]["identifier"]["source"], "player.identifier", "a Linux build's identifier is the shared one")
    await profile(ed, "Demo", action="set", values={"product_name": ""})
    r = await profile(ed, "Demo", action="get")
    ed.check_eq(r["resolved"]["product_name"], {"value": "Box", "source": "Project Name"})
    await profile(ed, "Demo", action="set", values={"product_name": "Box Demo"})
    ed.check(scenes, "scenes made")


async def test_build_without_profile_is_unchanged(ed: Editor):
    """AC2: no profile adds no key to game.json or content-manifest.json (keys compared, values masked)"""
    await setup(ed)
    out = ed.scratch.root / "no-profile"
    result = await build(ed, out, mode="player", platform="linux")
    ed.check("profile" not in result, f"no profile in the result: {result}")
    actual = {"game.json": key_paths(json.loads((out / "game.json").read_text())),
              "content-manifest.json": key_paths(json.loads((out / "content-manifest.json").read_text()))}
    if os.environ.get("JASPER_TEST_UPDATE_FIXTURES", "") not in ("", "0"):
        FIXTURE.parent.mkdir(parents=True, exist_ok=True)
        FIXTURE.write_text(json.dumps(actual, indent=2) + "\n")
    expected = json.loads(FIXTURE.read_text())
    for file in expected:
        ed.check_eq(actual[file], expected[file], f"{file}: the keys a build of no profile writes")


async def test_profile_identity_reaches_the_build(ed: Editor):
    """AC3: Product Name, the profile and its defines land in game.json"""
    await setup(ed)
    await ed.call("edit_build_profile", action="create", name="Demo", platform="linux",
                  values={"product_name": "Box Demo", "defines": ["DEMO"]})
    out = ed.scratch.root / "demo"
    result = await build(ed, out, profile="Demo")
    ed.check_eq(result["profile"], "Demo")
    ed.check_eq(result["platform"], "linux")
    game = json.loads((out / "game.json").read_text())
    ed.check_eq(game["window"]["title"], "Box Demo")
    ed.check_eq(game["buildProfile"], "Demo")
    ed.check_eq(game["defines"], ["DEMO"])
    ed.check_eq(json.loads((out / "content-manifest.json").read_text())["buildProfile"], "Demo")
    ed.check_eq(json.loads((out / "build-report.json").read_text())["buildProfile"], "Demo")
    # the open project kept its own settings
    settings = (await ed.call("get_project_settings", keys=["linux.application_name", "player.window_title"]))["settings"]
    ed.check_eq(settings, {"linux.application_name": "", "player.window_title": ""})
    await ed.call("edit_build_profile", action="delete", name="Demo")


async def test_profile_scene_list(ed: Editor):
    """AC5: a profile listing one of three scenes ships it and the child scene it involves, and starts on it"""
    scenes = await setup(ed)
    await ed.call("edit_build_profile", action="create", name="Levels", platform="linux", values={"scenes": ["scenes/Level1.scene"]})
    out = ed.scratch.root / "levels"
    await build(ed, out, profile="Levels")
    game = json.loads((out / "game.json").read_text())
    shipped = sorted(s["name"] for s in game["scenes"])
    ed.check_eq(shipped, ["Hud", "Level1"], "the listed scene and its involved child scene only")
    ed.check_eq(game["startScene"], scenes["Level1"]["id"], "starts on it")
    ed.check(not (out / "scenes" / f"{scenes['Bonus']['id']}.jscn").exists(), "a left-out scene is not cooked")
    # an empty list is refused, naming the fix
    await ed.check_error("edit_build_profile", action="set", name="Levels", values={"scenes": []}, contains="the Scene List is empty")
    await ed.call("edit_build_profile", action="delete", name="Levels")


async def test_excluded_asset_referenced_by_a_scene_fails(ed: Editor):
    """AC6: a texture labelled full-only used by a shipped scene fails the build naming both; unused, it is left out"""
    from test_build_method import png_bytes
    await setup(ed)
    art = ed.project / "art" / "full" / "boss.png"
    art.parent.mkdir(parents=True, exist_ok=True)
    art.write_bytes(png_bytes(8, 8))
    await ed.call("sync_files")
    await ed.call("edit_asset_labels", action="add", assets=["art/full/boss.png"], labels=["full-only"])
    await ed.call("edit_build_profile", action="create", name="Lite", platform="linux", values={"exclude_labels": ["full-only"]})
    out = ed.scratch.root / "lite"
    # unused: absent from the build
    await build(ed, out, profile="Lite")
    ed.check(not any(p.name == "boss.png" for p in out.rglob("*")), "the excluded texture is not shipped")
    # used by a shipped scene: the build fails naming the scene and the asset
    await ed.call("open_scene", scene="Level1")
    await ed.call("create_entity", scene="Level1", name="Boss", type="sprite",
                  values={"sprite": {"width": 8, "height": 8}, "mesh": {"submeshes[0].material.baseColorTexture": "art/full/boss.png"}})
    await ed.call("save_project")
    r = await ed.call("build_project", profile="Lite", output_dir=str(ed.scratch.root / "lite2"), timeout_seconds=600, expect_error=True)
    ed.check_in("Scene Level1 uses art/full/boss.png, which Build Profile 'Lite' excludes (label full-only)", str(r))
    await ed.call("delete_entities", scene="Level1", entities=["Boss"])
    await ed.call("save_project")
    await ed.call("edit_build_profile", action="delete", name="Lite")


async def test_build_project_profile_conflicts(ed: Editor):
    """AC9: build_project and the CLI take the profile's platform; an argument that differs is refused"""
    await setup(ed)
    await ed.call("edit_build_profile", action="create", name="Demo", platform="linux")
    await ed.check_error("build_project", profile="Demo", platform="web", output_dir=str(ed.scratch.root / "x"),
                         contains="differs from Build Profile 'Demo', which builds for linux")
    await ed.check_error("build_project", profile="Nope", contains="no Build Profile 'Nope'")
    cli = await asyncio.to_thread(subprocess.run, harness.editor_command() + ["build", "--project", str(ed.project), "--profile", "Demo",
                                                                            "--method", "prebuilt", "--out", str(ed.scratch.root / "cli-demo")],
                                  capture_output=True, text=True, timeout=900)
    ed.check_in("profile: Demo", cli.stdout, f"stdout {cli.stdout[-2000:]} stderr {cli.stderr[-2000:]}")
    refused = await asyncio.to_thread(subprocess.run, harness.editor_command() + ["build", "--project", str(ed.project), "--profile", "Demo",
                                                                                "--platform", "web", "--out", str(ed.scratch.root / "cli-web")],
                                      capture_output=True, text=True, timeout=900)
    ed.check_eq(refused.returncode, 2, refused.stderr[-2000:])
    ed.check_in("differs from Build Profile 'Demo'", refused.stderr)
    await ed.call("edit_build_profile", action="delete", name="Demo")


async def test_active_profile_in_play(ed: Editor):
    """AC11: Play takes the active profile's name and defines; none active, the project's"""
    await setup(ed)
    await ed.call("edit_build_profile", action="create", name="Demo", platform="linux",
                  values={"product_name": "Box Demo", "defines": ["DEMO"], "scenes": ["scenes/Level1.scene"]})
    await ed.call("edit_build_profile", action="activate", name="Demo")
    await ed.call("play", action="start", scene="Level1")
    try:
        r = await ed.call("eval_lua", code="return {System.getAppName(), System.isDefined('DEMO'), System.getBuildProfile(), System.getBuildInfo().defines[1]}")
        ed.check_eq(r["values"][0], ["Box Demo", True, "Demo", "DEMO"])
        # a scene the profile leaves out still loads in Play, and says a build leaves it out
        await ed.console_since(None)
        await ed.call("eval_lua", code="SceneManager.loadScene('Bonus')")
        await ed.call("wait", frames=3)
        lines = await ed.console_since(limit=200)
        ed.check(any("Scene 'Bonus' is not in Build Profile 'Demo': a build leaves it out" in str(l) for l in lines), f"{lines}")
    finally:
        await ed.call("play", action="stop")
    await ed.call("edit_build_profile", action="activate", name=None)
    await ed.call("play", action="start", scene="Level1")
    try:
        r = await ed.call("eval_lua", code="return {System.getAppName(), System.isDefined('DEMO'), System.getBuildProfile()}")
        ed.check_eq(r["values"][0], ["Box", False, ""])
    finally:
        await ed.call("play", action="stop")
    await ed.call("edit_build_profile", action="delete", name="Demo")


async def test_build_settings_profile_pane(ed: Editor):
    """AC14: Add Build Profile, a Store Title over 30 (the counter's warning), Switch Profile, in Build Settings"""
    await setup(ed)
    await ed.call("run_command", id="file.buildSettings")
    d = None
    for _ in range(40):
        d = (await ed.call("editor_status")).get("dialog")
        if d and d.get("title") == "Build Settings":
            break
        await asyncio.sleep(0.25)
    ed.check(d and d.get("title") == "Build Settings", f"Build Settings opens: {d}")
    try:
        await ed.call("respond_dialog", values={"new_profile_name": "Pane Demo", "new_profile_platform": "Android"})
        r = await ed.call("respond_dialog", choice="add_build_profile")
        ed.check((ed.project / PROFILES / "Pane Demo.buildprofile").is_file(), f"Add Build Profile wrote the file: {r}")
        ed.check_in("platform: android", (ed.project / PROFILES / "Pane Demo.buildprofile").read_text())
        await ed.call("respond_dialog", values={"build_profile": "Pane Demo"})
        await ed.call("respond_dialog", values={"profile_store": "google-play"})
        r = await ed.call("respond_dialog", values={"profile_store_title": "T" * 31})
        dialog = r.get("dialog") or (await ed.call("editor_status")).get("dialog")
        ed.check_in("W-STORE-TITLE-LENGTH", json.dumps(dialog), "the counter's warning")
        ed.check_in("storeTitle", (ed.project / PROFILES / "Pane Demo.buildprofile").read_text(), "each edit is saved")
        await ed.call("respond_dialog", choice="switch_profile")
        ed.check_eq((await ed.call("get_build_info"))["active_profile"], "Pane Demo")
    finally:
        await ed.call("respond_dialog", choice="cancel")
    await ed.call("edit_build_profile", action="activate", name=None)
    await ed.call("edit_build_profile", action="delete", name="Pane Demo")


async def test_profile_runs_in_a_real_player(ed: Editor):
    """AC3, AC5 at runtime: the headless player of a profile's build reads its profile and defines, and a
    load of a left-out scene says it is not in this build"""
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real player (the run builds its prebuilt runtime once)")
    await build_cache.player_runtime(ed, "glcore", "release")
    async with Editor(create=True, name="Box Real", env=build_cache.player_env("glcore", "release")) as real:
        await real.call("tool_groups", enable=["shipping"])
        await real.call("create_scene", name="Menu", path="scenes/Menu.scene", close_previous=True, unsaved="discard")
        await real.call("create_scene", name="Bonus", path="scenes/Bonus.scene")
        await real.call("select_scene", scene="Menu")
        await real.call("create_entity", name="Probe", type="empty")
        await real.call("create_script", kind="lua", name="Probe", entity="Probe", content=(
            "local Probe = {}\nfunction Probe:start()\n"
            "    print('profile=' .. System.getBuildProfile() .. ' demo=' .. tostring(System.isDefined('DEMO')))\n"
            "    SceneManager.loadScene('Bonus')\nend\nreturn Probe\n"))
        await real.call("save_project")
        await real.call("edit_build_profile", action="create", name="Demo", platform="linux",
                        values={"defines": ["DEMO"], "scenes": ["scenes/Menu.scene"]})
        out = real.scratch.root / "real-demo"
        r = await real.call("build_project", profile="Demo", output_dir=str(out), timeout_seconds=900)
        real.check_eq(r["status"], "succeeded", f"{r}")
        run = await real.call("run_build", path=str(out), headless=True, quit_after=30, log_lines=200)
        log = "\n".join(run.get("log", [])) if isinstance(run.get("log"), list) else json.dumps(run)
        real.check_in("profile=Demo demo=true", log)
        real.check_in("Scene 'Bonus' is not in this build (Build Profile 'Demo')", log)
