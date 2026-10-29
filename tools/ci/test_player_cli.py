#!/usr/bin/env python3
"""The prebuilt player end to end (docs/specs/S30-shipping.md AC1-AC3, AC5), in the no-compiler
image of docker/runtime.Dockerfile:

    tools/build.sh linux                      # the editor install (build/linux/editor-install)
    tools/build.sh players                    # Linux runtimes; PLAYER_PLATFORM=web for the web ones
    tools/build.sh exec runtime python3 tools/ci/test_player_cli.py [--only linux,web,fallback]

- linux (AC1): `jasperengine-editor build --platform linux` of tools/fixtures/unified-lighting
  with each Linux backend whose runtime is installed (--backends), timed (R28: <= 20 s with a warm
  cache, reported); the player under Xvfb with --headless --after-ready --quit-after 30
  --fixed-fps 60 --screenshot --fail-on-error exits 0, and its frame matches
  golden/prebuilt-linux-<backend>.png (pngdiff --tolerance 24 --max-fraction 0.01). Content of
  another player exits 5, a bad option 2.
- web (AC2, S06 AC20/AC21): `build --platform web` with EMSDK unset; the browser gate passes with
  COOP/COEP headers (pthreads player), without them (the service worker isolates the page after
  one reload: pthreads player), without them and with the service worker blocked (single-thread
  player), and a build with Web: Multithreaded off has no -mt files and runs single-thread.
- fallback (AC3): tools/fixtures/prefab-runtime (C++ scripts) reports `method: compile` and
  `compile_reason: C++ scripts (...)`, and here fails with `needs a C++ toolchain`; with
  --method prebuilt it fails listing the reasons.

--write-goldens writes the golden frames instead of comparing (then commit them). Exit 1 on the
first failure, with the log. Standard library only.
"""

from __future__ import annotations

import argparse
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


def set_web_threads(project: Path, on: bool) -> None:
    text = (project / "project.yaml").read_text()
    text = re.sub(r"\n\s*webMultithreaded:.*", "", text)
    if "\nthreading:" in text:
        text = text.replace("\nthreading:", f"\nthreading:\n  webMultithreaded: {'true' if on else 'false'}", 1)
    else:
        text += f"\nthreading:\n  webMultithreaded: {'true' if on else 'false'}\n"
    (project / "project.yaml").write_text(text)


def gate(root: Path, baseline: str, shot: Path, *extra: str) -> None:
    run([sys.executable, REPO / "tools/web/browser_gate.py", "--root", root, "--page", "index.html", "--until-ready",
         "--baseline", REPO / "tools/web/gates" / f"{baseline}.json", "--screenshot", shot, "--transcript", shot.with_suffix(".log"), *extra],
        timeout=600)


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--editor", type=Path, default=REPO / "build/linux/editor-install/bin/jasperengine-editor")
    parser.add_argument("--players", type=Path, default=REPO / "build/players")
    parser.add_argument("--out", type=Path, default=REPO / "build/player-cli")
    parser.add_argument("--backends", default="glcore,gles3,vulkan")
    parser.add_argument("--only", default="linux,web,fallback")
    parser.add_argument("--write-goldens", action="store_true")
    args = parser.parse_args()
    if not args.editor.is_file():
        print(f"no editor at {args.editor}: run tools/build.sh linux", file=sys.stderr)
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    checks = {"linux": check_linux, "web": check_web, "fallback": check_fallback}
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
