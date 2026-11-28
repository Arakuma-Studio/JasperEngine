"""Build And Run on a device (docs/specs/build-and-run-on-device.md AC3-AC5, AC8, AC13).

No device and no Android SDK are needed: the editor finds stand-ins first. A fake SDK (ANDROID_HOME)
holds build-tools whose aapt2, zipalign and apksigner write a placeholder APK (the packaging itself
is covered by the AndroidPackage.* unit tests), a fake JDK (JAVA_HOME) whose keytool makes the debug
keystore, and platform-tools/adb, a stand-in that records its calls (FAKE_ADB_DIR/calls.jsonl) and
serves canned `devices -l`, getprop, `pm path`, pidof, screencap and logcat output for one emulator.
The Android player runtime is a fake development runtime (the real manifest template, a fake
16 KB-aligned x86_64 library). The web build uses fake web runtimes and the editor's own server.
"""

import asyncio
import hashlib
import json
import os
import shutil
import struct
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from harness import Editor  # noqa: E402
from test_build_method import content_format, png_bytes, FEATURES  # noqa: E402

CI = True
REPO = Path(__file__).resolve().parents[3]
DEVICE = "emulator-5554"
MODEL = "Pixel 7"
RUNTIME = "android-universal-gles3-development"

ROOT = Path(tempfile.mkdtemp(prefix="jasper-device-run-"))
SDK = ROOT / "sdk"
JDK = ROOT / "jdk"
ADB_DIR = ROOT / "adb"
HOME = ROOT / "home"

FAKE_ADB = r'''#!/usr/bin/env python3
import json, os, shutil, sys, time
state = os.environ["FAKE_ADB_DIR"]
args = sys.argv[1:]
with open(os.path.join(state, "calls.jsonl"), "a") as log:
    log.write(json.dumps(args) + "\n")
def flag(name):
    return os.path.exists(os.path.join(state, name))
serial = None
if args[:1] == ["-s"]:
    serial, args = args[1], args[2:]
if args[:2] == ["devices", "-l"]:
    print("List of devices attached")
    print("emulator-5554          device product:sdk_gphone64_x86_64 model:sdk_gphone64_x86_64 device:emu64xa transport_id:1")
    sys.exit(0)
if args[:1] == ["install"]:
    shutil.copy(args[-1], os.path.join(state, "installed.apk"))
    print("Performing Streamed Install")
    print("Success")
    sys.exit(0)
if args[:1] == ["push"]:
    target = os.path.join(state, "pushed", os.path.basename(args[1].rstrip("/")))
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(args[1], target)
    print("pushed")
    sys.exit(0)
if args[:1] == ["pull"]:
    shutil.copy(os.path.join(state, "screen.png"), args[2])
    sys.exit(0)
if args[:1] == ["logcat"]:
    print("--------- beginning of main")
    print("11-20 10:00:00.100  4242  4250 I JasperEngine: Engine ready: every scene loaded and no shader build pending (frame 3)", flush=True)
    print("11-20 10:00:00.200  4242  4250 W JasperEngine: a warning of the game", flush=True)
    if flag("crash"):
        time.sleep(2.0)
        print("11-20 10:00:02.000  4242  4250 F libc    : Fatal signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x0 in tid 4250", flush=True)
        open(os.path.join(state, "crashed"), "w").close()
        sys.exit(0)
    while True:
        time.sleep(1)
if args[:1] == ["shell"]:
    command = " ".join(args[1:])
    if command.startswith("getprop"):
        print("Pixel 7")
        print("x86_64,arm64-v8a")
        print("34")
    elif command.startswith("pm path"):
        if flag("installed.apk"):
            print("package:/data/app/~~x/base.apk")
    elif command.startswith("pidof"):
        if not flag("installed.apk") or flag("crashed") or flag("stopped"):
            sys.exit(1)
        print("4242")
    elif command.startswith("am start"):
        for name in ("crashed", "stopped"):
            if flag(name):
                os.remove(os.path.join(state, name))
        print("Starting: Intent { cmp=x }")
        print("Status: ok")
        print("TotalTime: 321")
    elif command.startswith("am force-stop"):
        open(os.path.join(state, "stopped"), "w").close()
    elif command.startswith("screencap"):
        pass
    sys.exit(0)
print("adb: unknown command " + " ".join(args), file=sys.stderr)
sys.exit(1)
'''

