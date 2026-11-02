"""S30 build_project mode player, get_build_info players/platforms, install_player_runtime and
run_build's player run options (docs/specs/S30-shipping.md R5, R32, AC3).

A brand-new project (`mcp --create`) is built for Linux with a prebuilt player runtime the test
installs from an archive it makes: the runtime's "player" is /bin/true (an x64 ELF, so the payload
checks pass; the game's content is real: cooked scenes, game.json, content-manifest.json). A
C++ script makes Auto pick Compile and --method prebuilt refuse. run_build runs a stand-in player
script that implements --quit-after / --screenshot as a player does.
"""

import base64
import hashlib
import json
import shutil
import stat
import struct
import zipfile
import zlib
from pathlib import Path

from harness import Editor, record_built_player

CI = True
EDITOR = {"create": True, "name": "Build Method Test"}
RUNTIME = "linux-x64-glcore-release"


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
        "engineVersion": version, "gitSha": "", "contentFormatVersion": "1.0",
        "capacities": {}, "features": ["physics2d", "physics3d", "navigation", "runtimeshaders"], "notices": ["lua"],
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
    ed.check_in("needs a C++ toolchain", msg)
    (ed.project / "cpp" / "Mover.cpp").unlink()
    await ed.call("set_project_settings", values={"scripting.script_dirs": []})
    info = await ed.call("get_build_info")
    ed.check_eq(info["platforms"]["linux"]["method"], "prebuilt")


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
