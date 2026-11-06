"""W5 build tools: get_build_info, build_project (jobs, cancel), generate_shaders, run_build.

Starts from a brand-new project (`mcp --create`): saves its scene, sets the start scene, exports
the C++ source project (its Xcode workspace compiles the project's generated sources in both app
targets: checked by parsing project.pbxproj, no Xcode needed), cancels a Desktop build, generates shaders and checks run_build's process
handling with stand-in players (small native programs recorded with harness.record_built_player in
place of the Desktop build, since run_build runs only the recorded build, unchanged), including the
crash report a player names (S28).

The real players are slow: set JASPER_SLOW_BUILD=1 to run them. The full Desktop build
(test_desktop_build_and_run: compiles the engine with the game from source, cmake + g++, with
the container's CPUs, harness.build_jobs) is this suite's check of the compile path (method compile, and its compile errors as
file:line). The Development Build's crash report needs a real player, not a compiled one: it is
built with the prebuilt Development runtime of the run's shared build cache (tools/mcp/build_cache.py,
docs/testing.md "Compiled and prebuilt game builds").
"""

import asyncio
import json
import os
import re
import shutil
import stat
import tempfile
import time
from pathlib import Path

import build_cache
import yaml

import harness
import pbxproj
from harness import (Editor, build_jobs, make_native_player as make_player, project_player_name as player_name, record_built_player,
                     skip, symlinks_permitted)

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


ACTION_NAMES_SCRIPT = """local ActionNames = {}
function ActionNames:init()
    Log.print("input actions: " .. table.concat(Input.getActionNames(), ","))
    Log.print("mixer groups: " .. table.concat(AudioMixer.getGroupNames(), ",") .. " exposed " ..
              table.concat(AudioMixer.getParameterNames(), ","))
end
return ActionNames
"""
MIXER_LINE = "mixer groups: Master,Music exposed MusicVolume"
ACTION_NAMES_LINE = "input actions: Horizontal,Vertical,Fire1,Fire2,Fire3,Jump,Mouse X,Mouse Y,Mouse ScrollWheel,Submit,Cancel,Dash"


# the fixed component capacities exports passed before mesh-memory
CAPACITY_MACROS = {"MAX_SUBMESHES", "MAX_TILEMAP_TILESRECT", "MAX_TILEMAP_TILES", "MAX_SPRITE_FRAMES",
                   "MAX_EXTERNAL_BUFFERS", "MAX_BONES"}