# aapt2 link: the -A assets (the game's content) and a manifest in a zip at -o; dump badging: nothing
FAKE_AAPT2 = r'''#!/usr/bin/env python3
import os, sys, zipfile
args = sys.argv[1:]
if args[:1] == ["link"]:
    out = args[args.index("-o") + 1]
    assets = args[args.index("-A") + 1]
    with zipfile.ZipFile(out, "w") as z:
        z.writestr("AndroidManifest.xml", open(args[args.index("--manifest") + 1]).read())
        for root, _, files in os.walk(assets):
            for name in files:
                path = os.path.join(root, name)
                z.write(path, "assets/" + os.path.relpath(path, assets))
elif args[:1] == ["compile"]:
    pass
sys.exit(0)
'''
FAKE_ZIPALIGN = "#!/bin/sh\nfor last; do :; done\neval \"src=\\${$(($#-1))}\"\ncp \"$src\" \"$last\"\n"
FAKE_APKSIGNER = r'''#!/usr/bin/env python3
import shutil, sys
args = sys.argv[1:]
if args[:1] == ["sign"]:
    shutil.copy(args[-1], args[args.index("--out") + 1])
sys.exit(0)
'''
FAKE_KEYTOOL = r'''#!/usr/bin/env python3
import sys
args = sys.argv[1:]
if "-genkeypair" in args:
    open(args[args.index("-keystore") + 1], "wb").write(b"fake keystore")
sys.exit(0)
'''


def elf_library() -> bytes:
    """An x86_64 shared library header with one PT_LOAD aligned to 16 KB (what the payload check reads)."""
    header = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
    header += struct.pack("<HHIQQQIHHHHHH", 3, 0x3E, 1, 0, 64, 0, 0, 64, 56, 1, 64, 0, 0)
    load = struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, 120, 120, 16384)
    return header + load


