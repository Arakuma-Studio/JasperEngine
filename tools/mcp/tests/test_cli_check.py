"""S42 AC11 (docs/specs/S42-pixel-art-mode.md R24): `jasperengine-editor check --pixel-art` on copies of
tools/fixtures/pixel-art-bad (exit 1, one line per finding in the
`path[#GameObject]: error|warning: [rule] message (fix: ...) [PIXELART-<RULE>]` format, ending with the
call that fixes) and tools/fixtures/pixel-art (exit 0); --format json validates against
tools/mcp/schemas/pixel_art_check.json; in a Pixel Art Mode project the default set and --hook include
the pixel-art category. --hook reports only what the changed files hold: a Write of a lossy pixel-art
PNG reports its finding on stderr and exits 2; a Write of an image that is not pixel art (a normal
map's import) reports nothing, an unrelated clean image in a project full of old FAILs exits 0, and a
changed scene reports that scene's findings only.
"""

from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import harness
from harness import REPO, Editor

CI = True
TIMEOUT = 900
EDITOR = {"create": True, "name": "CLI Check"}

LINE = re.compile(r"^(?P<path>[^:]+): (?P<severity>error|warning): \[(?P<rule>[a-z_]+)\] (?P<message>.+?)"
                  r"(?: \(fix: (?P<fix>.+)\))? \[PIXELART-(?P<code>[A-Z_]+)\]$")


def copy_fixture(name: str, root: Path) -> Path:
    target = root / name
    shutil.copytree(REPO / "tools" / "fixtures" / name, target)
    return target


def check(project: Path, *args: str, stdin: str | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(harness.editor_command() + ["check", "--project", str(project), *args], cwd=str(project),
                          input=stdin, capture_output=True, text=True, timeout=600)


async def test_pixel_art(ed: Editor):
    root = Path(tempfile.mkdtemp(prefix="je-s42-cli-"))
    try:
        bad = copy_fixture("pixel-art-bad", root)
        clean = copy_fixture("pixel-art", root)

        run = check(bad, "--pixel-art")
        ed.check_eq(run.returncode, 1, f"FAIL findings are errors: exit 1 ({run.stderr[-1500:]})")
        lines = [line for line in run.stdout.splitlines() if "[PIXELART-" in line]
        ed.check(len(lines) >= 12, f"one line per finding: {len(lines)}")
        for line in lines:
            match = LINE.match(line)
            ed.check(match is not None, f"the R24 format: {line}")
            if match:
                ed.check_eq(match["code"], match["rule"].upper(), f"the code names the rule: {line}")
        ed.check(any("[PIXELART-TEXEL_SIZE]" in line and line.startswith("scenes/Main.scene#/Scaled: error:") for line in lines),
                 f"the texel_size line: {[l for l in lines if 'TEXEL' in l]}")
        ed.check(any("[PIXELART-OFF_GRID]" in line and ": warning:" in line for line in lines), "WARN is a warning")
        ed.check("check_pixel_art '{\"fix\":[\"all\"],\"output\":true}'" in run.stdout, f"the text ends with the call that fixes: {run.stdout[-400:]}")

        ok = check(clean, "--pixel-art")
        ed.check_eq(ok.returncode, 0, f"the clean fixture exits 0: {ok.stdout[-800:]} {ok.stderr[-800:]}")
        ed.check("[PIXELART-" not in ok.stdout, "and prints no finding")

        js = check(bad, "--pixel-art", "--format", "json")
        out = json.loads(js.stdout)
        import jsonschema
        schema = json.loads((REPO / "tools" / "mcp" / "schemas" / "pixel_art_check.json").read_text())
        jsonschema.validate(out["pixel_art"], schema)
        ed.check(out["checked"].get("pixel_art", 0) > 0, f"the category counts what it checked: {out['checked']}")

        # Pixel Art Mode is on in the fixture: the default set has the category, and so has --hook
        default = check(clean)
        ed.check("pixel_art" in default.stderr, f"the default set includes pixel art in Pixel Art Mode: {default.stderr[-400:]}")
        def write_image(project: Path, name: str, importer: str) -> Path:
            png = project / "sprites" / name
            shutil.copy2(project / "sprites" / "hero.png", png)
            png.touch()
            guid = hashlib.md5(name.encode()).hexdigest()
            (project / "sprites" / f"{name}.meta").write_text(f"guid: {guid}\nimport:\n  textureImporter:\n    version: 1\n{importer}")
            return png

        def hook_write(project: Path, path: Path) -> subprocess.CompletedProcess:
            return check(project, "--hook", stdin=json.dumps({"tool_name": "Write", "tool_input": {"file_path": str(path)}}))

        # a first --hook run records what the earlier runs left (it reports files changed in the last minutes)
        for project in (clean, bad):
            (project / "notes.txt").write_text("hook state\n")
            hook_write(project, project / "notes.txt")

        # an image that is not pixel art (lossy, mipmapped, a normal map's import): not the pixel-art rules'
        normal = write_image(clean, "normal.png", "    type: normal\n    quality: high\n    mipmaps: true\n")
        hook = hook_write(clean, normal)
        ed.check("PIXELART-" not in hook.stderr, f"a changed image that is not pixel art reports nothing: {hook.stderr[-1500:]}")
        ed.check_eq(hook.returncode, 0, f"and exits 0: {hook.stderr[-1500:]}")
        # a pixel-art image with explicit mipmaps: its finding, exit 2
        lossy = write_image(clean, "lossy.png", "    type: sprite\n    pixelArt: true\n    mipmaps: true\n")
        hook = hook_write(clean, lossy)
        ed.check_eq(hook.returncode, 2, f"a FAIL finding exits 2 in --hook: {hook.stderr[-1500:]}")
        ed.check("PIXELART-MIPMAPS_COMPRESSION" in hook.stderr and "lossy.png" in hook.stderr, f"on stderr: {hook.stderr[-1500:]}")

        # the bad fixture's old FAILs are not the hook's business when a clean pixel-art image changes
        fresh = write_image(bad, "fresh.png", "    type: sprite\n    pixelArt: true\n")
        hook = hook_write(bad, fresh)
        ed.check("PIXELART-" not in hook.stderr, f"only the changed file's findings: {hook.stderr[-1500:]}")
        ed.check_eq(hook.returncode, 0, f"a clean change exits 0 among old FAILs: {hook.stderr[-1500:]}")
        # a changed scene: its own findings, no other scene's or texture's
        scene = bad / "scenes" / "Main.scene"
        scene.touch()
        hook = hook_write(bad, scene)
        lines = [line for line in hook.stderr.splitlines() if "[PIXELART-" in line]
        ed.check(lines, f"the changed scene's findings: {hook.stderr[-1500:]}")
        ed.check(all(line.startswith("scenes/Main.scene") for line in lines), f"and only those: {lines}")
    finally:
        shutil.rmtree(root, ignore_errors=True)