# what a game's clip measures, read back from the player's log
CLIP_LENGTH_LUA = """local clipLength = {}

function clipLength:init()
    RegisterEngineEvent(self, "onUpdate")
end

function clipLength:onUpdate()
    local source = AudioSource(self.scene, self.entity)
    local length = source:getLength()
    if length > 0 then
        print(string.format("CLIPLEN %.3f", length))
        Engine.quit(0)
    end
end

return clipLength
"""


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
        definitions = settings.get("GCC_PREPROCESSOR_DEFINITIONS", [])
        # components grow their arrays: no capacity definitions (mesh-memory)
        ed.check(not any(d.split("=")[0].strip('"') in CAPACITY_MACROS for d in definitions), f"no capacities: {definitions}")
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
        # the build carries Project Settings > Input: the player prints its actions
        await ed.call("create_script", kind="lua", name="ActionNames", entity="Box", content=ACTION_NAMES_SCRIPT)
    # the Input Manager's actions with Dash: InputManager.inputactions, the project's actions asset
    settings = await ed.call("get_project_settings", keys=["input.manager_actions"])
    actions = settings["settings"]["input.manager_actions"] + [
        {"name": "Dash", "type": "button", "bindings": [{"kind": "buttons", "positive": ["left_shift"]}]}]
    await ed.call("set_project_settings", values={"input.manager_actions": actions})
    # and the project's Audio Mixer (audio/Main.mixer, loaded by the generated init)
    if ed.has_tool("audio_mixer"):
        await ed.call("audio_mixer", action="edit", ops=[
            {"op": "add_group", "name": "Music"},
            {"op": "expose", "name": "MusicVolume", "group": "Music", "volume": True}])
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
    # S67: the Android project builds from a shell: the installed editor's copy has no exec bit to pass on
    ed.check(os.access(out / "workspaces" / "androidstudio" / "gradlew", os.X_OK), "gradlew is executable")
    # web saves are prefixed per game: the default identifier is shared, so the project name
    # (bytes other than letters, digits, . and - escaped for the define)
    ed.check_in('set(JASPERENGINE_STORAGE_ID "Build_20Test")', (out / "CMakeLists.txt").read_text())
    # desktop saves go to a per-user folder named the same (script-data, spec decision 10)
    ed.check_in('set(JASPERENGINE_APP_IDENTIFIER "Build_20Test")', (out / "CMakeLists.txt").read_text())
    # Project Settings > Input: the generated main.cpp loads the project's actions asset, which
    # the export carries with Dash
    mains = [m for m in out.rglob("main.cpp") if "auto-generated by JasperEngine" in m.read_text(errors="replace")]
    ed.check(mains, "the exported generated main.cpp")
    ed.check(any('InputActions::loadDefault("InputManager.inputactions");' in m.read_text() for m in mains),
             "main.cpp loads the actions asset")
    assets = [a for a in out.rglob("InputManager.inputactions") if '"name": "Dash"' in a.read_text()]
    ed.check(assets, "the exported actions asset has Dash")
    # Project Settings > Audio: main.cpp loads the mixer, which ships with the assets
    if ed.has_tool("audio_mixer"):
        ed.check(any('AudioMixer::load("audio/Main.mixer");' in m.read_text() for m in mains), "main.cpp loads the mixer")
        ed.check(any(p.name == "Main.mixer" for p in out.rglob("*.mixer")), "the .mixer is exported")

    # a non-empty directory needs overwrite
    msg = await ed.call("build_project", mode="source", output_dir=str(out), expect_error=True)
    ed.check_in("overwrite", msg)
    r = await ed.call("build_project", mode="source", output_dir=str(out), overwrite=True,
                      backends=["opengl", "opengles"], timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded")
    # exporting again patches the workspace the same way
    await check_xcode_workspace(ed, out)

    # an agent's output folder is not saved as the project's export folder (only the user picks that)
    info = await ed.call("get_build_info")
    ed.check(Path(info["source"]["output_dir"]).resolve() != out.resolve(), f"not remembered: {info['source']['output_dir']}")
    ed.check_eq(sorted(info["source"]["backends"]), ["opengl", "opengles"])
    ed.check_eq(info["last_build"]["status"], "succeeded")


async def test_no_admob(ed: Editor):
    """Built-in AdMob is gone (owner decision 2026-11-04; ads go through the native bridge): a
    source export carries no Google Mobile Ads SDK, wrapper, adapter, App ID or ad-network list,
    the settings are unknown keys, and the Lua API has no AdMob calls."""
    await ed.call("save_project", untitled_scene_dir="scenes")
    out = ed.scratch.root / "export-no-admob"
    r = await ed.call("build_project", mode="source", output_dir=str(out), overwrite=True, timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    android_app = out / "workspaces" / "androidstudio" / "app"
    texts = {
        "build.gradle": (android_app / "build.gradle").read_text(),
        "AndroidManifest.xml": (android_app / "src" / "main" / "AndroidManifest.xml").read_text(),
        "Info.plist": (out / "workspaces" / "xcode" / "ios" / "Info.plist").read_text(),
        "project.pbxproj": (out / "workspaces" / "xcode" / "JasperEngine.xcodeproj" / "project.pbxproj").read_text(),
        "CMakeLists.txt": (out / "CMakeLists.txt").read_text(),
        "MainActivity.java": (out / "platform" / "android" / "java" / "com" / "jaspergameengine" / "jasperengine" /
                              "MainActivity.java").read_text(),
    }
    for name, text in texts.items():
        for needle in ("play-services-ads", "java-admob", "com.google.android.gms.ads.APPLICATION_ID", "GADApplicationIdentifier",
                       "SKAdNetworkItems", "JASPERENGINE_ADMOB", "AdMob", "GoogleMobileAds", "3940256099942544"):
            ed.check(needle not in text, f"{name} has no {needle}")
    ed.check(not (out / "platform" / "android" / "java-admob").exists(), "no AdMob wrapper source")
    ed.check(not (out / "platform" / "apple" / "GoogleMobileAdsSdkiOS-10.14.0").exists(), "no Google Mobile Ads SDK copy")
    for key in ("android.admob_enabled", "ios.admob_app_id"):
        await ed.check_error("set_project_settings", values={key: True}, contains=key)
    stubs = "\n".join(p.read_text() for p in (ed.project / ".jasperengine" / "engine-api" / "lua").rglob("*.lua"))
    ed.check("System" in stubs, "the Lua stubs were written")
    for name in ("initializeAdMob", "setMaxAdContentRating", "loadInterstitialAd", "isInterstitialAdLoaded", "showInterstitialAd",
                 "AdMobRating"):
        ed.check(name not in stubs, f"the Lua API has no {name}")


async def test_privacy_manifest_export(ed: Editor):
    """mobile-shipping: every Apple app of a source export ships PrivacyInfo.xcprivacy (before: none):
    the engine's required-reason APIs, the project's apple.privacy_* additions (undoable) and the
    static plugins' manifests of that platform."""
    import plistlib
    await ed.call("save_project", untitled_scene_dir="scenes")
    msg = await ed.call("set_project_settings", values={"apple.privacy_accessed_apis": [{"type": "FileTimestamp", "reasons": ["CA92.1"]}]},
                        expect_error=True)
    ed.check_in("C617.1", msg, "a wrong reason lists the valid ones")
    await ed.call("set_project_settings", values={
        "apple.privacy_tracking": True, "apple.privacy_tracking_domains": ["ads.example.com"],
        "apple.privacy_collected_data": [{"type": "DeviceID", "linked": True, "tracking": True, "purposes": ["ThirdPartyAdvertising"]}]})
    await ed.call("set_project_settings", values={"apple.privacy_accessed_apis": [{"type": "DiskSpace", "reasons": ["E174.1"]}]})
    plugin = ed.project / "plugins" / "native" / "ios" / "Ads" / "PrivacyInfo.xcprivacy"
    plugin.parent.mkdir(parents=True, exist_ok=True)
    plugin.write_bytes(plistlib.dumps({"NSPrivacyAccessedAPITypes": [{"NSPrivacyAccessedAPIType": "NSPrivacyAccessedAPICategoryActiveKeyboards",
                                                                     "NSPrivacyAccessedAPITypeReasons": ["54BD.1"]}]}))
    out = ed.scratch.root / "export-privacy"
    try:
        r = await ed.call("build_project", mode="source", output_dir=str(out), overwrite=True, timeout_seconds=600)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        manifests = {}
        for platform in ("ios", "macos"):
            data = plistlib.loads((out / "workspaces" / "xcode" / platform / "PrivacyInfo.xcprivacy").read_bytes())
            manifests[platform] = {a["NSPrivacyAccessedAPIType"].removeprefix("NSPrivacyAccessedAPICategory"): a["NSPrivacyAccessedAPITypeReasons"]
                                   for a in data["NSPrivacyAccessedAPITypes"]}
            ed.check_eq(data["NSPrivacyTracking"], True, platform)
            ed.check_eq(data["NSPrivacyTrackingDomains"], ["ads.example.com"], platform)
            ed.check_eq(data["NSPrivacyCollectedDataTypes"][0]["NSPrivacyCollectedDataType"], "NSPrivacyCollectedDataTypeDeviceID")
            for engine_api, reason in (("UserDefaults", "CA92.1"), ("FileTimestamp", "C617.1"), ("SystemBootTime", "35F9.1"), ("DiskSpace", "E174.1")):
                ed.check_in(reason, manifests[platform].get(engine_api, []), f"{platform} {engine_api}")
        ed.check_in("ActiveKeyboards", manifests["ios"], "the iOS plugin's manifest")
        ed.check("ActiveKeyboards" not in manifests["macos"], "an iOS plugin's manifest is not macOS's")
        root = pbxproj.parse((out / "workspaces" / "xcode" / "JasperEngine.xcodeproj" / "project.pbxproj").read_text())
        objects = root["objects"]
        for target_name, path in (("JasperEngine iOS", "ios/PrivacyInfo.xcprivacy"), ("JasperEngine macOS", "macos/PrivacyInfo.xcprivacy")):
            resources = [objects[objects[b]["fileRef"]].get("path") for phase in pbxproj.target(root, target_name)["buildPhases"]
                         if objects[phase]["isa"] == "PBXResourcesBuildPhase" for b in objects[phase]["files"]]
            ed.check_in(path, resources, target_name)
        # one undo of the project history takes the last call's change back
        await ed.call("undo", scope="project")
        back = (await ed.call("get_project_settings", keys=["apple.privacy_accessed_apis", "apple.privacy_tracking"]))["settings"]
        ed.check_eq(back["apple.privacy_accessed_apis"], [], "undone")
        ed.check_eq(back["apple.privacy_tracking"], True, "the earlier steps stay")
    finally:
        await ed.call("set_project_settings", values={"apple.privacy_tracking": False, "apple.privacy_tracking_domains": [],
                                                      "apple.privacy_collected_data": [], "apple.privacy_accessed_apis": []})
        shutil.rmtree(ed.project / "plugins", ignore_errors=True)


async def test_platform_services_export(ed: Editor):
    """Player > Resolution and Presentation reaches the Android manifest (screenOrientation, a
    configChanges list that keeps the activity on rotation and locale changes) and the iOS
    Info.plist (orientation arrays with the default first, UIRequiresFullScreen); the scripts'
    System.vibrate / getInternetReachability add their Android permissions; screen sleep is
    Screen.sleepTimeout's (no MainActivity patch); the generated init() sets them."""
    out = ed.scratch.root / "export-platform"
    android_app = out / "workspaces" / "androidstudio" / "app"
    manifest = android_app / "src" / "main" / "AndroidManifest.xml"
    plist = out / "workspaces" / "xcode" / "ios" / "Info.plist"
    activity = out / "platform" / "android" / "java" / "com" / "jaspergameengine" / "jasperengine" / "MainActivity.java"

    async def export() -> dict:
        r = await ed.call("build_project", mode="source", output_dir=str(out), overwrite=True, timeout_seconds=600)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        return r["result"]

    # the defaults: auto-rotation in portrait and both landscapes
    await export()
    m, pl = manifest.read_text(), plist.read_text()
    ed.check_in('android:screenOrientation="user"', m)
    ed.check_in('android:configChanges="mcc|mnc|locale|touchscreen|keyboard|keyboardHidden|navigation|orientation|'
                'screenLayout|uiMode|screenSize|smallestScreenSize|fontScale|layoutDirection|density"', m)
    ed.check("android.permission.VIBRATE" not in m, "no VIBRATE without System.vibrate")
    # oe-port's MainActivity keeps one guarded line for S30's prebuilt Android player (its manifest's
    # jasperengine.keepScreenOn meta-data, applied before the engine starts); a source export has no
    # such meta-data, so nothing keeps the screen on but Screen.sleepTimeout
    patched = [line for line in activity.read_text().splitlines()
               if "FLAG_KEEP_SCREEN_ON" in line and 'metaFlag("jasperengine.keepScreenOn", false)' not in line]
    ed.check(not patched and "jasperengine.keepScreenOn" not in m,
             f"no keep-screen-on patch: Screen.sleepTimeout does it: {patched}")
    portrait_first = ("<key>UISupportedInterfaceOrientations</key>\n\t<array>\n"
                      "\t\t<string>UIInterfaceOrientationPortrait</string>\n"
                      "\t\t<string>UIInterfaceOrientationLandscapeRight</string>\n"
                      "\t\t<string>UIInterfaceOrientationLandscapeLeft</string>\n\t</array>")
    ed.check_in(portrait_first, pl)
    ed.check_in(portrait_first.replace("Orientations</key>", "Orientations~ipad</key>"), pl)
    ed.check_in("<key>UIRequiresFullScreen</key>\n\t<true/>", pl)
    main = (out / "project" / "main.cpp").read_text()
    ed.check_in("Screen::initialize(ScreenOrientation::AutoRotation, 0x1A, true);", main)
    ed.check_in("Engine::setTargetFrameRate(-1);", main)
    ed.check_in("System::setApplicationInfo({\"Build Test\", \"com.yourcompany.project\", \"1.0\", 1});", main)

    # a fixed landscape default, a script that vibrates and checks the network
    await ed.call("create_script", kind="lua", name="Haptics", content=(
        "local Haptics = { properties = {} }\n"
        "function Haptics:init()\n"
        "    if System.getInternetReachability() ~= NetworkReachability.NotReachable then System.vibrate(50) end\n"
        "end\nreturn Haptics\n"))
    await ed.call("set_project_settings", values={"player.default_orientation": "landscape_right", "player.target_frame_rate": 30})
    try:
        result = await export()
        m, pl = manifest.read_text(), plist.read_text()
        ed.check_in('android:screenOrientation="reverseLandscape"', m)
        ed.check_in('<uses-permission android:name="android.permission.VIBRATE" />', m)
        ed.check_in('<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />', m)
        ed.check_in("<key>UISupportedInterfaceOrientations</key>\n\t<array>\n"
                    "\t\t<string>UIInterfaceOrientationLandscapeLeft</string>\n\t</array>", pl)
        log = Path(result["log_file"]).read_text()
        ed.check_in("added the VIBRATE permission", log)
        main = (out / "project" / "main.cpp").read_text()
        ed.check_in("Screen::initialize(ScreenOrientation::LandscapeRight,", main)
        ed.check_in("Engine::setTargetFrameRate(30);", main)

        # every orientation allowed: iPad multitasking may stay on
        await ed.call("set_project_settings", values={
            "player.default_orientation": "auto_rotation",
            "player.allowed_orientations": ["portrait", "portrait_upside_down", "landscape_left", "landscape_right"]})
        await export()
        ed.check_in('android:screenOrientation="fullUser"', manifest.read_text())
        ed.check_in("<key>UIRequiresFullScreen</key>\n\t<false/>", plist.read_text())
    finally:
        await ed.call("set_project_settings", values={
            "player.default_orientation": "auto_rotation", "player.target_frame_rate": -1,
            "player.allowed_orientations": ["portrait", "landscape_left", "landscape_right"]})


async def test_http_export(ed: Editor):
    """The HTTP client's export surfaces (http spec 6.7, 9): Android INTERNET (and
    ACCESS_NETWORK_STATE) once when exported Lua (an alias `local H = Http` too), an HTML UI
    document's Lua or a C++ script uses Http (Internet Access auto), never without it, always with
    require; the cleartext policy in network_security_config.xml and App Transport Security in
    both Info.plists follow player.allow_insecure_http; the NSURLSession backend is compiled by
    both Apple engine targets."""
    internet = '<uses-permission android:name="android.permission.INTERNET" />'
    network_state = '<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />'
    plist_names = [("ios", "Info.plist"), ("macos", "Info.plist")]
    exports = iter(range(100))

    async def export() -> tuple[Path, str]:
        # a new folder each time: files an earlier export left in one ship with the next
        out = ed.scratch.root / f"export-http-{next(exports)}"
        r = await ed.call("build_project", mode="source", output_dir=str(out), timeout_seconds=600)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        return out, Path(r["result"]["log_file"]).read_text()

    def manifest(out: Path) -> str:
        return (out / "workspaces" / "androidstudio" / "app" / "src" / "main" / "AndroidManifest.xml").read_text()

    def security(out: Path) -> str:
        return (out / "workspaces" / "androidstudio" / "app" / "src" / "main" / "res" / "xml" / "network_security_config.xml").read_text()

    def plists(out: Path) -> list[str]:
        return [(out / "workspaces" / "xcode" / d / n).read_text() for d, n in plist_names]

    # Lua, through an alias
    script = ed.project / "lua" / "net" / "analytics.lua"
    script.parent.mkdir(parents=True, exist_ok=True)
    script.write_text('local H = Http\nlocal M = {}\nfunction M.send(id)\n    H.post("https://backend.example.com/Interact", {playerID = id})\nend\nreturn M\n')
    try:
        out, log = await export()
        m = manifest(out)
        ed.check_eq(m.count(internet), 1, f"INTERNET once: {m}")
        ed.check_eq(m.count(network_state), 1, f"ACCESS_NETWORK_STATE once: {m}")
        ed.check_in('android:networkSecurityConfig="@xml/network_security_config"', m)
        ed.check_in("Android: added INTERNET (Http used in ", log)
        ed.check_in("analytics.lua", log)
        x = security(out)
        ed.check_in('<base-config cleartextTrafficPermitted="false"/>', x)
        for host in ("localhost", "127.0.0.1", "::1"):
            ed.check_in(f'<domain includeSubdomains="false">{host}</domain>', x)
        for text in plists(out):
            ed.check_in("<key>NSAppTransportSecurity</key>\n\t<dict>\n\t\t<key>NSAllowsLocalNetworking</key>\n\t\t<true/>\n\t</dict>", text)
            ed.check_eq(text.count("NSAppTransportSecurity"), 1, "one ATS dictionary")
        ed.check_in("set(JASPERENGINE_ALLOW_INSECURE_HTTP OFF)", (out / "CMakeLists.txt").read_text())
        ed.check_in("Http::setInsecureHttpAllowed(false);", (out / "project" / "main.cpp").read_text())

        # ticked by hand as well: still once
        await ed.call("set_project_settings", values={"android.permissions": ["internet", "access_network_state", "vibrate"]})
        out, _ = await export()
        m = manifest(out)
        ed.check_eq((m.count(internet), m.count(network_state)), (1, 1), "each permission once with them ticked too")
        await ed.call("set_project_settings", values={"android.permissions": []})
    finally:
        script.unlink(missing_ok=True)

    # nothing uses Http (the generated main.cpp calling Http:: does not count): auto adds nothing
    out, _ = await export()
    ed.check(internet not in manifest(out), "no INTERNET without Http (auto)")

    # an HTML UI document's Lua only
    document = ed.project / "assets" / "ui" / "news.html"
    document.parent.mkdir(parents=True, exist_ok=True)
    document.write_text('<p id="news">...</p>\n<script type="text/lua">\nHttp.get("https://example.com/news", {owner = document}, function(r) end)\n</script>\n')
    try:
        out, log = await export()
        ed.check_eq(manifest(out).count(internet), 1, "INTERNET for an HTML document's Lua")
        ed.check_in("news.html", log)
    finally:
        document.unlink(missing_ok=True)

    # a C++ script only
    scene = (await ed.call("get_build_info"))["scenes"][0]["id"]
    made = await ed.call("create_entity", scene=scene, type="empty", name="NetProbeHolder")
    holder = made["entity_id"]
    await ed.call("create_script", kind="cpp_script", name="NetProbe", scene=scene, entity=holder,
                  source_content='#include "NetProbe.h"\n#include "net/Http.h"\n'
                                 'NetProbe::NetProbe(jasperengine::Scene* scene, jasperengine::Entity entity)\n'
                                 '    : jasperengine::ScriptBase(scene, entity) {\n'
                                 '    jasperengine::HttpRequestDesc desc;\n    desc.url = "https://example.com/";\n'
                                 '    jasperengine::Http::send(desc, [](const jasperengine::HttpResponse&) {});\n}\n')
    await ed.call("save_project")
    try:
        out, log = await export()
        ed.check_eq(manifest(out).count(internet), 1, "INTERNET for a C++ script")
        ed.check_in("NetProbe.cpp", log)
    finally:
        await ed.call("delete_entities", scene=scene, entity=holder)
        await ed.call("save_project")

    await ed.call("set_project_settings", values={"android.internet_access": "require", "player.allow_insecure_http": True})
    try:
        out, _ = await export()
        ed.check_eq(manifest(out).count(internet), 1, "INTERNET with require")
        ed.check_in('<base-config cleartextTrafficPermitted="true"/>', security(out))
        for text in plists(out):
            ed.check_in("<key>NSAllowsArbitraryLoads</key>\n\t\t<true/>", text)
            ed.check("NSAllowsLocalNetworking" not in text, "Apple ignores NSAllowsArbitraryLoads next to it")
            ed.check_eq(text.count("NSAppTransportSecurity"), 1, "still one ATS dictionary")
        ed.check_in("Http::setInsecureHttpAllowed(true);", (out / "project" / "main.cpp").read_text())
        ed.check_in("set(JASPERENGINE_ALLOW_INSECURE_HTTP ON)", (out / "CMakeLists.txt").read_text())
    finally:
        await ed.call("set_project_settings", values={"android.internet_access": "auto", "player.allow_insecure_http": False})

    root = pbxproj.parse((out / "workspaces" / "xcode" / "JasperEngine.xcodeproj" / "project.pbxproj").read_text(encoding="utf-8"))
    for name in ("jasperengine-ios", "jasperengine-macos"):
        compiled = pbxproj.target_sources(root, name)
        ed.check_eq(compiled.count("HttpBackendApple.mm"), 1, f"{name} compiles the NSURLSession backend")
        ed.check_eq(compiled.count("Http.cpp"), 1, f"{name} compiles the HTTP client")
        ed.check_eq(compiled.count("HttpBackendCurl.cpp"), 0, f"{name} leaves the libcurl backend out")


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
    # renaming the project (its player's name) is refused for the whole job: what the build writes
    # and records is the name it started with
    msg = await ed.call("save_project", name="Renamed Mid Build", expect_error=True)
    ed.check_in(job_id, msg)

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
    name = await player_name(ed)
    crash_dir = ed.scratch.root / "fake-players" / "crash"
    crasher = make_player(crash_dir / name, 'int main(int c, char** v){ printf("starting %s\\n", c > 1 ? v[1] : ""); fflush(stdout);'
                                            ' fprintf(stderr, "Segmentation fault\\n"); return 3; }\n')
    record_built_player(ed.project, crasher)   # stands in for build_project mode=desktop
    r = await ed.call("run_build", path=str(crasher), seconds=5, args=["--level", "2"], screenshot=False)
    ed.check_eq(r["exit_code"], 3)
    ed.check(r["exited_early"], "exited early")
    ed.check(r["crashed"], "crash detected")
    ed.check_in("Segmentation fault", r["fatal_lines"])
    ed.check_in("starting --level", " ".join(r["log"]))
    ed.check(Path(r["log_file"]).read_text().count("starting") == 1, "log file")

    sleep_dir = ed.scratch.root / "fake-players" / "sleep"
    sleeper = make_player(sleep_dir / name, 'int main(void){ char d[4096]; if (getcwd(d, sizeof d)) printf("pwd=%s\\n", d);'
                                            ' for (;;) { printf("tick\\n"); fflush(stdout); usleep(500000); } }\n')
    record_built_player(ed.project, sleeper)
    t0 = time.monotonic()
    raw = await ed.call_raw("run_build", {"path": str(sleeper), "seconds": 2, "screenshot_after_seconds": 1})
    ed.check(not raw.is_error, raw.text)
    r = raw.value
    ed.check(time.monotonic() - t0 < 15, "stopped in time")
    ed.check(r["stopped_by_run_build"], "stopped by run_build")
    ed.check(not r["crashed"], "a stopped player is not a crash")
    ed.check_in(f"pwd={sleep_dir.resolve()}", r["log"], "runs in its own directory")
    # no window of its own: the whole screen (the editor) is captured
    ed.check(raw.images, "screenshot image")
    ed.check_in("whole screen", r["screenshot"]["note"])
    data, mime = raw.images[0]
    ed.check_eq(mime, "image/png")
    ed.check(data[:8] == b"\x89PNG\r\n\x1a\n", "PNG data")

    await ed.check_error("run_build", path=str(sleep_dir / "missing"), contains="No executable")
    await ed.check_error("run_build", path="relative", contains="absolute")
    # a script or a binary the build did not record (named as the player or not): refused
    script = make_script(ed.scratch.root / "fake-players" / "script" / name, "echo hi\n")
    await ed.check_error("run_build", path=str(script), contains="not built by this editor")
    other = make_player(ed.scratch.root / "fake-players" / "other" / "tool", "int main(void){ return 0; }\n")
    await ed.check_error("run_build", path=str(other), contains="not built by this editor")


async def test_run_build_refuses_unrecorded_or_changed_players(ed: Editor):
    """run_build runs only the file the last Desktop build recorded (.jasperengine/user/runbuild.yaml),
    unchanged: everything else is refused with the reason, and nothing runs."""
    name = await player_name(ed)
    root = ed.scratch.root / "fake-players"
    marker = ed.scratch.root / "ran-marker"
    quick = ('int main(void){ FILE* f = fopen("' + str(marker) + '", "a"); if (f) { fputs("ran\\n", f); fclose(f); }'
             ' printf("quick ran\\n"); return 0; }\n')

    async def refused(path: Path, why: str) -> None:
        r = await ed.call_raw("run_build", {"path": str(path), "seconds": 1, "screenshot": False})
        ed.check(r.is_error and "this project built" in r.text and why in r.text, f"run_build {path}: {r.text[:300]}")
        ed.check(not marker.exists(), f"nothing ran for {path}")

    # recorded and unchanged: runs (the baseline every refusal below differs from by one thing)
    ok = make_player(root / "ok" / name, quick)
    record_built_player(ed.project, ok)
    r = await ed.call("run_build", path=str(ok), seconds=2, screenshot=False)
    ed.check_in("quick ran", " ".join(r["log"]))
    ed.check(marker.exists(), "the recorded player ran")
    marker.unlink()

    # a player that was never recorded: the same bytes at another path, and nothing built at all
    never = make_player(root / "never" / name, quick)
    await refused(never, "not built by this editor")
    await refused(never.parent, "not built by this editor")          # its folder: finds it by name, still refused
    # a copy of a system program, named as the player; the system program itself
    system_program = Path(os.path.realpath(shutil.which("true") or "/bin/true"))
    copied = root / "copy" / name
    copied.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(system_program, copied)
    await refused(copied, "not built by this editor")
    await refused(system_program, "not built by this editor")
    # a copy of the recorded player itself (same bytes, same name, another file)
    twin = root / "twin" / name
    twin.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ok, twin)
    await refused(twin, "not built by this editor")

    # a symbolic link to the recorded player (and a link recorded as the player)
    if symlinks_permitted():
        link = root / "link" / name
        link.parent.mkdir(parents=True, exist_ok=True)
        link.symlink_to(ok)
        await refused(link, "symbolic link")
        record_built_player(ed.project, link)    # the record resolves it; the link itself never runs
        await refused(link, "symbolic link")

    # recorded, then its bytes changed: appended, and overwritten in place with the mtime put back
    grown = make_player(root / "grown" / name, quick)
    record_built_player(ed.project, grown)
    with open(grown, "ab") as f:
        f.write(b"\0")
    await refused(grown, "contents changed")
    patched = make_player(root / "patched" / name, quick)
    record_built_player(ed.project, patched)
    st = patched.stat()
    data = bytearray(patched.read_bytes())
    data[-1] ^= 0xFF
    with open(patched, "r+b") as f:
        f.write(bytes(data))
    os.utime(patched, ns=(st.st_atime_ns, st.st_mtime_ns))
    ed.check_eq(patched.stat().st_mtime_ns, st.st_mtime_ns, "mtime restored")
    await refused(patched, "contents changed")

    # recorded, then only its mtime changed
    touched = make_player(root / "touched" / name, quick)
    record_built_player(ed.project, touched)
    st = touched.stat()
    os.utime(touched, ns=(st.st_atime_ns, st.st_mtime_ns - 30 * 10**9))
    await refused(touched, "modified since")

    # recorded, then replaced by a new file with the same bytes (another inode)
    swapped = make_player(root / "swapped" / name, quick)
    record_built_player(ed.project, swapped)
    fresh = swapped.with_name(name + ".new")
    shutil.copy2(swapped, fresh)
    os.replace(fresh, swapped)
    await refused(swapped, "replaced since")

    # a renamed project: a binary named as its new player in the build folder was not built by it;
    # the player the build recorded under the old name still runs (it is the built file, unchanged)
    out = root / "out"
    built = make_player(out / name, quick)
    record_built_player(ed.project, built)
    old_project_name = (await ed.call("get_project_summary"))["name"]
    await ed.call("save_project", name="Renamed Build Test", untitled_scene_dir="scenes")
    try:
        new_name = await player_name(ed)
        ed.check(new_name != name, f"the player name follows the project name: {new_name}")
        renamed = out / new_name
        shutil.copy2(built, renamed)
        await refused(renamed, "not built by this editor")
        await refused(out, "not built by this editor")               # the folder finds the new name
        r = await ed.call("run_build", path=str(built), seconds=2, screenshot=False)
        ed.check_in("quick ran", " ".join(r["log"]))
    finally:
        await ed.call("save_project", name=old_project_name, untitled_scene_dir="scenes")


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
    record_built_player(ed.project, player)   # run_build runs only the recorded build (BuiltPlayers)
    r = await ed.call("run_build", path=str(player), seconds=5, screenshot=False)
    ed.check(r["crashed"], f"the crash is detected: {r}")
    ed.check_eq(r.get("signal"), 11, "died from SIGSEGV")
    ed.check_eq(r.get("crash_report"), str(folder), "crash_report is the folder the crash line names")
    ed.check(any("JASPERENGINE CRASH:" in line for line in r["fatal_lines"]), f"a fatal line: {r['fatal_lines']}")

    calm = make_script(bin_dir / "calm-player", 'echo "JASPERENGINE HANG: main thread recovered after 3.1 s (Loading)" >&2\nexit 0\n')
    record_built_player(ed.project, calm)
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
    ed.check_eq(exe.name, (await ed.call("get_build_info"))["desktop"]["executable_name"], "the player's name")
    # the build recorded the player (.jasperengine/user/runbuild.yaml) in the format the test
    # fixture writes: the fixture's entry for the same file is identical
    recorded = yaml.safe_load((ed.project / ".jasperengine" / "user" / "runbuild.yaml").read_text())
    ed.check(isinstance(recorded, dict) and "desktop" in recorded, f"record: {recorded}")
    with tempfile.TemporaryDirectory() as tmp:
        ed.check_eq(recorded["desktop"], record_built_player(tmp, exe), "the build's record = the fixture's")
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
    log_text = "\n".join(r["log"]) if isinstance(r["log"], list) else str(r["log"])
    ed.check_in(ACTION_NAMES_LINE, log_text, "the player has the project's input actions")
    if ed.has_tool("audio_mixer"):
        ed.check_in(MIXER_LINE, log_text, "the player loaded the project's mixer")
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
    # once the built player changes it no longer runs
    st = exe.stat()
    os.utime(exe, ns=(st.st_atime_ns, st.st_mtime_ns + 10**9))
    await ed.check_error("run_build", {"seconds": 1, "screenshot": False}, contains="modified since")
    os.utime(exe, ns=(st.st_atime_ns, st.st_mtime_ns))

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


async def test_audio_import_table():
    """S36 AC5: a prebuilt player reads the clip's import settings as data. The export writes the table and lists
    it in the content manifest, the player's clip has the trimmed length, and a project without settings has no
    table."""
    if os.environ.get("JASPER_SLOW_BUILD", "") in ("", "0"):
        skip("set JASPER_SLOW_BUILD=1 to build a real prebuilt player")
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    import audio_refs as ar
    env = build_cache.player_env("glcore", "development")
    async with Editor(create=True, name="Audio Table Test", env=env) as ed:
        await build_cache.player_runtime(ed, "glcore", "development")
        rel = "assets/sfx/tone.wav"
        ar.write_wav(ed.project / rel, [ar.sine(1000.0, 1.0, 48000, -12.0)], 48000, ar.PCM16)
        await ed.call("sync_files", paths=[rel])
        await ed.call("write_file", path="scripts/clip_length.lua", content=CLIP_LENGTH_LUA)
        await ed.call("create_entity", name="Clip", type="audio_source", values={"audiosource": {"filename": rel}})
        await ed.call("attach_script", entity="Clip", path="scripts/clip_length.lua")
        s = await ed.call("save_project", untitled_scene_dir="scenes")
        await ed.call("edit_build_scenes", action="set_start", scene=s["scenes_in_build"][0]["id"])
        await ed.call("set_project_settings", values={"graphics.runtime_shaders": False, "preferences.build_jobs": JOBS, "player.development_build": True})

        async def build(name: str) -> tuple[Path, dict]:
            out = ed.scratch.root / name
            r = await ed.call("build_project", mode="desktop", output_dir=str(out), method="prebuilt", timeout_seconds=600)
            job = r if r["status"] != "running" else await wait_job(ed, r["job_id"], TIMEOUT - 300, poll=5.0)
            ed.check_eq(job["status"], "succeeded", f"{job}")
            return out, job

        plain, _ = await build("audio-plain")
        ed.check(not list(plain.rglob("je-audio-imports.json")), "no settings: no table")
        raw = await ed.call_raw("run_build", {"seconds": 10, "screenshot": False})
        ed.check("CLIPLEN 1.000" in "\n".join(raw.value["log"]), f"the whole clip: {raw.value['log'][-20:]}")

        await ed.call("set_audio_import_settings", paths=[rel], settings={"trim_head_ms": 250, "gain_db": -6})
        out, _ = await build("audio-table")
        tables = list(out.rglob("je-audio-imports.json"))
        ed.check_eq(len(tables), 1, "the table is in the build")
        table = json.loads(tables[0].read_text())
        ed.check_eq((table["format"], table["version"]), ("jasperengine.audio-imports", 1), "its format")
        # relative to the assets folder, which a new project's root is
        clip = table["clips"][rel]
        ed.check_eq((clip["trim_head_frames"], clip["gain_db"]), (12000, -6), "resolved to frames at the file's rate")
        manifest = json.loads(next(out.rglob("content-manifest.json")).read_text())
        ed.check(any(name.endswith("je-audio-imports.json") for name in manifest["files"]), "listed in the content manifest")
        raw = await ed.call_raw("run_build", {"seconds": 10, "screenshot": False})
        ed.check("CLIPLEN 0.750" in "\n".join(raw.value["log"]), f"the trimmed clip, to the sample: {raw.value['log'][-20:]}")
