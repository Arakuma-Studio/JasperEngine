"""S48 AC26 (docs/specs/S48-pixel-fx.md): the exported game draws the runtime Pixel FX as the editor's Game view
does. It reads what `SMOKE_METHOD=compile SMOKE_GAME_SECONDS="ready 10" SMOKE_GAME_RUNS="idle PFX_STAGE=idle;
set PFX_STAGE=set; covered PFX_STAGE=covered" tools/smoke-test.sh tools/fixtures/pixel-fx-game` left in its
directory: game.png (the whole flow: PixelFX.play, HitStop.play, PixelFX.glitch, Transition.loadScene, ending on
the revealed Level scene) and game-<stage>.png, each the screen with the game's window at the top left.

Screenshots taken on the wall clock do not line up with game time, so each stage ends in a static state
(tools/fixtures/make_pixel_fx_game_fixture.py): an editor started with the same PFX_STAGE plays the start scene,
waits for the script's [gate] line and screenshots its Game view at the window's size, simulating the Device Tier
the game applied (its log's "Device tier:" line: llvmpipe is Low, which renders at scale 0.5, and the editor applies
no tier unless one is simulated). A pixel differs when a
channel differs by more than 24; at most 1 % may (AC26). The largest channel difference and the share over 24
are printed per stage and smoke directory.

PFX_SMOKE_DIRS names the smoke directories to compare, space separated (default build/smoke): one editor run
per stage serves every backend's game (build/pfx-opengl build/pfx-vulkan build/pfx-gles3). Images go to
build/test-results/pixel-fx-export/.
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
from pathlib import Path

import pngdiff
import yaml
from harness import REPO, Editor

sys.path.insert(0, str(Path(__file__).parent))
from test_pixel_fonts_export import rgb_rows  # noqa: E402

CI = False
CI_EXCLUDED = "needs SMOKE_METHOD=compile tools/smoke-test.sh tools/fixtures/pixel-fx-game runs first (PFX_SMOKE_DIRS)"
TIMEOUT = 900
FIXTURE = REPO / "tools" / "fixtures" / "pixel-fx-game"
OUT = REPO / "build" / "test-results" / "pixel-fx-export"
TOLERANCE = 24
MAX_SHARE = 0.01

# stage: (the smoke run's screenshot, the line the script logs once the state holds)
STAGES = {
    "flow": ("game.png", "[gate] pixel_fx level revealed"),
    "idle": ("game-idle.png", "[gate] pixel_fx stage idle"),
    "set": ("game-set.png", "[gate] pixel_fx stage set"),
    "covered": ("game-covered.png", "[gate] pixel_fx stage covered"),
}


def smoke_dirs() -> list[Path]:
    return [REPO / d for d in os.environ.get("PFX_SMOKE_DIRS", "build/smoke").split()]


def game_tier(directory: Path, stage: str) -> str:
    """The Device Tier the game applied ("low", ...), from its log; "auto" when it applied none"""
    log = directory / ("game.log" if stage == "flow" else f"game-{stage}.log")
    text = log.read_text(errors="replace") if log.exists() else ""
    found = re.search(r"^Device tier: .*, tier (\w+) \([^)]*\)$", text, re.M)
    return found.group(1).lower() if found else "auto"


def window_size() -> tuple[int, int]:
    project = yaml.safe_load((FIXTURE / "project.yaml").read_text())
    return int(project.get("windowWidth", 1280)), int(project.get("windowHeight", 720))


def rgba(width: int, height: int, rows) -> tuple:
    data = bytearray(width * height * 4)
    for y, row in enumerate(rows):
        for x, (r, g, b) in enumerate(row):
            data[(y * width + x) * 4:(y * width + x) * 4 + 4] = bytes((r, g, b, 255))
    return width, height, data


def largest_difference(a: tuple, b: tuple) -> int:
    return max((abs(p - q) for p, q in zip(a[2], b[2])), default=0)


def drawn_share(a: tuple, b: tuple) -> tuple[int, float]:
    """The pixels either image draws other than the background (the top left pixel's colour), and the share of
    them over the tolerance: the sprites alone, which the whole frame's share dilutes (reported, not judged)"""
    data_a, data_b = a[2], b[2]
    background = bytes(data_a[0:4])
    drawn = over = 0
    for i in range(0, len(data_a), 4):
        pa, pb = data_a[i:i + 4], data_b[i:i + 4]
        if pa == background and pb == background:
            continue
        drawn += 1
        if any(abs(x - y) > TOLERANCE for x, y in zip(pa, pb)):
            over += 1
    return drawn, over / drawn if drawn else 0.0


async def wait_for_line(ed: Editor, line: str, seconds: float = 120.0) -> None:
    deadline = time.monotonic() + seconds
    seen: list[str] = []
    while time.monotonic() < deadline:
        entries = await ed.console_since()
        seen += [e.get("message", "") for e in entries]
        if any(line in m for m in seen):
            return
        await ed.call("wait", frames=10)
    ed.fail(f"the script did not log {line!r} in {seconds:.0f} s; the console's last lines: {seen[-10:]}")


async def editor_frame(stage: str, tier: str, width: int, height: int) -> tuple:
    env = {} if stage == "flow" else {"PFX_STAGE": stage}
    async with Editor(project=FIXTURE, env=env, label=f"pixel-fx-export-{stage}") as ed:
        await ed.call("configure_game_view", resolution=f"{width}x{height}")
        await ed.call("set_device_simulation", tier=tier, platform="desktop" if tier != "auto" else "auto")
        main = next(s["id"] for s in (await ed.call("list_scenes"))["scenes"] if s["name"] == "Main")
        await ed.call("open_scene", scene=main)
        await ed.console_since(None)
        await ed.call("play", action="start", scene=main)
        try:
            await wait_for_line(ed, STAGES[stage][1])
            # the state holds; a few frames more so the frame drawn is the held one
            await ed.call("wait", frames=30)
            png = await ed.screenshot(view="game", max_width=0)
        finally:
            await ed.call("play", action="stop")
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / f"editor-{stage}-{tier}.png").write_bytes(png)
    ew, eh, rows = rgb_rows(png)
    return rgba(ew, eh, rows)


