"""W5 build tools: get_build_info, build_project (jobs, cancel), generate_shaders, run_build.

Starts from a brand-new project (`mcp --create`): saves its scene, sets the start scene, exports
the C++ source project (its Xcode workspace compiles the project's generated sources in both app
targets: checked by parsing project.pbxproj, no Xcode needed), cancels a Desktop build, generates shaders and checks run_build's process
handling with stand-in "players" (shell scripts), including the crash report a player names (S28).

The real players are slow: set JASPER_SLOW_BUILD=1 to run them. The full Desktop build
(test_desktop_build_and_run: compiles the engine with the game from source, cmake + g++, with
the container's CPUs, harness.build_jobs) is this suite's check of the compile path (method compile, and its compile errors as
file:line). The Development Build's crash report needs a real player, not a compiled one: it is
built with the prebuilt Development runtime of the run's shared build cache (tools/mcp/build_cache.py,
docs/testing.md "Compiled and prebuilt game builds").
"""

import asyncio
import os
import re
import stat
import time
from pathlib import Path

import build_cache
import harness
import pbxproj
from harness import Editor, build_jobs, skip

CI = True
# its compiles: a fixed compile-tree path and ccache (harness.compile_env); a shipped game's release
# link with GCC's LTO, as shipped: test_desktop_build_and_run is the suite's check of the LTO link
# (the other compiled games and the prebuilt test runtimes link without it)
EDITOR = {"create": True, "name": "Build Test", "env": harness.compile_env("test_build", lto=True)}
TIMEOUT = int(os.environ.get("JASPER_BUILD_TIMEOUT", "7200"))
# the container's CPU allowance (harness.build_jobs: JOBS, else docker's --cpus quota): Docker's
# cap is what shares the host, so the compile needs no cap of its own
JOBS = build_jobs()
# the runtime test_development_build_crash_report builds with (the runner starts building it)
PREBUILT_PLAYERS = ["glcore-development"]


async def wait_job(ed: Editor, job_id: str, timeout: float, poll: float = 3.0) -> dict:
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            ed.fail(f"job {job_id} still running after {timeout}s: {job}")
        await asyncio.sleep(poll)


