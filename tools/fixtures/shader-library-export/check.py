"""Checks that the exported game draws the shader-library-export fixture as the editor does.

For each scene (Lit, Blend, 2D, UI, Stencil, Phase3A) it renders the editor's Game view (the scene
through its camera, not playing) over MCP, runs tools/smoke-test.sh on the fixture (one export and
build; the game runs once per scene, SLX_SCENE picking it), and compares each game screenshot with
the editor's render: both scaled to the editor's size and slightly blurred, the per-pixel difference
(largest channel, 0-255) averaged over 16 px cells. A cell differing by more than CELL_LIMIT, or a
mean difference above MEAN_LIMIT, fails: a material that lost its shader, a parameter, a texture or
a feature on the way to the build changes a whole object, far above the noise of shadow edges and
filtering.

Writes to build/shader-library-export/: editor-<scene>.png, game-<scene>.png and compare-<scene>.png
(editor, game, and the difference x4 with the failing cells boxed in red).

    tools/fixtures/shader-library-export/check.sh                 # the whole check (~4 min)
    tools/fixtures/shader-library-export/check.sh --game-dir DIR  # compare with DIR/game*.png (no build)

JASPER_EDITOR selects the editor (default: this checkout's build).
"""
import argparse
import asyncio
import os
import shutil
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "mcp"))
from harness import Editor  # noqa: E402

SCENES = ["Lit", "Blend", "2D", "UI", "Stencil", "Phase3A"]   # Lit is the start scene; the others are SLX_SCENE runs
# Stencil (phase 2 presets) exists once generate.py has run with an editor that saves extra passes and
# stencil state (scene YAML) and has the apply_material_preset MCP tool
if not (HERE / "Stencil.scene").exists():
    print("check.py: Stencil.scene is missing: run generate.py to add the phase 2 presets scene", file=sys.stderr)
    SCENES.remove("Stencil")
CANVAS = (1280, 720)              # the project's window: the game draws it at the screen's top left
CELL = 16
CELL_LIMIT = 24.0
MEAN_LIMIT = 4.0


async def editor_renders(project: Path, out: Path) -> None:
    async with Editor(project=str(project)) as ed:
        await ed.call("configure_game_view", aspect="project", scale=1.0)
        scenes = {s["name"]: s["id"] for s in (await ed.call("list_scenes"))["scenes"]}
        for name in SCENES:
            await ed.call("open_scene", scene=scenes[name])
            await ed.call("wait", frames=30)
            r = await ed.call_raw("screenshot", {"view": "game", "scene": scenes[name], "max_width": 0,
                                                 "save_path": str(out / f"editor-{name}.png"),
                                                 "timeout_seconds": 300})
            if r.is_error:
                sys.exit(f"editor screenshot of {name} failed: {r.text}")
            print(f"editor: {r.text.splitlines()[0]}")


def smoke(project: Path) -> Path:
    env = dict(os.environ)
    env["SMOKE_GAME_RUNS"] = "; ".join(f"{name} SLX_SCENE={name}" for name in SCENES[1:])
    r = subprocess.run([str(REPO / "tools" / "smoke-test.sh"), str(project)], env=env)
    smoke_dir = REPO / "build" / "smoke"
    if r.returncode != 0:
        sys.exit(f"tools/smoke-test.sh failed (logs in {smoke_dir})")
    return smoke_dir


def game_shot(game_dir: Path, name: str) -> Path:
    return game_dir / ("game.png" if name == SCENES[0] else f"game-{name}.png")


def compare(name: str, editor_png: Path, game_png: Path, out: Path) -> bool:
    editor = Image.open(editor_png).convert("RGB")
    game = Image.open(game_png).convert("RGB").crop((0, 0) + CANVAS).resize(editor.size, Image.LANCZOS)
    game.save(out / f"game-{name}.png")
    a = np.asarray(editor.filter(ImageFilter.GaussianBlur(1.5)), dtype=np.float32)
    b = np.asarray(game.filter(ImageFilter.GaussianBlur(1.5)), dtype=np.float32)
    diff = np.abs(a - b).max(axis=2)
    h, w = (diff.shape[0] // CELL) * CELL, (diff.shape[1] // CELL) * CELL
    cells = diff[:h, :w].reshape(h // CELL, CELL, w // CELL, CELL).mean(axis=(1, 3))
    bad = np.argwhere(cells > CELL_LIMIT)
    mean = float(diff.mean())
    ok = len(bad) == 0 and mean <= MEAN_LIMIT

    heat = Image.fromarray(np.clip(diff * 4, 0, 255).astype(np.uint8)).convert("RGB")
    draw = ImageDraw.Draw(heat)
    for y, x in bad:
        draw.rectangle([x * CELL, y * CELL, (x + 1) * CELL - 1, (y + 1) * CELL - 1], outline=(255, 0, 0))
    sheet = Image.new("RGB", (editor.width, editor.height * 3 + 8), (255, 0, 0))
    for i, im in enumerate((editor, game, heat)):
        sheet.paste(im, (0, i * (editor.height + 4)))
    sheet.save(out / f"compare-{name}.png")
    print(f"{'ok  ' if ok else 'FAIL'} {name}: mean difference {mean:.2f} (limit {MEAN_LIMIT}), "
          f"largest cell {cells.max():.1f} (limit {CELL_LIMIT}), {len(bad)} cells over "
          f"-> {out / f'compare-{name}.png'}")
    return ok


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--game-dir", type=Path, help="compare with the game screenshots there (no smoke test)")
    parser.add_argument("--project", type=Path, default=HERE, help="the fixture (default: this directory)")
    parser.add_argument("--out", type=Path, default=REPO / "build" / "shader-library-export")
    args = parser.parse_args()

    out = args.out
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    asyncio.run(editor_renders(args.project, out))
    game_dir = args.game_dir or smoke(args.project)
    results = [compare(name, out / f"editor-{name}.png", game_shot(game_dir, name), out) for name in SCENES]
    if not all(results):
        sys.exit("FAIL: the exported game does not draw as the editor does (see the compare images)")
    print("PASS")


if __name__ == "__main__":
    main()