def game_frame(path: Path, width: int, height: int) -> tuple:
    """The game's window from a screenshot of the whole screen: the top left width x height, and only the black
    root window beside it (the window sits there and has that size)"""
    sw, sh, screen = rgb_rows(path.read_bytes())
    assert sw >= width and sh >= height, f"{path}: the screen is {sw}x{sh}, smaller than the {width}x{height} window"
    outside = sum(1 for y in range(sh) for x in range(sw) if (x >= width or y >= height) and screen[y][x] != (0, 0, 0))
    assert outside == 0, f"{path}: {outside} pixels beside the {width}x{height} window at the top left are not the root window's black"
    return rgba(width, height, [row[:width] for row in screen[:height]])


async def check_stage(stage: str) -> None:
    width, height = window_size()
    games = [(d, d / STAGES[stage][0]) for d in smoke_dirs()]
    missing = [str(p) for _, p in games if not p.exists()]
    games = [(d, p) for d, p in games if p.exists()]
    assert games, f"no smoke screenshot {missing}: run the smoke test of the docstring first (PFX_SMOKE_DIRS)"
    failures, report, editors = [], {}, {}
    for directory, path in games:
        tier = game_tier(directory, stage)
        if tier not in editors:
            editor = await editor_frame(stage, tier, width, height)
            assert (editor[0], editor[1]) == (width, height), f"the Game view is {editor[0]}x{editor[1]}, not the window's {width}x{height}"
            if stage == "covered":
                # the whole frame in the ink, sRGB #0b0a10
                ink = tuple(editor[2][0:3])
                assert pngdiff.flat(editor) and all(abs(c - e) <= 2 for c, e in zip(ink, (11, 10, 16))), \
                    f"covered: the editor's frame is not all ink #0b0a10 (top left {ink})"
            else:
                assert not pngdiff.flat(editor), f"{stage}: the editor's frame is one colour"
            editors[tier] = editor
        editor = editors[tier]
        game = game_frame(path, width, height)
        ok, share, diff = pngdiff.compare(editor, game, TOLERANCE, MAX_SHARE)
        largest = largest_difference(editor, game)
        drawn, drawn_over = drawn_share(editor, game)
        name = f"{directory.name}-{stage}"
        (OUT / f"game-{name}.png").write_bytes(pngdiff.encode(game))
        if diff:
            (OUT / f"diff-{name}.png").write_bytes(diff)
        report[directory.name] = {"tier": tier, "largest_channel_difference": largest, "share_over_24": share, "pass": ok,
                                  "drawn_pixels": drawn, "drawn_share_over_24": drawn_over}
        print(f"[pixel-fx-export] {stage} {directory.name} (tier {tier}): largest channel difference {largest}, "
              f"{share * 100:.3f} % of pixels over {TOLERANCE} (at most {MAX_SHARE * 100:.0f} %): {'ok' if ok else 'FAIL'}; "
              f"{drawn_over * 100:.3f} % of the {drawn} drawn pixels", flush=True)
        if not ok:
            failures.append(f"{directory.name}: {share * 100:.3f} % of pixels over {TOLERANCE} (largest {largest}), see {OUT / f'diff-{name}.png'}")
    (OUT / f"report-{stage}.json").write_text(json.dumps(report, indent=1))
    failures += [f"no smoke screenshot {p}" for p in missing]
    assert not failures, f"{stage}: the exported game does not draw as the editor: {failures}"


async def test_flow_ends_on_the_revealed_level():
    await check_stage("flow")


async def test_idle_sprite_features():
    await check_stage("idle")


async def test_set_dissolve_and_dither_fade():
    await check_stage("set")


async def test_covered_iris():
    await check_stage("covered")
