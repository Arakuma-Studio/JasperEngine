"""S30 build_project mode player, get_build_info players/platforms, install_player_runtime and
run_build's player run options (docs/specs/S30-shipping.md R5, R32, AC3).

A brand-new project (`mcp --create`) is built for Linux with a prebuilt player runtime the test
installs from an archive it makes: the runtime's "player" is /bin/true (an x64 ELF, so the payload
checks pass; the game's content is real: cooked scenes, game.json, content-manifest.json). A
C++ script makes Auto pick Compile and --method prebuilt refuse. run_build runs a stand-in player
script that implements --quit-after / --screenshot as a player does.
"""

import asyncio
import base64
import gzip
import hashlib
import json
import os
import plistlib
import re
import shutil
import stat
import struct
import subprocess
import zipfile
import zlib
from pathlib import Path

import pbxproj
from audio_refs import write_tone
from harness import Editor, editor_command, record_built_player

CI = True
EDITOR = {"create": True, "name": "Build Method Test"}
RUNTIME = "linux-x64-glcore-release"
REPO = Path(__file__).resolve().parents[3]


def content_format() -> str:
    """The newest content format this tree's player reads (CONTENT_MINOR: a Build Profile's build needs
    more than the cooked files' FORMAT_MINOR), as tools/players/package_player.py stamps a runtime."""
    text = (REPO / "engine/core/serialization/CookedScene.h").read_text(encoding="utf-8")
    major = re.search(r"FORMAT_MAJOR = (\d+)", text).group(1)
    minor = re.search(r"CONTENT_MINOR = (\d+)", text).group(1)
    return f"{major}.{minor}"


def png_bytes(width: int, height: int, rgb=(200, 40, 40)) -> bytes:
    rows = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def runtime_archive(path: Path, version: str) -> Path:
    binary = Path("/bin/true").read_bytes()
    info = {
        "schemaVersion": 1, "id": RUNTIME, "platform": "linux", "arch": "x64", "backend": "glcore", "flavour": "release",
        "engineVersion": version, "gitSha": "", "contentFormatVersion": content_format(), "modelCookFormats": [1],
        "capacities": {}, "features": ["physics2d", "physics3d", "navigation", "runtimeshaders", "soundlayer"], "notices": ["lua"],
        "executables": {"files/jasperengine-player": hashlib.sha256(binary).hexdigest()}, "executable": "jasperengine-player",
    }
    with zipfile.ZipFile(path, "w") as zipped:
        zipped.writestr("player-info.json", json.dumps(info))
        zipped.writestr("files/jasperengine-player", binary)
        zipped.writestr("licenses/lua-LICENSE", "MIT\n")
        zipped.writestr("THIRD-PARTY-NOTICES.txt", "Lua (MIT)\n")
    return path


async def test_build_info_platforms(ed: Editor):
    info = await ed.call("get_build_info")
    ed.check(info["editor_version"], "editor_version")
    ids = {p["id"]: p for p in info["players"]}
    ed.check_in(RUNTIME, ids)
    ed.check_eq(ids[RUNTIME]["installed"], False)
    ed.check_in("web-wasm32-gles3-release", ids)
    ed.check_in("android-universal-gles3-development", ids)
    for name in ("windows", "linux", "macos", "android", "ios", "web"):
        ed.check_in(name, info["platforms"])
        ed.check_in(info["platforms"][name]["method"], ("prebuilt", "compile"))
        ed.check(isinstance(info["platforms"][name]["tools_missing"], list), f"{name} tools_missing")
    ed.check(info["platforms"]["ios"]["tools_missing"], "iOS needs a Mac with Xcode here")
    ed.check_eq(info["build_method"], "auto")


