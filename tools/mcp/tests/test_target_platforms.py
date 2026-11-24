"""Target Platforms (docs/superpowers/specs/2026-11-12-target-platforms-design.md 5.2, 5.5): the
registry row player.target_platforms, get_build_info's target flags, target_platforms and
default_platform, and build_project's defaults and its note on a platform that is not a target.

A brand-new project (`mcp --create`) on a Linux host. The non-target build is a prebuilt web
player through a fake installed runtime, as test_build_method.py makes one: the Linux image has no
Emscripten, and nothing in the runtime runs.
"""

import shutil
import sys
from pathlib import Path

import yaml
from harness import Editor

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_build_method import fake_runtime, install  # noqa: E402

CI = True
EDITOR = {"create": True, "name": "Target Platforms Test"}
KEY = "player.target_platforms"
ALL = ["windows", "linux", "macos", "android", "ios", "web"]
EMPTY_FIX = "non-empty list of windows, linux, macos, android, ios, web"


async def targets(ed: Editor) -> list:
    return (await ed.call("get_project_settings", keys=[KEY]))["settings"][KEY]


def saved_targets(ed: Editor, project=None):
    data = yaml.safe_load(((project or ed.project) / "project.yaml").read_text(encoding="utf-8")) or {}
    return (data.get("export") or {}).get("targetPlatforms")


async def install_web_runtime(ed: Editor) -> None:
    info = await ed.call("get_build_info")
    page = ("<!doctype html><html><head><title>{{JASPERENGINE_TITLE}}</title>{{JASPERENGINE_HEAD_INCLUDE}}</head>"
            "<body><canvas id=\"canvas\"></canvas>{{JASPERENGINE_LOADER}}</body></html>\n")
    await install(ed, fake_runtime(ed.scratch.root / "web-runtime.zip", info["editor_version"], "web-wasm32-gles3-release", "web",
                                   "wasm32", "gles3", {"files/index.html": page.encode(), "files/player-loader.js": b"// loader\n",
                                                       "files/jasperengine-player-st.js": b"// player\n",
                                                       "files/jasperengine-player-st.wasm": b"\0asm\x01\0\0\0"}))


async def test_a_new_project_targets_every_platform(ed: Editor):
    ed.check_eq(await targets(ed), ALL)
    info = await ed.call("get_build_info")
    ed.check_eq(info["target_platforms"], ALL)
    for name in ALL:
        ed.check_eq(info["platforms"][name]["target"], True, name)
    ed.check_eq(info["default_platform"], info["host_platform"], "the host, as before")
    await ed.call("save_project", untitled_scene_dir="scenes")
    ed.check_eq(saved_targets(ed), None, "all six are not written")
    # the handler decides the default build, not the schema
    ed.check("default" not in ed.tool("build_project").input_schema["properties"]["mode"], "mode has no schema default")


async def test_set_refuses_an_empty_or_unknown_list(ed: Editor):
    await ed.check_error("set_project_settings", values={KEY: []}, contains=EMPTY_FIX)
    await ed.check_error("set_project_settings", values={KEY: ["android", "playstation"]}, contains=EMPTY_FIX)
    ed.check_eq(await targets(ed), ALL, "a refused value changes nothing")


async def test_mobile_targets_drive_build_info_and_builds(ed: Editor):
    await ed.call("save_project", untitled_scene_dir="scenes")
    await ed.call("set_project_settings", values={KEY: ["ios", "android"]})
    try:
        ed.check_eq(await targets(ed), ["android", "ios"], "canonical order")
        ed.check_eq(saved_targets(ed), ["android", "ios"], "saved to project.yaml at once")

        info = await ed.call("get_build_info")
        ed.check_eq(info["host_platform"], "linux", "these checks are of a Linux host")
        ed.check_eq(info["target_platforms"], ["android", "ios"])
        ed.check_eq({name: info["platforms"][name]["target"] for name in ALL},
                    {"windows": False, "linux": False, "macos": False, "android": True, "ios": True, "web": False})
        ed.check_eq(info["default_platform"], "android", "the host is not a target: the first target in Build Settings' order")
        ed.check_eq(sorted(info["source"]["backends"]), ["metal-ios", "opengles"], "the targets' presets, not the host's")

        # no mode, and mode player with no platform: the default platform. Here Android builds, or fails
        # on what this computer lacks for it; either way it names Android, never the host
        for args in ({}, {"mode": "player"}):
            r = await ed.call_raw("build_project", {**args, "output_dir": str(ed.scratch.root / "game-default"),
                                                    "overwrite": True, "timeout_seconds": 600}, timeout=700)
            value = r.value
            if r.is_error:
                ed.check_in("Build android", r.text, f"{args}")
            else:
                ed.check_eq(value["status"], "succeeded", f"{args}: {value}")
                ed.check_eq(value["result"]["platform"], "android", f"{args}")

        # a platform that is not a target builds as asked, and says so where the agent reads it
        await install_web_runtime(ed)
        out = ed.scratch.root / "game-web"
        r = await ed.call("build_project", mode="player", platform="web", output_dir=str(out), timeout_seconds=600)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        ed.check_eq(r["result"]["platform"], "web")
        ed.check_eq(r.get("next"), "web is not a target (player.target_platforms: android, ios)", f"{r}")

        # with web the only target, the default build is that web player, with no note
        await ed.call("set_project_settings", values={KEY: ["web"]})
        ed.check_eq((await ed.call("get_build_info"))["default_platform"], "web")
        for args in ({}, {"mode": "player"}):
            r = await ed.call("build_project", {**args, "output_dir": str(out), "timeout_seconds": 600})
            ed.check_eq(r["status"], "succeeded", f"{args}: {r}")
            ed.check_eq(r["result"]["platform"], "web", f"{args}")
            ed.check("next" not in r, f"a target platform has no note: {r.get('next')}")

        # the value survives a reload (a copy of the project, opened)
        await ed.call("set_project_settings", values={KEY: ["android", "ios"]})
        original = ed.project.resolve()
        copy = ed.scratch.root / "reloaded" / "TargetPlatforms"
        shutil.copytree(original, copy, ignore=shutil.ignore_patterns(".jasperengine", "build"))
        await ed.call("open_project", path=str(copy), unsaved="discard")
        try:
            ed.check_eq(await targets(ed), ["android", "ios"], "read back from project.yaml")
            ed.check_eq((await ed.call("get_build_info"))["default_platform"], "android")
        finally:
            await ed.call("open_project", path=str(original), unsaved="discard")
    finally:
        await ed.call("set_project_settings", values={KEY: ALL})
    ed.check_eq(saved_targets(ed), None, "back to all six: the key is gone")
