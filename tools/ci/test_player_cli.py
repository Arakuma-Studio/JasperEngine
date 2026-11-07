#!/usr/bin/env python3
"""The prebuilt player end to end (docs/specs/S30-shipping.md AC1-AC3, AC5), in the no-compiler
image of docker/runtime.Dockerfile:

    tools/build.sh linux                      # the editor install (build/linux/editor-install)
    tools/build.sh players                    # Linux runtimes; PLAYER_PLATFORM=web for the web ones
    tools/build.sh exec runtime python3 tools/ci/test_player_cli.py [--only linux,components,variants,web,fallback]

- linux (AC1): `jasperengine-editor build --platform linux` of tools/fixtures/unified-lighting
  with each Linux backend whose runtime is installed (--backends), timed (R28: <= 20 s with a warm
  cache, reported); the player under Xvfb with --headless --after-ready --quit-after 30
  --fixed-fps 60 --screenshot --fail-on-error exits 0, and its frame matches
  golden/prebuilt-linux-<backend>.png (pngdiff --tolerance 24 --max-fraction 0.01). Content of
  another player exits 5, a bad option 2.
- components: tools/fixtures/reflection-parity (every component type, assets missing) built with
  the release player and run headless --after-ready: it gets ready (missing assets are logged, not
  waited for) and exits 0, with no cooked component unknown or unreadable (the release link keeps
  every table and hook).
- web (AC2, S06 AC20/AC21): `build --platform web` with EMSDK unset; the browser gate passes with
  COOP/COEP headers (pthreads player), without them (the service worker isolates the page after
  one reload: pthreads player), without them and with the service worker blocked (single-thread
  player), and a build with Web: Multithreaded off has no -mt files and runs single-thread.
- variants (S34 AC3, AC5): tools/fixtures/unified-lighting (scripts that build meshes: no HTML UI),
  physics-placement (static primitives, no scripts, no physics components, no start scene in its
  project.yaml: no HTML UI, no runtime shader compiler) and the upgrade fixture
  tools/test-projects/upgrade-from-main/project (HTML UI documents without video, physics,
  scripts; its `duration` script property and a `"load"` log label renamed in the copy, as video
  members have those names and a name scan keeps video for them) built for Linux glcore get the player variant their modules
  need (no W-PLAYER-VARIANT), which leaves modules out; physics-placement also with the full
  player and lite3d alone (it takes lite3d, with no warning: no variant of fewer modules is
  missing); built with the full player alone they warn W-PLAYER-VARIANT and use it; each
  variant's build and the full player's run headless to the same frame, pixel for pixel (not
  atmosphere: its clouds differ between two runs of one player).
- titleart-off (S80 AC32, R40; needs cmake: run it in the linux image, tools/build.sh exec linux): the
  engine library configured with -DJASPERENGINE_TITLE_ART=OFF (--titleart-off-build, built when
  absent) holds no engine/core/titleart object and no jasperengine::titleart symbol, builds no
  Clipper2, builds ThorVG only for the HTML UI, and its HarfBuzz keeps HB_NO_DRAW (no hb_draw code).
- fallback (AC3): tools/fixtures/prefab-runtime (C++ scripts) reports `method: compile` and
  `compile_reason: C++ scripts (...)`, and here fails with `needs a C++ toolchain`; with
  --method prebuilt it fails listing the reasons.

--write-goldens writes the golden frames instead of comparing (then commit them). Exit 1 on the
first failure, with the log. Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
FIXTURES = REPO / "tools" / "fixtures"
sys.path.insert(0, str(REPO / "tools" / "mcp"))
import pngdiff  # noqa: E402


class Failure(Exception):
    pass


def run(args: list[str], *, env: dict | None = None, timeout: float = 900, check: bool = True, cwd: Path | None = None) -> subprocess.CompletedProcess:
    started = time.monotonic()
    result = subprocess.run([str(a) for a in args], cwd=cwd, env=env, capture_output=True, text=True, timeout=timeout)
    result.seconds = time.monotonic() - started  # type: ignore[attr-defined]
    if check and result.returncode != 0:
        raise Failure(f"{' '.join(str(a) for a in args)} exited {result.returncode}\n{result.stdout[-4000:]}\n{result.stderr[-4000:]}")
    return result


def fresh_copy(source: Path, target: Path) -> Path:
    shutil.rmtree(target, ignore_errors=True)
    shutil.copytree(source, target)
    return target


class Xvfb:
    """A virtual X server for the players (--headless keeps the GPU device: D9)."""

    def __init__(self, display: str = ":97"):
        self.display = display
        self.process = None

    def __enter__(self):
        if os.environ.get("DISPLAY"):
            return self
        self.process = subprocess.Popen(["Xvfb", self.display, "-screen", "0", "1600x900x24"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        os.environ["DISPLAY"] = self.display
        time.sleep(1)
        return self

    def __exit__(self, *exc):
        if self.process:
            self.process.terminate()
            self.process.wait(timeout=10)
            os.environ.pop("DISPLAY", None)


def editor_env(players: Path) -> dict:
    env = dict(os.environ)
    env["JASPER_PLAYERS_DIR"] = str(players)
    env.pop("EMSDK", None)
    env["HOME"] = env.get("HOME") or "/tmp"
    return env


def build(editor: Path, players: Path, project: Path, out: Path, platform: str, *extra: str, check: bool = True) -> subprocess.CompletedProcess:
    return run(["dbus-run-session", "--", editor, "build", "--project", project, "--platform", platform, "--out", out, *extra],
               env=editor_env(players), check=check)


def lines(result: subprocess.CompletedProcess, key: str) -> list[str]:
    return [line[len(key) + 2:] for line in result.stdout.splitlines() if line.startswith(key + ": ")]


def check_linux(args, players: Path, out: Path) -> None:
    project = fresh_copy(FIXTURES / "unified-lighting", out / "linux-project")
    goldens = FIXTURES / "unified-lighting" / "golden"
    installed = {p.name for p in players.iterdir()} if players.is_dir() else set()
    backends = [b for b in args.backends.split(",") if f"linux-x64-{b}-release" in installed]
    if not backends:
        raise Failure(f"no Linux player runtime in {players} for {args.backends}: run tools/build.sh players")
    executables: dict[str, Path] = {}
    with Xvfb():
        for backend in backends:
            target = out / f"linux-{backend}"
            result = build(args.editor, players, project, target, "linux", "--backend", backend)
            if lines(result, "method") != ["prebuilt"]:
                raise Failure(f"linux {backend}: not built with the prebuilt player:\n{result.stdout}")
            print(f"ok: linux {backend} built in {result.seconds:.1f} s (R28 budget 20 s on a 4-vCPU runner, warm cache)")
            exe = Path(lines(result, "executable")[0])
            shot = out / f"linux-{backend}.png"
            game = run([exe, "--headless", "--after-ready", "--quit-after", "30", "--fixed-fps", "60", "--screenshot", shot, "--fail-on-error"],
                       cwd=exe.parent, timeout=600, check=False)
            if game.returncode != 0 or not shot.is_file():
                raise Failure(f"linux {backend}: the player exited {game.returncode}\n{game.stdout[-3000:]}\n{game.stderr[-3000:]}")
            # a frame of one colour is nothing drawn over the clear colour: never a pass, nor a golden
            if pngdiff.flat(pngdiff.load(shot), tolerance=8):
                raise Failure(f"linux {backend}: the frame is one colour (nothing drawn): {shot}\n{game.stdout[-3000:]}\n{game.stderr[-3000:]}")
            golden = goldens / f"prebuilt-linux-{backend}.png"
            if args.write_goldens:
                golden.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(shot, golden)
                print(f"wrote {golden}")
            else:
                if not golden.is_file():
                    raise Failure(f"no golden {golden} (--write-goldens makes it)")
                run([sys.executable, REPO / "tools/mcp/pngdiff.py", shot, golden, "--tolerance", "24", "--max-fraction", "0.01",
                     "--diff", out / f"linux-{backend}-diff.png"])
            print(f"ok: linux {backend} ran and matches its golden")
            # the context it really ran on (Engine's "Graphics:" line): a gles3 player on OpenGL ES
            graphics = next((line for line in (game.stdout + game.stderr).splitlines() if line.startswith("Graphics: ")), "")
            expected = {"glcore": "OpenGL 3.3 core", "gles3": "OpenGL ES", "vulkan": "Vulkan"}[backend]
            if expected not in graphics or (backend == "gles3" and "OpenGL ES 3" not in graphics.split(";")[1]):
                raise Failure(f"linux {backend}: the player ran on another context: {graphics!r}")
            print(f"ok: linux {backend} context: {graphics}")
            executables[backend] = exe

        # the players are different binaries: their own hashes, and the GLES one links libGLESv2
        hashes = {b: hashlib.sha256(p.read_bytes()).hexdigest() for b, p in executables.items()}
        if len(set(hashes.values())) != len(hashes):
            raise Failure(f"two Linux players are the same binary: {hashes}")
        for b, p in executables.items():
            needs_gles = b"libGLESv2.so.2" in p.read_bytes()
            if needs_gles != (b == "gles3"):
                raise Failure(f"linux {b}: {'needs' if needs_gles else 'does not need'} libGLESv2.so.2")
        print("ok: the players are distinct binaries; only gles3 links libGLESv2.so.2: " +
              ", ".join(f"{b} {h[:12]}" for b, h in sorted(hashes.items())))

        # R19: bad options exit 2, content of another player 5
        backend = backends[0]
        exe = Path(lines(build(args.editor, players, project, out / f"linux-{backend}", "linux", "--backend", backend), "executable")[0])
        bad = run([exe, "--quit-after", "zero"], cwd=exe.parent, check=False, timeout=60)
        if bad.returncode != 2:
            raise Failure(f"--quit-after zero exited {bad.returncode}, not 2")
        manifest = exe.parent / "content-manifest.json"
        data = json.loads(manifest.read_text())
        data["playerId"] = "linux-x64-other-release"
        manifest.write_text(json.dumps(data))
        other = run([exe, "--headless", "--quit-after", "5"], cwd=exe.parent, check=False, timeout=120)
        if other.returncode != 5:
            raise Failure(f"content of another player exited {other.returncode}, not 5\n{other.stderr[-2000:]}")
        print("ok: exit codes 2 (bad option) and 5 (content of another player)")


def check_audio(args, players: Path, out: Path) -> None:
    """S35 AC3: a development player's --headless capture is the same bytes on every run, its sources play
    (Lua's log says so at frame 60), and a release player refuses --audio-capture with exit 2."""
    installed = {p.name for p in players.iterdir()} if players.is_dir() else set()
    if "linux-x64-glcore-development" not in installed or "linux-x64-glcore-release" not in installed:
        raise Failure(f"no glcore development and release players in {players}: run tools/build.sh players")
    project = fresh_copy(FIXTURES / "audio-capture", out / "audio-project")
    with Xvfb():
        built = build(args.editor, players, project, out / "audio-dev", "linux", "--backend", "glcore", "--development")
        exe = Path(lines(built, "executable")[0])
        digests, logs = [], []
        for take in ("one", "two"):
            root = out / f"audio-sessions-{take}"
            shutil.rmtree(root, ignore_errors=True)
            game = run([exe, "--headless", "--fixed-fps", "60", "--quit-after", "300", "--audio-capture", root], cwd=exe.parent, timeout=600, check=False)
            if game.returncode != 0:
                raise Failure(f"the capture run exited {game.returncode}\n{game.stdout[-3000:]}\n{game.stderr[-3000:]}")
            sessions = sorted(p for p in root.iterdir() if p.is_dir())
            if len(sessions) != 1:
                raise Failure(f"one session, not {sessions}")
            session = json.loads((sessions[0] / "session.json").read_text())
            if session["state"] != "complete" or not session["deterministic"]:
                raise Failure(f"session.json: {session}")
            digests.append({name: hashlib.sha1((sessions[0] / name).read_bytes()).hexdigest() for name in ("master.wav", "events.jsonl", "levels.jsonl")})
            logs.append(game.stdout + game.stderr)
        if digests[0] != digests[1]:
            raise Failure(f"two runs differ: {digests}")
        if "capture probe frame 60 playing true" not in logs[0]:
            raise Failure(f"the source does not play at frame 60 under --headless:\n{logs[0][-3000:]}")
        print(f"ok: two --headless captures are the same bytes: {digests[0]}")

        built = build(args.editor, players, project, out / "audio-release", "linux", "--backend", "glcore")
        exe = Path(lines(built, "executable")[0])
        refused = run([exe, "--headless", "--quit-after", "5", "--audio-capture", out / "audio-refused"], cwd=exe.parent, timeout=120, check=False)
        if refused.returncode != 2 or "--audio-capture needs a development player" not in refused.stderr:
            raise Failure(f"a release player exited {refused.returncode}: {refused.stderr[-1000:]}")
        print("ok: a release player refuses --audio-capture with exit 2")


def check_components(args, players: Path, out: Path) -> None:
    """Every component type through a release player: tools/fixtures/reflection-parity (one entity
    per type) cooked and run headless. The release link (sections, hidden visibility, LTO) must
    keep every component's table and hooks: no cooked component unknown or unreadable."""
    project = fresh_copy(FIXTURES / "reflection-parity", out / "components-project")
    texture = project / "textures" / "parity.png"   # (its project has no assetsDir: the project root)
    texture.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy(FIXTURES / "unified-lighting" / "golden" / "prebuilt-linux-glcore.png", texture)
    installed = {p.name for p in players.iterdir()} if players.is_dir() else set()
    backend = next((b for b in args.backends.split(",") if f"linux-x64-{b}-release" in installed), None)
    if not backend:
        raise Failure(f"no Linux release player runtime in {players}: run tools/build.sh players")
    with Xvfb():
        # (the fixture is the reflection tables' every value, not a playable game: the assets it
        # names are not all there, and the player logs those it cannot load)
        result = build(args.editor, players, project, out / "components", "linux", "--backend", backend, "--method", "prebuilt",
                       "--allow-missing-assets", "--start-scene", "AllComponents")
        exe = Path(lines(result, "executable")[0])
        # its assets are not all there (a texture, a pose animation, an empty mesh, a model file):
        # the failures are logged, and the scene still finishes loading and gets ready
        game = run([exe, "--headless", "--after-ready", "--quit-after", "10", "--fixed-fps", "60", "--ready-timeout", "120"],
                   cwd=exe.parent, timeout=600, check=False)
    log = game.stdout + game.stderr
    problems = [line for line in log.splitlines() if "Cooked content" in line or "unknown component" in line]
    if game.returncode != 0 or problems:
        raise Failure(f"components: the player exited {game.returncode}; {problems}\n{log[-3000:]}")
    print(f"ok: every component type of {project.name} loaded by the {backend} release player")


def set_web_threads(project: Path, on: bool) -> None:
    text = (project / "project.yaml").read_text()
    text = re.sub(r"\n\s*webMultithreaded:.*", "", text)
    if "\nthreading:" in text:
        text = text.replace("\nthreading:", f"\nthreading:\n  webMultithreaded: {'true' if on else 'false'}", 1)
    else:
        text += f"\nthreading:\n  webMultithreaded: {'true' if on else 'false'}\n"
    (project / "project.yaml").write_text(text)


def gate(root: Path, baseline: str, shot: Path, *extra: str) -> None:
    shot.unlink(missing_ok=True)
    result = run([sys.executable, REPO / "tools/web/browser_gate.py", "--root", root, "--page", "index.html", "--until-ready",
                  "--baseline", REPO / "tools/web/gates" / f"{baseline}.json", "--screenshot", shot, "--transcript",
                  shot.with_suffix(".log"), *extra], timeout=600)
    shot.with_suffix(".gate.txt").write_text(result.stdout + result.stderr)
    # the page drew the game: a capture, and not one colour
    if not shot.is_file():
        raise Failure(f"{baseline}: the gate captured no screenshot\n{result.stdout[-3000:]}")
    if pngdiff.flat(pngdiff.load(shot), tolerance=8):
        raise Failure(f"{baseline}: the page shows one colour (nothing drawn): {shot}")


def check_web(args, players: Path, out: Path) -> None:
    project = fresh_copy(FIXTURES / "unified-lighting", out / "web-project")
    set_web_threads(project, True)
    target = out / "web"
    result = build(args.editor, players, project, target, "web")
    if lines(result, "method") != ["prebuilt"]:
        raise Failure(f"web: not built with the prebuilt player:\n{result.stdout}")
    for name in ("index.html", "content.jpak", "content-manifest.json", "jasperengine-player-st.wasm", "jasperengine-player-mt.wasm",
                 "coi-serviceworker.js", "player-loader.js"):
        if not (target / name).is_file():
            raise Failure(f"web: no {name} in the build")
    print(f"ok: web built in {result.seconds:.1f} s with EMSDK unset")
    gate(target, "prebuilt-unified-lighting", out / "web-isolated.png")
    print("ok: web with COOP/COEP: pthreads player")
    gate(target, "prebuilt-unified-lighting", out / "web-worker.png", "--no-isolation")
    print("ok: web without COOP/COEP: the service worker isolates the page, pthreads player")
    gate(target, "prebuilt-unified-lighting-noworker", out / "web-noworker.png", "--no-isolation")
    print("ok: web without COOP/COEP and without a service worker: single-thread player (S06 AC21)")

    set_web_threads(project, False)
    single = out / "web-st"
    build(args.editor, players, project, single, "web")
    leftovers = sorted(p.name for p in single.iterdir() if "-mt" in p.name or p.name == "coi-serviceworker.js")
    if leftovers:
        raise Failure(f"web without multithreading ships {leftovers}")
    gate(single, "prebuilt-unified-lighting-st", out / "web-st.png", "--no-isolation")
    print("ok: web.multithreaded false: no -mt files, single-thread player")


def check_variants(args, players: Path, out: Path) -> None:
    full_id = "linux-x64-glcore-release"
    if not (players / full_id).is_dir():
        raise Failure(f"no {full_id} in {players}: run tools/build.sh players")
    # a players directory with the full player alone
    full_only = out / "players-full-only"
    shutil.rmtree(full_only, ignore_errors=True)
    full_only.mkdir(parents=True)
    (full_only / full_id).symlink_to(players / full_id, target_is_directory=True)
    # the variant each fixture gets from every installed variant, then the others it also runs
    # with when they are the only variant installed next to the full player
    expected = {"unified-lighting": ["core"], "physics-placement": ["lite2d", "lite3d"], "upgrade-from-main": ["ui"]}
    sources = {"unified-lighting": FIXTURES / "unified-lighting", "physics-placement": FIXTURES / "physics-placement",
               "upgrade-from-main": REPO / "tools/test-projects/upgrade-from-main/project"}

    def copy(fixture: str, target: Path) -> Path:
        project = fresh_copy(sources[fixture], target)
        if fixture == "upgrade-from-main":
            # its directors' "duration" script property (self.duration, the scenes' values) and the
            # title's "load" transcript label are also the names of video members (UIElement.duration,
            # :load()), which a name scan keeps video for: the same game under other names
            renamed = 0
            for path in list(project.rglob("*.lua")) + list(project.rglob("*.scene")):
                text = path.read_text(encoding="utf-8")
                new = text.replace('name = "duration"', 'name = "runFrames"').replace("self.duration", "self.runFrames")
                new = new.replace("- name: duration\n", "- name: runFrames\n").replace('T.log("load"', 'T.log("loading"')
                if new != text:
                    path.write_text(new, encoding="utf-8")
                    renamed += 1
            leftover = [str(p) for p in project.rglob("*.lua") if re.search(r"\bduration\b|\"load\"", p.read_text(encoding="utf-8"))]
            if renamed == 0 or leftover:
                raise Failure(f"upgrade-from-main: renaming its duration property: {renamed} files, still in {leftover}")
        return project

    def frame(exe: Path, shot: Path) -> None:
        game = run([exe, "--headless", "--after-ready", "--quit-after", "30", "--fixed-fps", "60", "--screenshot", shot, "--fail-on-error"],
                   cwd=exe.parent, timeout=600, check=False)
        if game.returncode != 0 or not shot.is_file():
            raise Failure(f"{exe}: exited {game.returncode}\n{game.stdout[-3000:]}\n{game.stderr[-3000:]}")
        if pngdiff.flat(pngdiff.load(shot), tolerance=8):
            raise Failure(f"{exe}: the frame is one colour (nothing drawn): {shot}")

    def with_variant(variant_id: str) -> Path:  # a players directory with the full player and this variant
        directory = out / f"players-{variant_id}"
        shutil.rmtree(directory, ignore_errors=True)
        directory.mkdir(parents=True)
        for runtime in (full_id, variant_id):
            (directory / runtime).symlink_to(players / runtime, target_is_directory=True)
        return directory

    with Xvfb():
        for fixture, variants in expected.items():
            whole = build(args.editor, full_only, copy(fixture, out / f"full-{fixture}-project"), out / f"full-{fixture}",
                          "linux", "--backend", "glcore")
            first_id = f"{full_id}-{variants[0]}"
            if lines(whole, "player") != [full_id] or f"W-PLAYER-VARIANT: the smaller player {first_id} fits" not in whole.stdout:
                raise Failure(f"{fixture}: with the full player alone expected it and W-PLAYER-VARIANT naming {first_id}:\n{whole.stdout}")
            full_shot = out / f"full-{fixture}.png"
            frame(Path(lines(whole, "executable")[0]), full_shot)
            for index, variant in enumerate(variants):
                variant_id = f"{full_id}-{variant}"
                if not (players / variant_id).is_dir():
                    raise Failure(f"no {variant_id} in {players}: run tools/build.sh players (every release variant)")
                source_players = players if index == 0 else with_variant(variant_id)
                project = copy(fixture, out / f"variant-{fixture}-{variant}-project")
                stripped = build(args.editor, source_players, project, out / f"variant-{fixture}-{variant}", "linux", "--backend", "glcore")
                if lines(stripped, "player") != [variant_id] or "W-PLAYER-VARIANT" in stripped.stdout:
                    raise Failure(f"{fixture}: expected the player {variant_id}:\n{stripped.stdout}")
                left_out = lines(stripped, "left_out")[0].split()
                if not left_out or left_out == ["none"]:
                    raise Failure(f"{fixture}: {variant_id} left nothing out:\n{stripped.stdout}")
                print(f"ok: {fixture} -> {variant_id} (modules: {lines(stripped, 'modules')[0]}; left out: {' '.join(left_out)})")
                for reason in lines(stripped, "module"):
                    print(f"    module: {reason}")
                shot = out / f"variant-{fixture}-{variant}.png"
                frame(Path(lines(stripped, "executable")[0]), shot)
                run([sys.executable, REPO / "tools/mcp/pngdiff.py", shot, full_shot, "--tolerance", "0", "--max-fraction", "0",
                     "--diff", out / f"variant-{fixture}-{variant}-diff.png"])
                print(f"ok: {fixture}: {variant_id} and {full_id} draw the same frame")


def check_fallback(args, players: Path, out: Path) -> None:
    project = fresh_copy(FIXTURES / "prefab-runtime", out / "fallback-project")
    result = build(args.editor, players, project, out / "fallback", "linux", check=False)
    if lines(result, "method") != ["compile"] or not any(r.startswith("C++ scripts (") for r in lines(result, "compile_reason")):
        raise Failure(f"prefab-runtime: expected method compile, C++ scripts:\n{result.stdout}\n{result.stderr[-2000:]}")
    if shutil.which("c++") is None and shutil.which("cc") is None:
        if result.returncode != 1 or "needs a C++ toolchain" not in result.stderr:
            raise Failure(f"prefab-runtime without a compiler: expected exit 1 'needs a C++ toolchain', got {result.returncode}\n{result.stderr[-2000:]}")
    forced = build(args.editor, players, project, out / "fallback-prebuilt", "linux", "--method", "prebuilt", check=False)
    if forced.returncode != 1 or "cannot use a prebuilt player" not in forced.stderr or "C++ scripts" not in forced.stderr:
        raise Failure(f"--method prebuilt on prefab-runtime: expected the reasons\n{forced.stdout}\n{forced.stderr[-2000:]}")
    print("ok: C++ scripts compile (and say why); --method prebuilt refuses them")


def check_titleart_off(args, players: Path, out: Path) -> None:
    build_dir = args.titleart_off_build.resolve()
    library = build_dir / "libjasperengine.a"
    if not library.is_file():
        if shutil.which("cmake") is None:
            raise Failure("titleart-off configures the engine: run it in the linux image (tools/build.sh exec linux ...)")
        run(["cmake", "-S", REPO / "engine", "-B", build_dir, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DJASPERENGINE_TITLE_ART=OFF",
             "-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"], timeout=900)
        run(["cmake", "--build", build_dir, "--target", "jasperengine", "-j", os.environ.get("JOBS", "4")], timeout=7200)
    cache = (build_dir / "CMakeCache.txt").read_text()
    if "JASPERENGINE_TITLE_ART:BOOL=OFF" not in cache:
        raise Failure(f"{build_dir} is not configured with JASPERENGINE_TITLE_ART=OFF")
    htmlui = "JASPERENGINE_HTMLUI:BOOL=ON" in cache
    objects = [m for m in run(["ar", "t", library]).stdout.split() if "titleart" in m.lower()]
    if objects:
        raise Failure(f"the OFF library holds Title Art objects: {objects[:10]}")
    symbols = run(["nm", "-C", "--defined-only", library], timeout=600).stdout.splitlines()
    titleart = [line for line in symbols if "jasperengine::titleart::" in line]
    if titleart:
        raise Failure(f"the OFF library defines {len(titleart)} Title Art symbols, e.g. {titleart[:5]}")
    if (build_dir / "libs" / "clipper2").exists():
        raise Failure("the OFF build configures Clipper2 (only Title Art uses it)")
    if (build_dir / "libs" / "thorvg").exists() != htmlui:
        raise Failure(f"ThorVG is built {'without' if htmlui else 'with'} the HTML UI {'on' if htmlui else 'off'}: only the HTML UI needs it here")
    harfbuzz = next(build_dir.glob("libs/harfbuzz/**/libharfbuzz*.a"), None)
    if harfbuzz is None:
        raise Failure(f"no HarfBuzz library under {build_dir / 'libs/harfbuzz'}")
    # HB_NO_DRAW compiles hb-draw.cc out (hb_font_draw_glyph stays, a no-op without it)
    def draw_functions(archive: Path) -> list[str]:
        return [line for line in run(["nm", "--defined-only", archive]).stdout.splitlines() if " T hb_draw_" in line]
    drawing = draw_functions(harfbuzz)
    if drawing:
        raise Failure(f"HarfBuzz lost HB_NO_DRAW in the OFF build: {drawing[:5]}")
    # the measure is not empty: the ON engine's HarfBuzz (the linux build's) has them
    on = next((REPO / "build/linux/engine/libs/harfbuzz").glob("**/libharfbuzz*.a"), None)
    if on is not None and not draw_functions(on):
        raise Failure(f"{on} (Title Art on) has no hb_draw_ functions: the HB_NO_DRAW check measures nothing")
    print(f"ok: titleart off ({library}): no Title Art objects or symbols, no Clipper2, ThorVG {'for the HTML UI' if htmlui else 'not built'}, "
          "HarfBuzz without hb_draw")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--editor", type=Path, default=REPO / "build/linux/editor-install/bin/jasperengine-editor")
    parser.add_argument("--players", type=Path, default=REPO / "build/players")
    parser.add_argument("--out", type=Path, default=REPO / "build/player-cli")
    parser.add_argument("--backends", default="glcore,gles3,vulkan")
    parser.add_argument("--only", default="linux,audio,components,variants,web,fallback")
    parser.add_argument("--write-goldens", action="store_true")
    parser.add_argument("--titleart-off-build", type=Path, default=REPO / "build/linux/engine-titleart-off",
                        help="the engine configured with JASPERENGINE_TITLE_ART=OFF (titleart-off; built when absent)")
    args = parser.parse_args()
    if not args.editor.is_file() and args.only != "titleart-off":
        print(f"no editor at {args.editor}: run tools/build.sh linux", file=sys.stderr)
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    checks = {"linux": check_linux, "audio": check_audio, "components": check_components, "variants": check_variants, "web": check_web,
              "fallback": check_fallback, "titleart-off": check_titleart_off}
    try:
        for name in args.only.split(","):
            if name not in checks:
                raise Failure(f"--only: unknown check {name}")
            checks[name](args, args.players.resolve(), args.out.resolve())
    except (Failure, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