async def test_prebuilt_linux_build(ed: Editor):
    await ed.call("create_entity", name="Box", type="box")
    saved = await ed.call("save_project", untitled_scene_dir="scenes")
    scene = saved["scenes_in_build"][0]
    await ed.call("edit_build_scenes", action="set_start", scene=scene["id"])

    info = await ed.call("get_build_info")
    archive = runtime_archive(ed.scratch.root / f"jasperengine-player-{RUNTIME}.zip", info["editor_version"])
    r = await ed.call("install_player_runtime", archive=str(archive), timeout_seconds=120)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    ed.check_eq(r["result"]["id"], RUNTIME)
    ed.check(r["result"]["warnings"], "a development editor warns that it has no release hashes")
    info = await ed.call("get_build_info")
    ed.check({p["id"]: p for p in info["players"]}[RUNTIME]["installed"], "installed")
    ed.check_eq(info["platforms"]["linux"]["method"], "prebuilt")

    out = ed.scratch.root / "game-linux"
    r = await ed.call("build_project", mode="player", platform="linux", output_dir=str(out), timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    result = r["result"]
    # S99 R13, R14: an estimate at submit; the report's fallbacks are the downgrades the build took:
    # here at most the bigger player variant, exactly when the build warned W-PLAYER-VARIANT
    ed.check(r.get("estimate", {}).get("seconds", 0) > 0, f"an estimate: {r.get('estimate')}")
    fallbacks = result.get("report", {}).get("fallbacks")
    variant = any(isinstance(w, dict) and w.get("code") == "W-PLAYER-VARIANT" for w in result.get("warnings", []))
    ed.check_eq([f["step"] for f in fallbacks or []], ["player"] if variant else [], f"a report: {result.get('report')}")
    ed.check_eq(result["method_used"], "prebuilt")
    ed.check_eq(result["player_id"], RUNTIME)
    ed.check_eq(result["compile_reasons"], [])
    manifest = json.loads(Path(result["content_manifest"]).read_text())
    ed.check_eq(manifest["playerId"], RUNTIME)
    ed.check_eq(manifest["method"], "prebuilt")
    ed.check_eq(manifest["startScene"], scene["id"])
    ed.check((out / "game.json").is_file(), "game.json")
    ed.check((out / "scenes" / f"{scene['id']}.jscn").is_file(), "the cooked start scene")
    ed.check((out / "licenses" / "lua-LICENSE").is_file(), "the player's notices")
    exe = Path(result["executable"])
    ed.check(exe.is_file() and exe.stat().st_mode & stat.S_IXUSR, f"executable {exe}")
    ed.check(not any(p.name.startswith(out.name + ".staging-") for p in out.parent.iterdir()), "no staging folder left")
    # a second build replaces the first one whole
    (out / "stale.txt").write_text("from before")
    r = await ed.call("build_project", mode="player", platform="linux", output_dir=str(out), timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    ed.check(not (out / "stale.txt").exists(), "the previous build is replaced, not merged")

    await ed.check_error("build_project", mode="player", platform="linux", output_dir=str(out), graphics_api="d3d11",
                         contains="opengl")
    await ed.check_error("build_project", mode="player", platform="linux", output_dir=str(out), method="fast", contains="method")


async def test_ios_simulator_parameter(ed: Editor):
    """S47: an agent can ask for the simulator .app. Here (no iOS runtime, no Mac) the build says what is missing."""
    ed.check_eq(ed.tool("build_project").input_schema["properties"]["ios_simulator"]["type"], "boolean")
    raw = await ed.call_raw("build_project", {"mode": "player", "platform": "ios", "ios_simulator": True, "method": "prebuilt",
                                              "output_dir": str(ed.scratch.root / "game-ios"), "timeout_seconds": 120}, timeout=180)
    r = raw.value
    if isinstance(r, dict) and r.get("status") == "running":
        r = await ed.call("get_job", job_id=r["job_id"], wait_seconds=120)
    ed.check_in("ios-arm64-metal-release", json.dumps(r) if isinstance(r, dict) else raw.text)
    # (runs after test_prebuilt_linux_build: a build needs a saved scene first)


async def test_prebuilt_build_hears_the_projects_mixer_and_audio_settings(ed: Editor):
    """A prebuilt game sounds as Play does: Play applies Project Settings > Audio and the project's
    mixer, a Compile build's generated init does the same; game.json carries both to the player
    (the mixer by its asset-relative path, which the build ships). A clip's Load Type travels in
    je-audio-imports.json and the Unused Clip Cache in game.json (S97 REQ-10, REQ-17)."""
    r = await ed.call_raw("audio_mixer", {"action": "edit", "ops": [
        {"op": "add_group", "name": "Music"},
        {"op": "add_effect", "group": "Music", "type": "lowpass", "params": {"cutoff": 800}},
    ]})
    ed.check(not r.is_error, f"the project's mixer: {r.text[:300]}")
    await ed.call("set_project_settings", values={"audio.max_real_voices": 20, "audio.max_voices": 100, "audio.sample_rate": 48000,
                                                  "audio.unused_clip_cache": 8})
    write_tone(ed.project / "sounds" / "rain.wav", seconds=0.5)
    await ed.call("sync_files", paths=["sounds/rain.wav"])
    await ed.call("set_audio_import_settings", paths=["sounds/rain.wav"], settings={"load_type": "streaming"})
    await ed.call("save_project", untitled_scene_dir="scenes")
    played = (await ed.call("get_project_settings", keys=["audio.mixer", "audio.max_real_voices", "audio.max_voices",
                                                           "audio.sample_rate", "audio.unused_clip_cache"]))["settings"]
    print(f"[audio] Play's settings: {played}")

    out = ed.scratch.root / "game-linux-audio"
    r = await ed.call("build_project", mode="player", platform="linux", output_dir=str(out), timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    ed.check_eq(r["result"]["method_used"], "prebuilt")
    game = json.loads((out / "game.json").read_text())
    audio = game.get("audio")
    print(f"[audio] game.json audio: {audio}")
    ed.check(isinstance(audio, dict), f"game.json has the audio settings: {sorted(game)}")
    ed.check_eq((audio["maxRealVoices"], audio["maxVoices"], audio["sampleRate"], audio["unusedClipCacheMb"]),
                (played["audio.max_real_voices"], played["audio.max_voices"], 48000, 8), "Project Settings > Audio, as Play has them")
    tables = list(out.rglob("je-audio-imports.json"))
    ed.check_eq(len(tables), 1, "the clip's import settings ship")
    ed.check_eq(json.loads(tables[0].read_text())["clips"]["sounds/rain.wav"], {"load_type": "streaming"}, "its Load Type")
    ed.check(played["audio.mixer"].endswith(audio["mixer"]), f"the mixer Play loads ({played['audio.mixer']}): {audio['mixer']}")
    shipped = out / "assets" / audio["mixer"]
    ed.check(shipped.is_file(), f"the mixer ships at {shipped}")
    ed.check_eq(shipped.read_text(), (ed.project / played["audio.mixer"]).read_text(), "the project's mixer, as Play reads it")


async def test_native_code_compiles(ed: Editor):
    """AC3 through MCP: C++ scripts make Auto compile, and --method prebuilt lists why it cannot."""
    (ed.project / "cpp").mkdir(exist_ok=True)
    (ed.project / "cpp" / "Mover.cpp").write_text("int mover() { return 1; }\n")
    await ed.call("set_project_settings", values={"scripting.script_dirs": ["cpp"]})
    info = await ed.call("get_build_info")
    linux = info["platforms"]["linux"]
    ed.check_eq(linux["method"], "compile")
    ed.check(any(r.startswith("C++ scripts (") and "Mover.cpp" in r for r in linux["compile_reasons"]), f"{linux}")
    msg = await ed.call("build_project", mode="player", platform="linux", method="prebuilt",
                        output_dir=str(ed.scratch.root / "never"), timeout_seconds=300, expect_error=True)
    ed.check_in("cannot use a prebuilt player", msg)
    ed.check_in("C++ scripts", msg)
    # a platform this computer cannot compile for says how to build it instead
    msg = await ed.call("build_project", mode="player", platform="android", output_dir=str(ed.scratch.root / "never-android"),
                        timeout_seconds=300, expect_error=True)
    ed.check_in("needs to compile native code", msg)
    ed.check_in("Export source, then Android Studio", msg)
    (ed.project / "cpp" / "Mover.cpp").unlink()
    await ed.call("set_project_settings", values={"scripting.script_dirs": []})
    info = await ed.call("get_build_info")
    ed.check_eq(info["platforms"]["linux"]["method"], "prebuilt")


# a full runtime's modules: the mixer test leaves the project a clip with a Load Type, which needs the sound layer
FEATURES = ["physics2d", "physics3d", "navigation", "runtimeshaders", "soundlayer"]


def fake_runtime(path: Path, version: str, runtime: str, platform: str, arch: str, backend: str, files: dict[str, bytes],
                 extra_features: list[str] | None = None) -> Path:
    """A runtime archive of another platform, made from files: enough for the build to stage it
    (nothing in it runs here)."""
    info = {
        "schemaVersion": 1, "id": runtime, "platform": platform, "arch": arch, "backend": backend, "flavour": "release",
        "engineVersion": version, "gitSha": "", "contentFormatVersion": content_format(), "capacities": {},
        "features": FEATURES + (extra_features or []), "notices": [], "executables": {}, "executable": "",
    }
    with zipfile.ZipFile(path, "w") as zipped:
        zipped.writestr("player-info.json", json.dumps(info))
        zipped.writestr("licenses/lua-LICENSE", "MIT\n")
        zipped.writestr("THIRD-PARTY-NOTICES.txt", "Lua (MIT)\n")
        for name, data in files.items():
            zipped.writestr(name, data)
    return path


async def install(ed: Editor, archive: Path) -> None:
    r = await ed.call("install_player_runtime", archive=str(archive), timeout_seconds=120)
    ed.check_eq(r["status"], "succeeded", f"{r}")


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


async def test_prebuilt_refuses_to_drop_cpp_plugins(ed: Editor):
    """mobile-shipping: plugins/native/cpp is C++ the player does not hold. Before, the prebuilt
    Linux build shipped without it (the C++ scan read the script folders only)."""
    write(ed.project / "plugins" / "native" / "cpp" / "SteamPlugin.cpp", "int steamPlugin;\n")
    try:
        info = await ed.call("get_build_info")
        linux = info["platforms"]["linux"]
        ed.check_eq(linux["method"], "compile")
        ed.check_in("C++ native plugins (plugins/native/cpp/SteamPlugin.cpp)", linux["compile_reasons"])
        msg = await ed.call("build_project", mode="player", platform="linux", method="prebuilt",
                            output_dir=str(ed.scratch.root / "never-native"), timeout_seconds=300, expect_error=True)
        ed.check_in("cannot use a prebuilt player", msg)
        ed.check_in("plugins/native/cpp/SteamPlugin.cpp", msg)
    finally:
        shutil.rmtree(ed.project / "plugins", ignore_errors=True)
    ed.check_eq((await ed.call("get_build_info"))["platforms"]["linux"]["method"], "prebuilt")


async def test_prebuilt_web_carries_plugins(ed: Editor):
    """mobile-shipping: the prebuilt web page loads web.script_urls and plugins/native/web/*.js as
    the Compile build's does (before: both were left out)."""
    await ed.call("save_project", untitled_scene_dir="scenes")
    info = await ed.call("get_build_info")
    page = ("<!doctype html><html><head><title>{{JASPERENGINE_TITLE}}</title>{{JASPERENGINE_HEAD_INCLUDE}}</head>"
            "<body><canvas id=\"canvas\"></canvas>{{JASPERENGINE_LOADER}}</body></html>\n")
    await install(ed, fake_runtime(ed.scratch.root / "web-runtime.zip", info["editor_version"], "web-wasm32-gles3-release", "web", "wasm32",
                                   "gles3", {"files/index.html": page.encode(), "files/player-loader.js": b"// loader\n",
                                             "files/jasperengine-player-st.js": b"// player\n",
                                             "files/jasperengine-player-st.wasm": b"\0asm\x01\0\0\0"}))
    write(ed.project / "plugins" / "native" / "web" / "poki.js", "JasperNative.register('Poki', {start: function () {}});\n")
    sdk = "https://game-cdn.poki.com/scripts/v2/poki-sdk.js"
    await ed.call("set_project_settings", values={"web.script_urls": [sdk]})
    try:
        platform = (await ed.call("get_build_info"))["platforms"]["web"]
        ed.check_eq(platform["method"], "prebuilt", f"{platform}")
        ed.check(any("plugins/native/web/poki.js" in item for item in platform["carried"]), f"{platform['carried']}")
        out = ed.scratch.root / "game-web"
        r = await ed.call("build_project", mode="player", platform="web", output_dir=str(out), timeout_seconds=600)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        ed.check_eq(r["result"]["method_used"], "prebuilt")
        ed.check(any("web.script_urls" in item for item in r["result"]["carried"]), f"{r['result']['carried']}")
        html = (out / "index.html").read_text()
        stub, sdk_at, plugin, loader = (html.find("window.JasperNative"), html.find(f'<script src="{sdk}">'),
                                        html.find('<script src="plugins/poki.js">'), html.find("player-loader.js"))
        ed.check(-1 not in (stub, sdk_at, plugin, loader), f"every script in the page: {html}")
        ed.check(stub < sdk_at < plugin < loader, "the stub, the SDK, the plugin, then the engine")
        ed.check_eq((out / "plugins" / "poki.js").read_text(), (ed.project / "plugins" / "native" / "web" / "poki.js").read_text())
    finally:
        await ed.call("set_project_settings", values={"web.script_urls": []})
        shutil.rmtree(ed.project / "plugins", ignore_errors=True)


def web_fetched_gzip(out: Path) -> int:
    """gzip -9 of what a browser fetches of a web build: not the notices, manifests or report."""
    skipped = {"build-report.json", "content-manifest.json", "_headers", "THIRD-PARTY-NOTICES.txt"}
    return sum(len(gzip.compress(p.read_bytes(), 9)) for p in out.rglob("*")
               if p.is_file() and p.name not in skipped and "licenses" not in p.relative_to(out).parts)


async def test_build_sizes_and_download_budget(ed: Editor):
    """S99 R10, R11, AC3: build_project's result says what the build weighs (a web build also its
    gzip download estimate); a build over its platform group's Download Budget still succeeds,
    with W-BUILD-OVER-BUDGET and a next naming the tools that inspect its largest category. The
    CLI's build --json has the same fields. (The Linux and web runtimes of the tests above are
    installed.)"""
    for platform in ("linux", "web"):
        out = ed.scratch.root / f"sized-{platform}"
        r = await ed.call("build_project", mode="player", platform=platform, output_dir=str(out), timeout_seconds=600)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        result = r["result"]
        ed.check_eq(Path(result["build_report"]), out / "build-report.json")
        report = json.loads((out / "build-report.json").read_text())
        ed.check(result["build_bytes"] > 0, f"{platform} build_bytes {result['build_bytes']}")
        ed.check_eq(result["build_bytes"], report["total_bytes"])
        ed.check_eq(result["build_categories"], report["categories"])
        ed.check_eq(result["top_files"], [{"path": f["path"], "bytes": f["bytes"]} for f in report["largest"][:5]])
        ed.check(0 < len(result["top_files"]) <= 5, f"{result['top_files']}")
        if platform == "web":
            expected = web_fetched_gzip(out)
            ed.check(abs(result["download_bytes_gzip"] - expected) <= expected * 0.05 + 64,
                     f"download_bytes_gzip {result['download_bytes_gzip']}, gzip -9 of the fetched files {expected}")
            ed.check_eq(report["download_bytes_gzip"], result["download_bytes_gzip"])
        else:
            ed.check("download_bytes_gzip" not in result, "only a web build has a download estimate")
        ed.check("download_budget" not in result and "W-BUILD-OVER-BUDGET" not in warning_codes(r), "no budget, no verdict")

    await ed.call("set_project_settings", values={"player.download_budget_mb.web": 0.001})
    try:
        out = ed.scratch.root / "sized-web"
        r = await ed.call("build_project", mode="player", platform="web", output_dir=str(out), timeout_seconds=600)
        ed.check_eq(r["status"], "succeeded", f"over budget still builds: {r}")
        result = r["result"]
        ed.check_in("W-BUILD-OVER-BUDGET", warning_codes(r))
        message = next(w["message"] for w in result["warnings"] if w.get("code") == "W-BUILD-OVER-BUDGET")
        ed.check_in("over its Download Budget of 1.0 KB (player.download_budget_mb.web)", message)
        ed.check_in("largest categories: ", message)
        budget = result["download_budget"]
        ed.check_eq(budget["budget_bytes"], 1049)
        ed.check_eq(budget["actual_bytes"], result["download_bytes_gzip"])
        ed.check_eq(budget["over"], True)
        largest = max(result["build_categories"].items(), key=lambda kv: kv[1])
        ed.check_eq(budget["largest_categories"][0], {"category": largest[0], "bytes": largest[1]})
        ed.check(len(budget["largest_categories"]) <= 3, f"{budget}")
        # the next names tools that exist, for the largest category
        inspect = {"textures": "get_texture_cook_report", "models": "inspect_models", "engine": "player.strip_engine_code"}.get(largest[0], "list_assets")
        ed.check_in("Over the Download Budget", r.get("next", ""))
        ed.check_in(inspect, r["next"])
        names = {t.name for t in await ed.tools()}
        for tool in re.findall(r"\b([a-z]+(?:_[a-z]+)+) \{", r["next"]):
            ed.check_in(tool, names)

        # another platform group's budget does not hold a Linux build
        r = await ed.call("build_project", mode="player", platform="linux", output_dir=str(ed.scratch.root / "sized-linux"), timeout_seconds=600)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        ed.check("W-BUILD-OVER-BUDGET" not in warning_codes(r) and "download_budget" not in r["result"], f"{r['result']}")

        # the CLI's build --json, with the editor's environment (its HOME holds the installed runtimes)
        env = {k: v for k, v in os.environ.items() if not k.startswith("XDG_") and k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env["HOME"] = str(ed.scratch.home)
        cli_out = ed.scratch.root / "cli-web"
        p = await asyncio.to_thread(subprocess.run, [*editor_command(), "build", "--project", str(ed.project), "--platform", "web",
                                                     "--out", str(cli_out), "--json"],
                                    cwd=ed.scratch.root, env=env, capture_output=True, text=True, timeout=600)
        ed.check_eq(p.returncode, 0, f"{p.stdout[-2000:]}\n{p.stderr[-2000:]}")
        cli = json.loads(p.stdout[p.stdout.index("{"):])
        ed.check_eq(Path(cli["build_report"]), cli_out / "build-report.json")
        ed.check(cli["build_bytes"] > 0 and cli["top_files"] and cli["download_bytes_gzip"] > 0, f"{cli}")
        ed.check_eq(cli["download_budget"]["budget_bytes"], 1049)
        ed.check_eq(cli["download_budget"]["over"], True)
        ed.check_in("W-BUILD-OVER-BUDGET", [w["code"] for w in cli["warnings"]])
    finally:
        await ed.call("set_project_settings", values={"player.download_budget_mb.web": 0})


async def install_ios_runtime(ed: Editor, name: str, with_manifest: bool = True) -> None:
    """A fake iOS runtime of the committed player template; without_manifest: one packaged before
    privacy manifests (no PrivacyInfo.xcprivacy in the template or its project)."""
    info = await ed.call("get_build_info")
    template = REPO / "engine" / "platform" / "apple" / "ios" / "player-template"
    files = {f"ios/template/{p.relative_to(template).as_posix()}": p.read_bytes() for p in template.rglob("*") if p.is_file()}
    files["ios/template/include/JasperNative.h"] = (REPO / "engine" / "platform" / "apple" / "JasperNative.h").read_bytes()
    files["ios/JasperEnginePlayer.xcframework/Info.plist"] = b"<plist version=\"1.0\"><dict/></plist>\n"
    if not with_manifest:
        del files["ios/template/PrivacyInfo.xcprivacy"]
        pbx_key = "ios/template/JasperEnginePlayer.xcodeproj/project.pbxproj"
        files[pbx_key] = b"".join(line for line in files[pbx_key].splitlines(keepends=True) if b"PrivacyInfo.xcprivacy" not in line)
    await install(ed, fake_runtime(ed.scratch.root / name, info["editor_version"], "ios-arm64-metal-release", "ios", "arm64",
                                   "metal", files, ["nativeplugins"]))


async def build_ios(ed: Editor, name: str) -> tuple[dict, Path]:
    """A prebuilt iOS build; with no Xcode here, the staged Xcode project is the result."""
    platform = (await ed.call("get_build_info"))["platforms"]["ios"]
    ed.check_eq(platform["method"], "prebuilt", f"{platform}")
    out = ed.scratch.root / name
    r = await ed.call("build_project", mode="player", platform="ios", output_dir=str(out), timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    project_dirs = [p for p in out.iterdir() if (p / "JasperEnginePlayer.xcodeproj").is_dir()]
    ed.check_eq(len(project_dirs), 1, f"{list(out.iterdir())}")
    return r, project_dirs[0]


def warning_codes(r: dict) -> list:
    return [w["code"] for w in r["result"].get("warnings", []) if isinstance(w, dict) and "code" in w]


async def test_prebuilt_ios_carries_plugins(ed: Editor):
    """mobile-shipping: the prebuilt iOS build is an Xcode project, so the game's Objective-C
    plugins, SDK resource bundles and Info.plist entries go into it (before: the build compiled,
    which an iOS build cannot do here). No privacy keys are set: the plugins alone decide."""
    await ed.call("save_project", untitled_scene_dir="scenes")
    await install_ios_runtime(ed, "ios-runtime.zip")
    write(ed.project / "plugins" / "native" / "ios" / "Device.h", "#import \"JasperNative.h\"\n@interface Device : NSObject <JasperNativePlugin>\n@end\n")
    write(ed.project / "plugins" / "native" / "ios" / "Device.m", "#import \"Device.h\"\n@implementation Device\n@end\n")
    bundle = ed.project / "plugins" / "native" / "ios" / "sdk" / "Ads_Privacy.bundle" / "PrivacyInfo.xcprivacy"
    bundle.parent.mkdir(parents=True, exist_ok=True)
    bundle.write_bytes(plistlib.dumps({"NSPrivacyAccessedAPITypes": [{"NSPrivacyAccessedAPIType": "NSPrivacyAccessedAPICategoryDiskSpace",
                                                                     "NSPrivacyAccessedAPITypeReasons": ["E174.1"]}]}))
    await ed.call("set_project_settings", values={"ios.plist_entries": {"NSCameraUsageDescription": "Scans codes"}})
    try:
        r, staged = await build_ios(ed, "game-ios")
        root = pbxproj.parse((staged / "JasperEnginePlayer.xcodeproj" / "project.pbxproj").read_text())
        ed.check_eq(pbxproj.dangling_references(root), [])
        ed.check_in("Device.m", [Path(s).name for s in pbxproj.target_sources(root, "JasperEnginePlayer")])
        for settings in pbxproj.target_settings(root, "JasperEnginePlayer"):
            ed.check_in("$(SRCROOT)/include", settings["HEADER_SEARCH_PATHS"], "the plugin finds JasperNative.h")
        ed.check((staged / "plugins" / "native" / "ios" / "Device.m").is_file(), "the plugin next to the project")
        objects = root["objects"]
        resources = [objects[objects[b]["fileRef"]].get("path") for p in pbxproj.target(root, "JasperEnginePlayer")["buildPhases"]
                     if objects[p]["isa"] == "PBXResourcesBuildPhase" for b in objects[p]["files"]]
        ed.check_in("Ads_Privacy.bundle", resources, "the SDK's resource bundle is copied into the app")
        ed.check((staged / "plugins" / "native" / "ios" / "sdk" / "Ads_Privacy.bundle" / "PrivacyInfo.xcprivacy").is_file())
        ed.check_in("<key>NSCameraUsageDescription</key>", (staged / "Info.plist").read_text())
        privacy = plistlib.loads((staged / "PrivacyInfo.xcprivacy").read_bytes())
        apis = [a["NSPrivacyAccessedAPIType"] for a in privacy["NSPrivacyAccessedAPITypes"]]
        ed.check_in("NSPrivacyAccessedAPICategoryUserDefaults", apis, "the engine's entries")
        ed.check_in("NSPrivacyAccessedAPICategoryDiskSpace", apis, "the bundle's manifest is merged")
        ed.check("W-IOS-PRIVACY-RUNTIME" not in warning_codes(r), "the runtime's template has the manifest")
    finally:
        await ed.call("set_project_settings", values={"ios.plist_entries": {}})
        shutil.rmtree(ed.project / "plugins", ignore_errors=True)


async def test_prebuilt_ios_old_runtime_privacy(ed: Editor):
    """mobile-shipping: an iOS runtime packaged before privacy manifests still ships one: the
    build adds the file and its resource, and warns W-IOS-PRIVACY-RUNTIME. Tracking without
    NSUserTrackingUsageDescription is W-PRIVACY-ATT."""
    await ed.call("save_project", untitled_scene_dir="scenes")
    await install_ios_runtime(ed, "ios-runtime-old.zip", with_manifest=False)
    await ed.call("set_project_settings", values={"apple.privacy_tracking": True, "apple.privacy_tracking_domains": ["ads.example.com"]})
    try:
        r, staged = await build_ios(ed, "game-ios-old")
        codes = warning_codes(r)
        ed.check_in("W-IOS-PRIVACY-RUNTIME", codes, "the old runtime is named")
        ed.check_in("W-PRIVACY-ATT", codes, "tracking without NSUserTrackingUsageDescription")
        root = pbxproj.parse((staged / "JasperEnginePlayer.xcodeproj" / "project.pbxproj").read_text())
        ed.check_eq(pbxproj.dangling_references(root), [])
        objects = root["objects"]
        resources = [objects[objects[b]["fileRef"]].get("path") for p in pbxproj.target(root, "JasperEnginePlayer")["buildPhases"]
                     if objects[p]["isa"] == "PBXResourcesBuildPhase" for b in objects[p]["files"]]
        ed.check_eq(resources.count("PrivacyInfo.xcprivacy"), 1, f"{resources}")
        privacy = plistlib.loads((staged / "PrivacyInfo.xcprivacy").read_bytes())
        ed.check_eq(privacy["NSPrivacyTracking"], True)
        ed.check_eq(privacy["NSPrivacyTrackingDomains"], ["ads.example.com"])
    finally:
        await ed.call("set_project_settings", values={"apple.privacy_tracking": False, "apple.privacy_tracking_domains": []})
        await install_ios_runtime(ed, "ios-runtime.zip")


async def test_prebuilt_mock_warning(ed: Editor):
    """mobile-shipping: a Lua mock that would stand in for an Android-only plugin in a Linux game is
    a build warning, and game.json tells the player the plugin is Android's."""
    await ed.call("save_project", untitled_scene_dir="scenes")
    write(ed.project / "plugins" / "native" / "android" / "java" / "com" / "me" / "Ads.java", "package com.me;\npublic class Ads {}\n")
    lua_dir = (await ed.call("get_project_settings", keys=["scripting.lua_dir"]))["settings"]["scripting.lua_dir"] or "lua"
    write(ed.project / lua_dir / "native_mocks" / "Ads.lua", "return {show = function() return true end}\n")
    try:
        out = ed.scratch.root / "game-linux-mock"
        r = await ed.call("build_project", mode="player", platform="linux", output_dir=str(out), timeout_seconds=600)
        ed.check_eq(r["status"], "succeeded", f"{r}")
        ed.check_eq(r["result"]["method_used"], "prebuilt")
        warnings = [w for w in r["result"].get("warnings", []) if isinstance(w, dict) and w.get("code") == "W-NATIVE-MOCK"]
        ed.check_eq(len(warnings), 1, f"{r['result'].get('warnings')}")
        ed.check_in("native_mocks/Ads.lua", warnings[0]["message"])
        game = json.loads((out / "game.json").read_text())
        ed.check_eq(game.get("nativePlatformOnly"), {"Ads": "Java/Kotlin plugins run only on Android"})
    finally:
        shutil.rmtree(ed.project / "plugins", ignore_errors=True)
        shutil.rmtree(ed.project / lua_dir / "native_mocks", ignore_errors=True)


async def test_run_build_player_options(ed: Editor):
    """run_build's quit_after / fixed_fps / after_ready / headless / screenshot_frame reach the
    player, which writes its own frame: the result's screenshot is that file."""
    bin_dir = ed.scratch.root / "stand-in-player"
    bin_dir.mkdir(parents=True, exist_ok=True)
    png = base64.b64encode(png_bytes(320, 200)).decode()
    script = bin_dir / "player"
    script.write_text("#!/bin/sh\n"
                      'echo "args: $*"\n'
                      'shot=""\n'
                      'while [ $# -gt 0 ]; do case "$1" in --screenshot) shot="$2"; shift 2 ;; *) shift ;; esac; done\n'
                      f'[ -n "$shot" ] && echo "{png}" | base64 -d > "$shot"\n'
                      'echo "Engine ready: ok"\nexit 0\n')
    script.chmod(script.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    record_built_player(ed.project, script)   # run_build runs only the recorded build (BuiltPlayers)
    raw = await ed.call_raw("run_build", {"path": str(script), "quit_after": 30, "fixed_fps": 60, "after_ready": True,
                                          "headless": True, "screenshot_frame": 10, "max_width": 160})
    ed.check(not raw.is_error, raw.text)
    r = raw.value
    log = " ".join(r["log"])
    for flag in ("--quit-after 30", "--fixed-fps 60", "--after-ready", "--headless", "--screenshot-frame 10", "--screenshot "):
        ed.check_in(flag, log)
    ed.check_eq(r["exit_code"], 0)
    ed.check(not r["crashed"], f"{r}")
    ed.check(raw.images, "the player's own screenshot")
    ed.check_in("player's frame", r["screenshot"]["note"])
    ed.check_eq(r["screenshot"]["width"], 160, "scaled to max_width")
    data, mime = raw.images[0]
    ed.check_eq(mime, "image/png")