def make_script(path: Path, body: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(path.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return path


async def check_xcode_workspace(ed: Editor, out: Path) -> None:
    """The exported Xcode workspace compiles the project's own sources, as the CMake build does:
    each scene's (named by its id, so a scene called "main" leaves main.cpp alone), the prefabs',
    scene_scripts.cpp and the C++ scripts, in both app targets."""
    project_dir = out / "project"
    pbx_file = out / "workspaces" / "xcode" / "JasperEngine.xcodeproj" / "project.pbxproj"
    ed.check(pbx_file.is_file(), f"no {pbx_file}")
    try:
        root = pbxproj.parse(pbx_file.read_text(encoding="utf-8"))
    except ValueError as e:
        ed.fail(f"the exported project.pbxproj does not parse: {e}")
    ed.check_eq(pbxproj.dangling_references(root), [], "references to objects the project.pbxproj lacks")

    info = await ed.call("get_build_info")
    for scene in info["scenes"]:
        found = sorted(p.name for p in project_dir.glob(f"scene_*_{scene['id']}.cpp"))
        ed.check_eq(len(found), 1, f"the generated source of scene {scene['name']!r} ({scene['id']}): {found}")
    sources = sorted(p.relative_to(project_dir).as_posix() for p in project_dir.rglob("*")
                     if p.suffix in (".cpp", ".c") and p.is_file()
                     and not p.relative_to(project_dir).as_posix().startswith(("assets/", "lua/")))
    ed.check_in("main.cpp", sources)
    ed.check_in("scene_scripts.cpp", sources)
    ed.check(any(s.startswith("scene_") and s != "scene_scripts.cpp" for s in sources), f"scene sources: {sources}")

    in_group = pbxproj.group_paths(root, "../../project")
    for name in ("JasperEngine iOS", "JasperEngine macOS"):
        compiled = pbxproj.target_sources(root, name)
        for source in sources:
            ed.check_eq(compiled.count(Path(source).name), 1, f"{name} compiles {source} once: {compiled}")
            ed.check_eq(in_group.get(source), "PBXFileReference", f"{source} in the project group: {in_group}")
        for settings in pbxproj.target_settings(root, name):
            ed.check_in("$(SRCROOT)/../../project", settings["HEADER_SEARCH_PATHS"], f"{name} include directories")
            ed.check_in("$(SRCROOT)/../../project/scripts", settings["HEADER_SEARCH_PATHS"], f"{name} include directories")
    for name in ("jasperengine-ios", "jasperengine-macos", "JasperEngine iOS", "JasperEngine macOS"):
        for settings in pbxproj.target_settings(root, name):
            # the project's standard, GNU dialect (S01 R16: Exporter::applyXcodeCxxStandard)
            ed.check(re.fullmatch(r'"?gnu\+\+\d+"?', settings["CLANG_CXX_LANGUAGE_STANDARD"]) is not None,
                     f"{name}: {settings['CLANG_CXX_LANGUAGE_STANDARD']}")
    for settings in pbxproj.project_settings(root):
        definitions = settings["GCC_PREPROCESSOR_DEFINITIONS"]
        ed.check(any(d.startswith("MAX_SUBMESHES=") for d in definitions), f"scene capacities: {definitions}")
        ed.check_eq(len(definitions), len(set(definitions)), f"no definition twice: {definitions}")


async def test_build_info(ed: Editor):
    info = await ed.call("get_build_info")
    ed.check_eq(info["host_platform"], "linux")
    ed.check(info["desktop"]["available"], f"cmake/g++ missing on the host? {info['desktop']['missing_tools']}")
    ed.check_in("opengl", info["desktop"]["graphics_apis"])
    ed.check_eq(info["scenes"], [], "the new project's scene is untitled")
    ed.check(info["running_job"] is None, "no build running")


async def test_prepare_project(ed: Editor):
    # nothing to build yet: the scene is untitled
    msg = await ed.call("build_project", mode="source", output_dir=str(ed.scratch.root / "never"), expect_error=True)
    ed.check_in("save", msg.lower())

    if ed.has_tool("create_entity"):
        await ed.call("create_entity", name="Box", type="box")
    s = await ed.call("save_project", untitled_scene_dir="scenes")
    ed.check_eq(len(s["scenes_in_build"]), 1)
    scene = s["scenes_in_build"][0]
    r = await ed.call("edit_build_scenes", action="set_start", scene=scene["id"])
    ed.check(r["scenes_in_build"][0]["start"], "start scene")
    await ed.call("set_project_settings", values={
        "graphics.runtime_shaders": False,       # smaller, faster player build
        "preferences.build_jobs": JOBS,
        "player.window_width": 640,
        "player.window_height": 480,
    })
    info = await ed.call("get_build_info", include_shaders=True)
    ed.check_eq(len(info["scenes"]), 1)
    ed.check(info["shader_count"] > 0, "the scene needs shaders")
    ed.check(all(s["key"].startswith("0x") and s["name"] for s in info["shaders"]), "shader keys and names")


async def test_refusals(ed: Editor):
    out = str(ed.scratch.root / "x")
    await ed.check_error("build_project", mode="console", output_dir=out, contains="mode")
    await ed.check_error("build_project", mode="source", output_dir="relative/dir", contains="absolute")
    msg = await ed.call("build_project", mode="source", output_dir=str(ed.project / "Builds"), expect_error=True)
    ed.check_in("inside the project", msg)
    msg = await ed.call("build_project", mode="desktop", output_dir=out, graphics_api="d3d11", expect_error=True)
    ed.check_in("opengl", msg)
    await ed.check_error("build_project", mode="source", output_dir=out, backends=["glide"], contains="backends")
    await ed.check_error("build_project", mode="desktop", output_dir=out, build_type="Fast", contains="build_type")
    info = await ed.call("get_build_info")
    if not info["web"]["available"]:
        msg = await ed.call("build_project", mode="web", output_dir=out, expect_error=True)
        ed.check_in("Emscripten", msg)


async def test_source_export(ed: Editor):
    out = ed.scratch.root / "export-source"
    r = await ed.call("build_project", mode="source", output_dir=str(out), platform="linux", timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    result = r["result"]
    ed.check_eq(Path(result["output_dir"]).resolve(), out.resolve())
    ed.check((out / "CMakeLists.txt").exists(), "CMakeLists.txt exported")
    ed.check_in("CMakeLists.txt", result["artifacts"])
    ed.check("how_to_build" in result, "how_to_build")
    ed.check(Path(result["log_file"]).exists(), "build log file")
    ed.check_eq(result["errors"], [])
    await check_xcode_workspace(ed, out)
    # web saves are prefixed per game: the default identifier is shared, so the project name
    # (bytes other than letters, digits, . and - escaped for the define)
    ed.check_in('set(JASPERENGINE_STORAGE_ID "Build_20Test")', (out / "CMakeLists.txt").read_text())

    # a non-empty directory needs overwrite
    msg = await ed.call("build_project", mode="source", output_dir=str(out), expect_error=True)
    ed.check_in("overwrite", msg)
    r = await ed.call("build_project", mode="source", output_dir=str(out), overwrite=True,
                      backends=["opengl", "opengles"], timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded")
    # exporting again patches the workspace the same way
    await check_xcode_workspace(ed, out)

    # the remembered output dir is the default next time
    info = await ed.call("get_build_info")
    ed.check_eq(Path(info["source"]["output_dir"]).resolve(), out.resolve())
    ed.check_eq(sorted(info["source"]["backends"]), ["opengl", "opengles"])
    ed.check_eq(info["last_build"]["status"], "succeeded")


async def test_admob_export(ed: Editor):
    """AdMob is opt-in: the Android and iOS projects of a source export carry Google Mobile Ads,
    and the project's App ID (never Google's sample one), only when the project enables it."""
    out = ed.scratch.root / "export-admob"
    android_app = out / "workspaces" / "androidstudio" / "app"
    gradle = android_app / "build.gradle"
    manifest = android_app / "src" / "main" / "AndroidManifest.xml"
    plist = out / "workspaces" / "xcode" / "ios" / "Info.plist"
    pbxproj = out / "workspaces" / "xcode" / "JasperEngine.xcodeproj" / "project.pbxproj"
    android_id = "ca-app-pub-1234567890123456~1111111111"
    ios_id = "ca-app-pub-1234567890123456~2222222222"

    async def export() -> dict:
        r = await ed.call("build_project", mode="source", output_dir=str(out), overwrite=True, timeout_seconds=600)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        return r["result"]

    def check_left_out(what: str):
        g, m, pl, px = gradle.read_text(), manifest.read_text(), plist.read_text(), pbxproj.read_text()
        ed.check("play-services-ads" not in g, f"{what}: no ads SDK in build.gradle")
        ed.check("java-admob" not in g, f"{what}: no AdMob wrapper source in build.gradle")
        ed.check("com.google.android.gms.ads.APPLICATION_ID" not in m, f"{what}: no App ID meta-data in the manifest")
        ed.check("GADApplicationIdentifier" not in pl, f"{what}: no GADApplicationIdentifier in Info.plist")
        ed.check("JASPERENGINE_ADMOB" not in px, f"{what}: the iOS target does not define JASPERENGINE_ADMOB")
        ed.check("AdMobAdapter.m in Sources" not in px, f"{what}: AdMobAdapter.m not compiled")
        ed.check("GoogleMobileAds.xcframework in Frameworks" not in px, f"{what}: GoogleMobileAds not linked")
        ed.check("GoogleMobileAds.xcframework" in px, f"{what}: the file reference itself stays")
        ed.check_in("set(JASPERENGINE_ADMOB OFF)", (out / "CMakeLists.txt").read_text())

    # the default: no AdMob anywhere, and never the sample ids
    await export()
    check_left_out("default")
    for f in (manifest, plist):
        ed.check("3940256099942544" not in f.read_text(), f"{f.name}: no Google sample App ID")
    activity = (out / "platform" / "android" / "java" / "com" / "jaspergameengine" / "jasperengine" / "MainActivity.java").read_text()
    ed.check("new AdMobWrapper" not in activity, "MainActivity does not reference the AdMob wrapper class")

    # an ad unit ID is not an App ID
    msg = await ed.call("set_project_settings", values={"android.admob_app_id": "ca-app-pub-1234567890123456/1111111111"},
                        expect_error=True)
    ed.check_in("app ID", msg)

    # enabled without an App ID: left out, with a warning
    await ed.call("set_project_settings", values={"android.admob_enabled": True, "ios.admob_enabled": True})
    result = await export()
    check_left_out("no App ID")
    log = Path(result["log_file"]).read_text()
    ed.check_in("Android AdMob is enabled but has no App ID", log)
    ed.check_in("iOS AdMob is enabled but has no App ID", log)

    # enabled with App IDs: the SDKs, the wrapper/adapter and the project's ids
    await ed.call("set_project_settings", values={"android.admob_app_id": android_id, "ios.admob_app_id": ios_id})
    back = (await ed.call("get_project_settings", keys=["android.admob_enabled", "android.admob_app_id",
                                                         "ios.admob_enabled", "ios.admob_app_id"]))["settings"]
    ed.check_eq(back, {"android.admob_enabled": True, "android.admob_app_id": android_id,
                       "ios.admob_enabled": True, "ios.admob_app_id": ios_id})
    await export()
    g, m, pl, px = gradle.read_text(), manifest.read_text(), plist.read_text(), pbxproj.read_text()
    ed.check_in('"com.google.android.gms:play-services-ads:', g)
    ed.check_in("/platform/android/java-admob'", g)
    ed.check_in(f'android:value="{android_id}"', m)
    ed.check_in(f"<key>GADApplicationIdentifier</key>\n\t<string>{ios_id}</string>", pl)
    ed.check_eq(px.count("JASPERENGINE_ADMOB,"), 2, "Debug and Release of the iOS target")
    ed.check_in("AdMobAdapter.m in Sources", px)
    ed.check_in("GoogleMobileAds.xcframework in Frameworks", px)
    ed.check_in("set(JASPERENGINE_ADMOB ON)", (out / "CMakeLists.txt").read_text())
    ed.check((out / "platform" / "android" / "java-admob" / "com" / "jaspergameengine" / "jasperengine" /
              "AdMobWrapper.java").exists(), "the wrapper source is exported")

    # saved in project.yaml and back off for the tests that follow
    await ed.call("save_project")
    yaml = (ed.project / "project.yaml").read_text()
    ed.check_in(android_id, yaml)
    ed.check_in(ios_id, yaml)
    await ed.call("set_project_settings", values={"android.admob_enabled": False, "ios.admob_enabled": False})
    await export()
    check_left_out("disabled again")


async def test_cancel_desktop_build(ed: Editor):
    out = ed.scratch.root / "export-cancel"
    # a compile (a prebuilt build could end before the cancel)
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), wait=False, build_jobs=JOBS, method="compile")
    job_id = r["job_id"]
    ed.check_eq(r["status"], "running")

    # one build at a time
    msg = await ed.call("build_project", mode="source", output_dir=str(ed.scratch.root / "other"), expect_error=True)
    ed.check_in(job_id, msg)
    info = await ed.call("get_build_info")
    ed.check_eq(info["running_job"], job_id)
    # the exporter reads the project from its thread: project tools wait for it
    msg = await ed.call("set_project_settings", values={"quality.vsync": True}, expect_error=True)
    ed.check_in(job_id, msg)
    await ed.call("create_project", path=str(ed.scratch.root / "not-now"), expect_error=True)

    await ed.call("cancel_job", job_id=job_id)
    job = await wait_job(ed, job_id, 180, poll=1.0)
    ed.check_eq(job["status"], "cancelled", f"{job}")
    # the editor is still responsive and free to build again
    await ed.call("editor_status")
    ed.check((await ed.call("get_build_info"))["running_job"] is None, "no build running after cancel")


async def test_generate_shaders(ed: Editor):
    out = ed.scratch.root / "shaders"
    r = await ed.call("generate_shaders", output_dir=str(out), backends=["opengl"], format="binary", timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    files = [p for p in out.rglob("*") if p.is_file()]
    ed.check(files, "shader files written")
    await ed.check_error("generate_shaders", output_dir="rel", contains="absolute")
    await ed.check_error("generate_shaders", output_dir=str(out), format="text", contains="format")


async def test_run_build_process_handling(ed: Editor):
    bin_dir = ed.scratch.root / "fake-players"
    crasher = make_script(bin_dir / "crasher", 'echo "starting $1"\necho "Segmentation fault" >&2\nexit 3\n')
    r = await ed.call("run_build", path=str(crasher), seconds=5, args=["--level", "2"], screenshot=False)
    ed.check_eq(r["exit_code"], 3)
    ed.check(r["exited_early"], "exited early")
    ed.check(r["crashed"], "crash detected")
    ed.check_in("Segmentation fault", r["fatal_lines"])
    ed.check_in("starting --level", " ".join(r["log"]))
    ed.check(Path(r["log_file"]).read_text().count("starting") == 1, "log file")

    sleeper = make_script(bin_dir / "sleeper", 'echo "pwd=$(pwd)"\nwhile true; do echo tick; sleep 0.5; done\n')
    t0 = time.monotonic()
    raw = await ed.call_raw("run_build", {"path": str(sleeper), "seconds": 2, "screenshot_after_seconds": 1})
    ed.check(not raw.is_error, raw.text)
    r = raw.value
    ed.check(time.monotonic() - t0 < 15, "stopped in time")
    ed.check(r["stopped_by_run_build"], "stopped by run_build")
    ed.check(not r["crashed"], "a stopped player is not a crash")
    ed.check_in(f"pwd={bin_dir}", r["log"], "runs in its own directory")
    # no window of its own: the whole screen (the editor) is captured
    ed.check(raw.images, "screenshot image")
    ed.check_in("whole screen", r["screenshot"]["note"])
    data, mime = raw.images[0]
    ed.check_eq(mime, "image/png")
    ed.check(data[:8] == b"\x89PNG\r\n\x1a\n", "PNG data")

    await ed.check_error("run_build", path=str(bin_dir / "missing"), contains="No executable")
    await ed.check_error("run_build", path="relative", contains="absolute")
    msg = await ed.call("run_build", path=str(bin_dir), expect_error=True)   # two executables there
    ed.check_in("No player executable", msg)


async def test_run_build_crash_report(ed: Editor):
    """S28 AC19: a player that dies after the engine's crash line reports crashed, its signal and
    crash_report (the report folder the line names)."""
    bin_dir = ed.scratch.root / "fake-players-crash"
    folder = ed.scratch.root / "player-crash" / "reports" / "20261026T120000Z-4242"
    folder.mkdir(parents=True)
    player = make_script(bin_dir / "crashing-player",
                         'echo "Engine ready"\n'
                         f'echo "JASPERENGINE CRASH: SIGSEGV (invalid memory access) report={folder}" >&2\n'
                         'kill -SEGV $$\n')
    r = await ed.call("run_build", path=str(player), seconds=5, screenshot=False)
    ed.check(r["crashed"], f"the crash is detected: {r}")
    ed.check_eq(r.get("signal"), 11, "died from SIGSEGV")
    ed.check_eq(r.get("crash_report"), str(folder), "crash_report is the folder the crash line names")
    ed.check(any("JASPERENGINE CRASH:" in line for line in r["fatal_lines"]), f"a fatal line: {r['fatal_lines']}")

    calm = make_script(bin_dir / "calm-player", 'echo "JASPERENGINE HANG: main thread recovered after 3.1 s (Loading)" >&2\nexit 0\n')
    r = await ed.call("run_build", path=str(calm), seconds=5, screenshot=False)
    ed.check(not r["crashed"], f"a hang line is a warning, not a crash: {r}")
    ed.check("crash_report" not in r, "no crash_report without a crash line")


async def test_development_build_crash_report():
    """S28 AC19 with a real player: a Development Build run with JASPERENGINE_DEBUG_CRASH=segv (the
    player inherits run_build's environment, which is the editor's; the editor itself ignores it)
    crashes once its scene loaded and writes the report run_build names."""
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and crash a real Development Build player (compiles the engine)")
    env = {"JASPERENGINE_DEBUG_CRASH": "segv", **build_cache.player_env("glcore", "development")}
    async with Editor(create=True, name="Crash Build Test", env=env) as ed:
        runtime = await build_cache.player_runtime(ed, "glcore", "development")
        await ed.call("create_entity", name="Box", type="box")
        s = await ed.call("save_project", untitled_scene_dir="scenes")
        await ed.call("edit_build_scenes", action="set_start", scene=s["scenes_in_build"][0]["id"])
        await ed.call("set_project_settings", values={
            "graphics.runtime_shaders": False,
            "preferences.build_jobs": JOBS,
            "player.development_build": True,
        })
        out = ed.scratch.root / "export-desktop-dev"
        r = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
        job = r if r["status"] != "running" else await wait_job(ed, r["job_id"], TIMEOUT - 300, poll=5.0)
        ed.check_eq(job["status"], "succeeded", f"{job}")
        ed.check_eq(job["result"]["method_used"], "prebuilt", f"{job}")
        ed.check_eq(job["result"]["player_id"], runtime.name, "the Development Build's runtime")
        raw = await ed.call_raw("run_build", {"seconds": 60, "screenshot": False})
        r = raw.value
        print("[test] run_build:", raw.text[:1500])
        ed.check(r["crashed"], f"the development build crashed on purpose: {r['log'][-20:]}")
        report = Path(r.get("crash_report", ""))
        ed.check(r.get("crash_report") and (report / "crash.txt").is_file(), f"crash_report names a report folder: {r}")
        text = (report / "crash.txt").read_text()
        ed.check("reason: SIGSEGV" in text and "deliberate: true" in text, f"the report: {text[:800]}")


async def test_desktop_build_and_run(ed: Editor):
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build and run a real Desktop player (compiles the engine)")
    out = ed.scratch.root / "export-desktop"
    t0 = time.monotonic()
    # the suite's compile path check (docs/testing.md): compiled from source, whatever runtimes exist
    r = await ed.call("build_project", mode="desktop", output_dir=str(out), build_jobs=JOBS, timeout_seconds=30, method="compile")
    job_id = r["job_id"]
    job = r if r["status"] != "running" else await wait_job(ed, job_id, TIMEOUT - 300, poll=15.0)
    print(f"[test] desktop build: {job['status']} in {time.monotonic() - t0:.0f}s")
    ed.check_eq(job["status"], "succeeded", f"{job}")
    ed.check_eq(job["result"]["method_used"], "compile", f"{job}")
    ed.check("executable" in job["result"], f"the build names its executable: {job}")
    exe = Path(job["result"]["executable"])
    ed.check(exe.exists(), f"player at {exe}")
    # the suite's check of a shipped game's release link with GCC's LTO (the other compiled games and
    # the prebuilt test runtimes link without it): the compile tree was configured with both
    caches = [p.read_text(errors="replace") for p in (Path(EDITOR["env"]["XDG_CACHE_HOME"]) / "jasperengine" /
                                                      "compile-builds").rglob("CMakeCache.txt")]
    ed.check(any("JASPERENGINE_RELEASE_LINK:BOOL=ON" in c and "JASPERENGINE_RELEASE_LTO:BOOL=ON" in c for c in caches),
             f"the game's release link with LTO (CMakeCache.txt of the compile tree, {len(caches)} found)")

    # the player's own frame, 60 frames of 1/60 s after the one that logs "Engine ready:", not a
    # window grab 6 s after the launch: how far a loaded machine's player has got by then is
    # anyone's guess
    raw = await ed.call_raw("run_build", {"after_ready": True, "fixed_fps": 60, "quit_after": 60})
    ed.check(not raw.is_error, raw.text)
    r = raw.value
    print("[test] run_build:", raw.text[:1500])
    ed.check_eq(Path(r["executable"]).resolve(), exe.resolve())
    ed.check(not r["crashed"], f"the player crashed: {r['fatal_lines']} {r['log'][-20:]}")
    ed.check_eq(r.get("exit_code"), 0, f"the player got ready and quit after its frames: {r}")
    ed.check(raw.images, f"screenshot: {raw.text[:2000]}")
    ed.check_in("player's frame", r["screenshot"]["note"])
    shot = ed.scratch.root / "player.png"
    shot.write_bytes(raw.images[0][0])
    print(f"[test] player screenshot: {shot}")
    # a player that runs on is stopped at the end of `seconds` (no screenshot: nothing waits on time)
    raw = await ed.call_raw("run_build", {"seconds": 5, "screenshot": False})
    ed.check(not raw.is_error, raw.text)
    r = raw.value
    ed.check(not r["crashed"], f"the player crashed: {r['fatal_lines']} {r['log'][-20:]}")
    ed.check(r.get("stopped_by_run_build"), "still running when stopped")

    # compile errors come back as file:line: a broken source in a script directory
    (ed.project / "cpp").mkdir(exist_ok=True)
    (ed.project / "cpp" / "Broken.cpp").write_text("int broken() {\n    return undefined_symbol_xyz;\n}\n")
    await ed.call("set_project_settings", values={"scripting.script_dirs": ["cpp"]})
    raw = await ed.call_raw("build_project", {"mode": "desktop", "output_dir": str(out), "build_jobs": JOBS,
                                              "timeout_seconds": 30, "method": "compile"})
    r = raw.value
    ed.check(raw.is_error == (r["status"] == "failed"), "a failed build is a tool error")
    job = r if r["status"] != "running" else await wait_job(ed, r["job_id"], 3600, poll=10.0)
    ed.check_eq(job["status"], "failed", f"{job}")
    print("[test] broken build:", job["message"])
    errors = job["result"]["errors"]
    ed.check(errors, f"no parsed errors: {job}")
    first = errors[0]
    ed.check_in("undefined_symbol_xyz", first["message"])
    ed.check_eq(first["line"], 2)
    ed.check_eq(first.get("project_file"), "cpp/Broken.cpp")
    (ed.project / "cpp" / "Broken.cpp").unlink()
    await ed.call("set_project_settings", values={"scripting.script_dirs": []})