def write_tool(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(0o755)


def make_fakes() -> None:
    bt = SDK / "build-tools" / "35.0.0"
    write_tool(bt / "aapt2", FAKE_AAPT2)
    write_tool(bt / "zipalign", FAKE_ZIPALIGN)
    write_tool(bt / "apksigner", FAKE_APKSIGNER)
    (SDK / "platforms" / "android-36").mkdir(parents=True, exist_ok=True)
    (SDK / "platforms" / "android-36" / "android.jar").write_bytes(b"jar")
    write_tool(SDK / "platform-tools" / "adb", FAKE_ADB)
    write_tool(JDK / "bin" / "keytool", FAKE_KEYTOOL)
    write_tool(JDK / "bin" / "java", "#!/bin/sh\nexit 0\n")
    write_tool(JDK / "bin" / "jarsigner", "#!/bin/sh\nexit 0\n")
    ADB_DIR.mkdir(parents=True, exist_ok=True)
    (ADB_DIR / "screen.png").write_bytes(png_bytes(8, 4, (10, 200, 30)))
    HOME.mkdir(parents=True, exist_ok=True)


make_fakes()
EDITOR = {"create": True, "name": "Device Run",
          "env": {"ANDROID_HOME": str(SDK), "JAVA_HOME": str(JDK), "HOME": str(HOME), "FAKE_ADB_DIR": str(ADB_DIR)}}


def runtime_archive(path: Path, version: str, runtime: str, platform: str, arch: str, backend: str, flavour: str,
                    files: dict[str, bytes]) -> Path:
    info = {
        "schemaVersion": 1, "id": runtime, "platform": platform, "arch": arch, "backend": backend, "flavour": flavour,
        "engineVersion": version, "gitSha": "", "contentFormatVersion": content_format(), "capacities": {},
        "features": FEATURES + (["tracy"] if flavour == "development" else []), "notices": [], "executables": {}, "executable": "",
    }
    with zipfile.ZipFile(path, "w") as zipped:
        zipped.writestr("player-info.json", json.dumps(info))
        zipped.writestr("licenses/lua-LICENSE", "MIT\n")
        zipped.writestr("THIRD-PARTY-NOTICES.txt", "Lua (MIT)\n")
        for name, data in files.items():
            zipped.writestr(name, data)
    return path


def calls() -> list[list[str]]:
    path = ADB_DIR / "calls.jsonl"
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def clear_calls() -> None:
    (ADB_DIR / "calls.jsonl").unlink(missing_ok=True)


def shell_calls(recorded: list[list[str]]) -> list[str]:
    return [" ".join(c[c.index("shell") + 1:]) for c in recorded if "shell" in c]


async def setup_android(ed: Editor) -> None:
    """The fake development runtime installed once, a saved scene and a Lua script"""
    info = await ed.call("get_build_info")
    if not any(p["id"] == RUNTIME and p["installed"] for p in info["players"]):
        template = (REPO / "engine/platform/android/player/AndroidManifest.template.xml").read_bytes()
        res = ROOT / "res-flat.zip"
        with zipfile.ZipFile(res, "w"):
            pass
        archive = runtime_archive(ROOT / "android-runtime.zip", info["editor_version"], RUNTIME, "android", "universal", "gles3",
                                  "development", {
                                      "android/AndroidManifest.template.xml": template,
                                      "android/res-flat.zip": res.read_bytes(),
                                      "android/stable-ids.txt": b"",
                                      "android/classes.dex": b"dex\n035\0",
                                      "android/lib/x86_64/libjasperengine-android.so": elf_library(),
                                  })
        r = await ed.call("install_player_runtime", archive=str(archive), timeout_seconds=120)
        ed.check_eq(r["status"], "succeeded", f"{r}")
    lua = ed.project / "lua" / "Hello.lua"
    if not lua.exists():
        lua.parent.mkdir(parents=True, exist_ok=True)
        lua.write_text("print('hello 1')\n")
        await ed.call("create_entity", name="Box", type="box")
        await ed.call("save_project", untitled_scene_dir="scenes")


async def build_and_run(ed: Editor, **extra) -> dict:
    r = await ed.call("build_project", mode="player", platform="android", development=True, run_device=DEVICE,
                      output_dir=str(ed.scratch.root / "game-android"), timeout_seconds=600, **extra)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    return r


def records(ed: Editor) -> str:
    return (ed.project / ".jasperengine" / "user" / "devices.yaml").read_text()


async def test_build_and_run_android_installs_and_launches(ed: Editor):
    await setup_android(ed)
    clear_calls()
    shutil.rmtree(ADB_DIR / "pushed", ignore_errors=True)
    (ADB_DIR / "installed.apk").unlink(missing_ok=True)
    r = await build_and_run(ed)
    device = r["result"]["device"]
    ed.check_eq(device["installed"], "full")
    ed.check_eq(device["id"], DEVICE)
    ed.check_eq(device["name"], MODEL)
    ed.check_in("read_console", r.get("next", ""), "next points at the device log")
    recorded = calls()
    installs = [c for c in recorded if "install" in c]
    ed.check_eq(len(installs), 1, f"{recorded}")
    ed.check(installs[0][-4:-1] == ["install", "-r", "-t"], f"install -r -t, no -g: {installs[0]}")
    ed.check(installs[0][-1].endswith("-x86_64.apk"), f"one APK of the emulator's ABI: {installs[0]}")
    shells = shell_calls(recorded)
    ed.check(any(s.startswith("run-as ") and "rm -rf" in s and "files/je-dev-content" in s for s in shells), f"clears the folder: {shells}")
    ed.check(any(s.startswith("am start -W -n") and "/com.jaspergameengine.jasperengine.MainActivity" in s for s in shells), f"{shells}")
    # the APK is a debuggable development package of the device's ABI only
    with zipfile.ZipFile(installs[0][-1]) as apk:
        names = apk.namelist()
        manifest = apk.read("AndroidManifest.xml").decode()
    ed.check_in("lib/x86_64/libjasperengine-android.so", names)
    ed.check(not any(n.startswith("lib/arm64-v8a") for n in names), "single ABI")
    ed.check_in('android:debuggable="true"', manifest)
    ed.check_in(DEVICE, records(ed))
    ed.check_in("patch: 0", records(ed))
    # its log reaches the Console, category device
    for _ in range(50):
        entries = await ed.call("read_console", categories=["device"], limit=200)
        if any("Engine ready:" in e["message"] for e in entries.get("entries", [])):
            break
        await asyncio.sleep(0.2)
    ed.check(any(e["message"].startswith(f"[{MODEL}] ") for e in entries.get("entries", [])), f"{entries}")


async def test_patch_and_run_pushes_only_changed_files(ed: Editor):
    await setup_android(ed)
    if "installed.apk" not in os.listdir(ADB_DIR):
        await build_and_run(ed)
    # the first patch after a full install mirrors the whole content (R8): the folder starts empty
    (ed.project / "lua" / "Hello.lua").write_text("print('hello 2')\n")
    first = await build_and_run(ed)
    ed.check_eq(first["result"]["device"]["installed"], "patched")
    mirrored = first["result"]["device"]["pushed_files"]
    ed.check(mirrored > 2, f"the first patch mirrors the content: {first['result']['device']}")
    # one Lua file changed: that file and the manifest
    clear_calls()
    (ed.project / "lua" / "Hello.lua").write_text("print('hello 3')\n")
    r = await build_and_run(ed)
    device = r["result"]["device"]
    ed.check_eq(device["installed"], "patched")
    ed.check_eq(device["pushed_files"], 2, f"{device}")
    recorded = calls()
    ed.check(not any("install" in c for c in recorded), f"no reinstall: {recorded}")
    pushed = sorted(ADB_DIR.glob("pushed/patch-*"))
    ed.check(pushed, "a patch was pushed")
    files = sorted(str(p.relative_to(pushed[-1])) for p in pushed[-1].rglob("*") if p.is_file())
    ed.check_eq(len(files), 2, f"{files}")
    ed.check_eq(files[0], "content-manifest.json")
    ed.check(files[1].startswith("files/") and files[1].endswith("/Hello.lua"), f"{files}")
    ed.check_in("hello 3", (pushed[-1] / files[1]).read_text())
    manifest = json.loads((pushed[-1] / "content-manifest.json").read_text())
    ed.check(manifest.get("patch", 0) >= 2 and manifest.get("patchedAt"), f"the patch counter: {manifest.get('patch')}")
    shells = shell_calls(recorded)
    stop = next(i for i, s in enumerate(shells) if s.startswith("am force-stop"))
    start = next(i for i, s in enumerate(shells) if s.startswith("am start"))
    ed.check(stop < start, f"force-stop, then am start: {shells}")
    copy = next(s for s in shells if s.startswith("run-as ") and "cp -R" in s)
    ed.check(copy.rfind("content-manifest.json") > copy.find("cp -R"), f"the manifest is copied last: {copy}")


async def test_patch_falls_back_to_full_when_the_player_changed(ed: Editor):
    await setup_android(ed)
    if "installed.apk" not in os.listdir(ADB_DIR):
        await build_and_run(ed)
    path = ed.project / ".jasperengine" / "user" / "devices.yaml"
    path.write_text(path.read_text().replace(f"playerId: {RUNTIME}", "playerId: android-universal-vulkan-development"))
    clear_calls()
    r = await build_and_run(ed)
    ed.check_eq(r["result"]["device"]["installed"], "full")
    ed.check_in("the player binary changed", r.get("next", ""))
    ed.check(any("install" in c for c in calls()), "a full install")


async def test_run_build_device_refuses_unrecorded_packages(ed: Editor):
    other = ed.scratch.root / "other.apk"
    other.write_bytes(b"PK\x05\x06" + bytes(18))
    msg = await ed.call("run_build", device=DEVICE, path=str(other), expect_error=True)
    ed.check_in("the package this project built", msg)
    msg = await ed.call("run_build", device=DEVICE, input=[{"type": "key", "key": "space"}], expect_error=True)
    ed.check_in("input is desktop-only", msg)


async def test_run_build_device_reports_crash(ed: Editor):
    await setup_android(ed)
    if "installed.apk" not in os.listdir(ADB_DIR):
        await build_and_run(ed)
    (ADB_DIR / "crash").touch()
    try:
        r = await ed.call_raw("run_build", {"device": DEVICE, "seconds": 8, "screenshot_after_seconds": 0.5})
    finally:
        (ADB_DIR / "crash").unlink(missing_ok=True)
    ed.check(not r.is_error, r.text)
    data = r.value
    ed.check_eq(data["crashed"], True, f"{data}")
    ed.check(any("Fatal signal 11" in line for line in data["fatal_lines"]), f"{data['fatal_lines']}")
    ed.check(r.images, "the screencap PNG as an image")
    ed.check(r.images[0][0].startswith(b"\x89PNG"), "a PNG")
    entries = (await ed.call("read_console", categories=["device"], limit=500))["entries"]
    ed.check(any(e["message"].startswith(f"[{MODEL}] ") for e in entries), "device lines in the Console")
    ed.check(any(e["message"].startswith(f"[{MODEL}] crashed: ") for e in entries), "the run says how it ended")


async def test_build_info_lists_devices(ed: Editor):
    await setup_android(ed)
    info = await ed.call("get_build_info", devices=True)
    devices = {d["id"]: d for d in info["devices"]}
    ed.check_in(DEVICE, devices)
    ed.check_eq(devices[DEVICE]["name"], MODEL)
    ed.check_eq(devices[DEVICE]["kind"], "emulator")
    ed.check_eq(devices[DEVICE]["usable"], True)
    ed.check_eq(devices[DEVICE]["os_version"], "34")
    ed.check_in("web", devices)
    ed.check_eq(devices["web"]["kind"], "browser")
    if "installed.apk" in os.listdir(ADB_DIR):
        ed.check_eq(devices[DEVICE]["installed"]["package"].startswith("com."), True, f"{devices[DEVICE]}")
    ed.check("devices" not in await ed.call("get_build_info"), "only when asked: probing spawns processes")


async def test_web_build_and_run_serves_with_isolation(ed: Editor):
    info = await ed.call("get_build_info")
    page = ("<!doctype html><html><head><title>{{JASPERENGINE_TITLE}}</title>{{JASPERENGINE_HEAD_INCLUDE}}</head>"
            "<body><canvas id=\"canvas\"></canvas>{{JASPERENGINE_LOADER}}</body></html>\n")
    files = {"files/index.html": page.encode(), "files/player-loader.js": b"// loader\n", "files/jasperengine-player-st.js": b"// player\n",
             "files/jasperengine-player-st.wasm": b"\0asm\x01\0\0\0"}
    for flavour in ("release", "development"):
        runtime = f"web-wasm32-gles3-{flavour}"
        if not any(p["id"] == runtime and p["installed"] for p in info["players"]):
            r = await ed.call("install_player_runtime", archive=str(runtime_archive(ROOT / f"{runtime}.zip", info["editor_version"], runtime,
                                                                                    "web", "wasm32", "gles3", flavour, files)),
                              timeout_seconds=120)
            ed.check_eq(r["status"], "succeeded", f"{r}")
    await setup_android(ed)
    out = ed.scratch.root / "game-web"
    r = await ed.call("build_project", mode="web", development=True, run_device="web", output_dir=str(out), timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    url = r["result"]["device"]["url"]
    ed.check(url.startswith("http://127.0.0.1:86") and "editor-port=" in url, url)
    ed.check_in("/__jasper/log", (out / "index.html").read_text(), "a development page carries the reporter")
    base = url.split("/index.html")[0]
    with urllib.request.urlopen(url) as answer:
        ed.check_eq(answer.headers["Cross-Origin-Opener-Policy"], "same-origin")
        ed.check_eq(answer.headers["Cross-Origin-Embedder-Policy"], "require-corp")
        ed.check_eq(answer.headers["Cache-Control"], "no-store")
    with urllib.request.urlopen(base + "/jasperengine-player-st.wasm") as answer:
        ed.check_eq(answer.headers["Content-Type"], "application/wasm")
    try:
        urllib.request.urlopen(base + "/../project.yaml")
        ed.fail("a path out of the build folder is served")
    except urllib.error.HTTPError as e:
        ed.check_eq(e.code, 404)
    # the page's log channel: its own origin only
    body = json.dumps({"lines": [{"level": "log", "text": "page line one"}, {"level": "warn", "text": "page line two"}]}).encode()
    try:
        urllib.request.urlopen(urllib.request.Request(base + "/__jasper/log", body, {"Content-Type": "application/json", "Origin": "http://evil.example"}))
        ed.fail("a post from another origin is taken")
    except urllib.error.HTTPError as e:
        ed.check_eq(e.code, 403)
    with urllib.request.urlopen(urllib.request.Request(base + "/__jasper/log", body, {"Content-Type": "application/json", "Origin": base})) as answer:
        ed.check_in("commands", json.loads(answer.read()))
    for _ in range(50):
        entries = (await ed.call("read_console", categories=["device"], limit=200))["entries"]
        if any(e["message"].endswith("page line two") for e in entries):
            break
        await asyncio.sleep(0.1)
    ed.check(any(e["message"] == "[Default browser] page line one" for e in entries), f"{[e['message'] for e in entries][-10:]}")
    ed.check(any(e["message"] == "[Default browser] page line two" for e in entries), "both lines")
    # a release page has no reporter
    r = await ed.call("build_project", mode="web", output_dir=str(ed.scratch.root / "game-web-release"), timeout_seconds=600)
    ed.check_eq(r["status"], "succeeded", f"{r}")
    ed.check("/__jasper/log" not in (ed.scratch.root / "game-web-release" / "index.html").read_text(), "no reporter in a release page")


async def test_build_settings_run_device_row(ed: Editor):
    await setup_android(ed)
    await ed.call("run_command", id="file.buildSettings")
    dialog = None
    for _ in range(40):
        dialog = (await ed.call("editor_status")).get("dialog")
        if dialog and dialog.get("title") == "Build Settings":
            break
        await asyncio.sleep(0.25)
    ed.check(dialog and dialog.get("title") == "Build Settings", f"Build Settings opens: {dialog}")
    try:
        await ed.call("respond_dialog", values={"platform": "Android"})
        await ed.call("respond_dialog", values={"development_build": False})
        field = None
        for _ in range(60):
            dialog = (await ed.call("editor_status")).get("dialog") or {}
            field = next((f for f in dialog.get("fields", []) if f["name"] == "run_device"), None)
            if field and any(DEVICE in option for option in field.get("options", [])):
                break
            await asyncio.sleep(0.25)
        ed.check(field and any(f"{MODEL} ({DEVICE})" in option for option in field["options"]), f"the Run Device combo lists the device: {field}")
        choices = dialog.get("choices", []) + dialog.get("disabled_choices", [])
        ed.check_in("build_and_run", choices)
        ed.check("patch_and_run" not in choices, f"no Patch And Run without Development Build: {choices}")
        await ed.call("respond_dialog", values={"development_build": True})
        dialog = (await ed.call("editor_status")).get("dialog") or {}
        ed.check_in("patch_and_run", dialog.get("choices", []) + dialog.get("disabled_choices", []))
    finally:
        await ed.call("respond_dialog", values={"development_build": False})
        await ed.call("respond_dialog", choice="cancel")
