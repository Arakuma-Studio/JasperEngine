#!/usr/bin/env python3
"""Open the upgrade-from-main fixture in the NEW editor and check that the game survives.

    uv run --no-project --with-requirements tools/mcp/requirements.txt \\
        python tools/ci/upgrade_from_main/check.py [--editor <editor>] [--out build/upgrade-from-main/check]

Run by check.sh. The fixture (tools/test-projects/upgrade-from-main: a game an origin/main
editor made, with its transcripts) is copied to <out>/project, then:

  1. first open: no error or warning in the Console; every scene opens with an empty load
     report; every prefab loads; every scene is saved; afterwards every scene, prefab, material
     and project.yaml carries the format version a new file of this editor gets; every asset
     has a .meta with a GUID of its own, and no reference is missing;
  2. second open: no error or warning, and saving everything again changes no file;
  3. play: the input run (keys through send_input) and the autopilot run (UPGRADE_AUTOPILOT=1)
     print the old editor's transcripts (compare.py, --tolerance), with no Lua error;
     screenshots at the same frames are compared with the old ones (reported, not judged:
     rendering defaults changed on purpose, docs/upgrading-from-main.md).

Writes <out>/report.json and <out>/report.md (what changed on the first open, the diffs) and
exits 1 when a check fails.
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

import yaml

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path.insert(0, str(REPO / "tools" / "mcp"))
sys.path.insert(0, str(HERE))

import compare  # noqa: E402
import harness  # noqa: E402
import playrun  # noqa: E402
import pngdiff  # noqa: E402

FIXTURE = REPO / "tools" / "test-projects" / "upgrade-from-main"
START = "scenes/Title.scene"
# screenshots are compared at this size (the canvas' 16:9)
SHOT_SIZE = (480, 270)
# Editor-local state, never committed: not part of what a first open "changes"
LOCAL = re.compile(r"^(\.jasperengine/|build/)")


class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.notes: dict[str, Any] = {}

    def check(self, ok: bool, what: str) -> bool:
        print(("ok:   " if ok else "FAIL: ") + what, flush=True)
        if not ok:
            self.failures.append(what)
        return ok


def snapshot(root: Path) -> dict[str, str]:
    out = {}
    for p in sorted(root.rglob("*")):
        if p.is_file():
            rel = p.relative_to(root).as_posix()
            if not LOCAL.match(rel):
                out[rel] = hashlib.sha1(p.read_bytes()).hexdigest()
    return out


def diff(a: dict[str, str], b: dict[str, str]) -> dict[str, list[str]]:
    return {"added": sorted(set(b) - set(a)), "removed": sorted(set(a) - set(b)),
            "changed": sorted(k for k in set(a) & set(b) if a[k] != b[k])}


# The hang watchdog's report (docs/crash-diagnostics.md) measures the build server's load, not
# the project: noted, not judged
ENVIRONMENT = ("The editor was not responding for",)


def problems_of(entries: list[dict], notes: list[str] | None = None) -> list[str]:
    out = []
    for e in entries:
        if e.get("type") not in ("warning", "error"):
            continue
        line = f"{e.get('type')}: {e.get('message', '')[:400]}"
        if any(marker in line for marker in ENVIRONMENT):
            if notes is not None:
                notes.append(line)
            continue
        out.append(line)
    return out


async def session(project: Path, cmd: list[str], **kw) -> harness.Editor:
    return harness.Editor(project=project, command=cmd, keep=True, call_timeout=900, **kw)


def copy_back(ed: harness.Editor, dst: Path, keep: Path) -> None:
    """The scratch copy the editor worked on becomes <dst> (the next session opens it); <keep>
    keeps this stage's state for the report."""
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(ed.scratch.project, dst, symlinks=True)
    shutil.copytree(dst, keep, symlinks=True, ignore=shutil.ignore_patterns(".jasperengine"))
    shutil.rmtree(ed.scratch.root, ignore_errors=True)


def yaml_version(path: Path, key: str) -> Any:
    try:
        data = yaml.safe_load(path.read_text())
    except yaml.YAMLError:
        return "unreadable"
    return data.get(key) if isinstance(data, dict) else None


# Files of a project that are not assets: settings, editor-written agent and tool files
NOT_ASSETS = re.compile(r"^(project\.yaml|AGENTS\.md|CLAUDE\.md|\.gitignore|\.gitattributes|\.clangd|\.luarc\.json|"
                        r"\.vscode/.*|.*\.meta)$")
# Assets that carry their GUID in the file instead of a .meta (S04)
IN_FILE_GUID = (".scene", ".prefab", ".material", ".anim")


def committed(root: Path) -> list[str]:
    """The files a user commits: what the project's .gitignore leaves in."""
    tmp = root.parent / (root.name + ".git-probe")
    if tmp.exists():
        shutil.rmtree(tmp)
    subprocess.run(["git", "init", "-q", "--separate-git-dir", str(tmp), str(root)], check=True)
    try:
        out = subprocess.run(["git", "-C", str(root), "ls-files", "-co", "--exclude-standard", "-z"], check=True,
                             capture_output=True).stdout.decode()
    finally:
        (root / ".git").unlink()
        shutil.rmtree(tmp)
    return sorted(f for f in out.split("\0") if f)


def asset_guids(root: Path, report: Report) -> dict[str, str]:
    """{asset: GUID} of every committed asset, from its .meta or (scenes, prefabs, materials,
    clips) its own file; checks each has one of its own."""
    guids: dict[str, str] = {}
    owner: dict[str, str] = {}
    missing = []
    for rel in committed(root):
        if NOT_ASSETS.match(rel):
            continue
        path = root / rel
        guid = ""
        if rel.endswith(IN_FILE_GUID):
            text = path.read_text()
            data = json.loads(text) if text.lstrip().startswith("{") else yaml.safe_load(text)
            guid = str((data or {}).get("guid", "")) if isinstance(data, dict) else ""
        else:
            meta = path.with_name(path.name + ".meta")
            if meta.exists():
                guid = str((yaml.safe_load(meta.read_text()) or {}).get("guid", ""))
        if not guid:
            missing.append(rel)
            continue
        report.check(guid not in owner, f"{rel}: GUID {guid}" + (f" (also {owner[guid]})" if guid in owner else ""))
        owner[guid] = rel
        guids[rel] = guid
    report.check(not missing, "every asset has a GUID (.meta or in its file)" + ("" if not missing else f": none for {missing}"))
    return guids


async def fresh_versions(cmd: list[str], out: Path) -> dict[str, Any]:
    """The format versions this editor writes into new files (a scratch project)."""
    async with harness.Editor(create=True, name="Versions", command=cmd, call_timeout=900) as ed:
        await ed.call("create_scene", path="scenes/New.scene", type="3D")
        await ed.call("create_entity", scene="scenes/New.scene", type="cube", name="Box")
        await ed.call("create_prefab", scene="scenes/New.scene", entity="Box", path="prefabs/Box.prefab")
        await ed.call("create_material", path="assets/New.material", values={"roughnessFactor": 0.5})
        await ed.call("save_scene", all=True)
        await ed.call("save_project")
        p = ed.project
        return {"scene": yaml_version(p / "scenes/New.scene", "formatVersion"),
                "prefab": yaml_version(p / "prefabs/Box.prefab", "version"),
                "material": yaml_version(p / "assets/New.material", "formatVersion"),
                "project": yaml_version(p / "project.yaml", "formatVersion")}


async def open_and_save(ed: harness.Editor, report: Report, label: str) -> None:
    scenes = await ed.call("list_scenes")
    names = [s.get("path") or s.get("name") for s in harness.find_key(scenes, "scenes") or []]
    report.notes[f"{label}_scenes"] = names
    for scene in names:
        await ed.call("open_scene", scene=scene)
        # as a user looks at it before saving: the Scene view's screenshot waits for its content
        # to load (a save while a model still loads writes its submeshes' vertex counts as they
        # are then, on origin/main too)
        await ed.screenshot("scene", scene=scene, max_width=320, timeout_seconds=300)
        load = await ed.call("get_load_report", scene=scene)
        issues = harness.find_key(load, "issues") or []
        # a migration is reported (info): what it did goes into the report; anything else fails
        migrated = [i for i in issues if i.get("kind") == "migrated" and i.get("fate") == "info"]
        others = [i for i in issues if i not in migrated]
        if migrated:
            report.notes.setdefault(f"{label}_migrations", {})[scene] = [i.get("message") for i in migrated]
        report.check(not others, f"{label}: {scene} loads with nothing but migrations in its load report"
                     + ("" if not others else f" ({json.dumps(others)[:600]})"))
    prefabs = await ed.call("list_prefabs")
    paths = [p.get("path") for p in harness.find_key(prefabs, "prefabs") or [] if p.get("path")]
    report.notes[f"{label}_prefabs"] = paths
    for prefab in paths:
        info = await ed.call_raw("get_prefab_info", {"prefab": prefab})
        report.check(not info.is_error, f"{label}: prefab {prefab} loads" + (f" ({info.text[:300]})" if info.is_error else ""))
    for scene in names:
        await ed.call("save_scene", scene=scene)
    await ed.call("save_scene", all=True)
    await ed.call("save_project")


async def check_cpp(cmd: list[str], fixture: Path, out: Path, report: Report) -> dict:
    """The C++ script fixture builds with this engine's headers and prints the old transcript."""
    ed = harness.Editor(project=fixture / "cpp-project", command=cmd, call_timeout=3600)
    result: dict[str, Any] = {}
    async with ed:
        await ed.call("open_scene", scene="scenes/Main.scene")
        before = await ed.call("read_console", limit=1)
        started = await ed.call_raw("play", {"action": "start", "scene": "scenes/Main.scene", "fixed_fps": 60,
                                              "auto_pause": True, "timeout_seconds": 3600})
        result["play"] = "ok" if not started.is_error else started.text[:3000]
        if not started.is_error:
            await ed.call("wait", frames=130)
            await ed.call("play", action="stop", timeout_seconds=600)
        entries, _ = await playrun.console_all(ed, before.get("next_cursor"))
        lines = playrun.transcript_lines(entries)
        (out / "cpp.txt").write_text("\n".join(lines) + "\n")
        result["errors"] = [e.get("message", "")[:600] for e in entries if e.get("type") in ("error", "build")][:40]
    ref = compare.read(fixture / "transcripts" / "cpp-editor.txt")
    diffs, worst = compare.compare(ref, lines, 0.0)
    result["transcript_lines"] = len(lines)
    result["differences"] = diffs[:20]
    report.check(not started.is_error and not diffs,
                 f"C++ script fixture: builds, plays and prints the old transcript ({len(lines)}/{len(ref)} lines"
                 + ("" if not diffs else ", differing:\n" + "\n".join(diffs[:10])) + ")")
    return result


async def run(editor: Path, fixture: Path, out: Path, tolerance: float = 0.0,
              modes: tuple[str, ...] = ("input", "autopilot"), cpp: bool = True) -> Report:
    """Every check above against `fixture`, in `out` (emptied first); returns the report (its
    failures empty when the upgrade passed). `modes`: the play runs to compare."""
    cmd = [str(Path(editor).resolve())]
    out = out.resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    report = Report()
    work = out / "project"
    shutil.copytree(fixture / "project", work, symlinks=True)
    snap0 = snapshot(work)

    expected = await fresh_versions(cmd, out)
    report.notes["new_file_versions"] = expected

    # 1. first open
    ed = await session(work, cmd)
    async with ed:
        entries, _ = await playrun.console_all(ed, None)
        environment: list[str] = []
        report.notes["environment"] = environment
        problems = problems_of(entries, environment)
        report.notes["first_open_console"] = problems
        report.check(not problems, "first open: no error or warning in the Console"
                     + ("" if not problems else ":\n    " + "\n    ".join(problems[:20])))
        cursor = (await ed.call("read_console", limit=1)).get("next_cursor")
        await open_and_save(ed, report, "first open")
        missing = await ed.call("list_missing_assets")
        report.notes["missing_assets_first_open"] = missing
        entries, _ = await playrun.console_all(ed, cursor)
        problems = problems_of(entries, environment)
        report.check(not problems, "first open: opening and saving every scene logs no error or warning"
                     + ("" if not problems else ":\n    " + "\n    ".join(problems[:20])))
    copy_back(ed, work, out / "after-first-open")
    snap1 = snapshot(work)
    first = diff(snap0, snap1)
    report.notes["first_open_changes"] = first

    # formats, metas, GUIDs, references
    for scene in sorted(work.rglob("*.scene")):
        v = yaml_version(scene, "formatVersion")
        report.check(v == expected["scene"], f"{scene.relative_to(work)}: scene format {v} (new scenes: {expected['scene']})")
    for prefab in sorted(work.rglob("*.prefab")):
        v = yaml_version(prefab, "version")
        report.check(v == expected["prefab"], f"{prefab.relative_to(work)}: prefab version {v} (new prefabs: {expected['prefab']})")
    # a material is written only when it changes; an unversioned one (format 0) reads as it is:
    # its one migration (0 -> 1) only stamps the version (editor/StreamMigrations.cpp)
    for material in sorted(work.rglob("*.material")):
        v = yaml_version(material, "formatVersion")
        report.check(v in (expected["material"], None),
                     f"{material.relative_to(work)}: material format {v or 0} (new: {expected['material']}; 0 reads unchanged)")
    v = yaml_version(work / "project.yaml", "formatVersion")
    report.check(v == expected["project"], f"project.yaml: format {v} (new: {expected['project']})")
    guids = asset_guids(work, report)
    report.notes["guids"] = len(guids)

    # 2. second open: stable
    ed = await session(work, cmd)
    async with ed:
        entries, _ = await playrun.console_all(ed, None)
        problems = problems_of(entries, environment)
        report.check(not problems, "second open: no error or warning in the Console"
                     + ("" if not problems else ":\n    " + "\n    ".join(problems[:20])))
        await open_and_save(ed, report, "second open")
        missing = await ed.call("list_missing_assets")
        report.notes["missing_assets"] = missing
        gone = harness.find_key(missing, "missing") or []
        by_path = harness.find_key(missing, "resolved_by_path") or []
        orphans = harness.find_key(missing, "orphan_metas") or []
        report.check(not gone, "every asset reference resolves" + ("" if not gone else f": {json.dumps(gone)[:800]}"))
        report.check(not by_path, "every asset reference carries a GUID"
                     + ("" if not by_path else f": bound by path only {json.dumps(by_path)[:800]}"))
        report.check(not orphans, "no orphan .meta" + ("" if not orphans else f": {orphans}"))
    copy_back(ed, work, out / "after-second-open")
    snap2 = snapshot(work)
    again = asset_guids(work, Report())
    moved = sorted(k for k in set(guids) | set(again) if guids.get(k) != again.get(k))
    report.check(not moved, "every GUID is the same after the second open" + ("" if not moved else f": {moved}"))
    second = diff(snap1, snap2)
    report.notes["second_open_changes"] = second
    report.check(not any(second.values()), "second open and save rewrites nothing"
                 + ("" if not any(second.values()) else f": {second}"))

    # 3. play
    shots_dir = out / "screenshots"
    for mode in modes:
        env = {"UPGRADE_AUTOPILOT": "1"} if mode == "autopilot" else {}
        ed = harness.Editor(project=work, command=cmd, env=env, call_timeout=900)
        async with ed:
            run = await playrun.play(ed, shots_dir, mode, START)
        lua_errors = [p for p in run["problems"] if p.get("type") == "error"]
        report.check(not lua_errors, f"play ({mode}): no error in the Console"
                     + ("" if not lua_errors else f": {json.dumps(lua_errors)[:1200]}"))
        report.notes[f"play_{mode}_warnings"] = [p.get("message", "")[:300] for p in run["problems"]]
        ref = fixture / "transcripts" / f"editor-{mode}.txt"
        ref_lines = compare.read(ref)
        diffs, worst = compare.compare(ref_lines, run["lines"], tolerance)
        report.notes[f"transcript_{mode}"] = {"lines": len(run["lines"]), "reference_lines": len(ref_lines),
                                             "largest_decimal_difference": worst, "differences": diffs[:40]}
        report.check(not diffs, f"play ({mode}): the transcript matches the old editor's ({len(ref_lines)} lines, "
                     f"largest decimal difference {worst:g})" + ("" if not diffs else "\n" + "\n".join(diffs[:15])))
        for name in run["shots"]:
            old = fixture / "screenshots" / name
            if not old.exists():
                continue
            a, b = pngdiff.load(old), pngdiff.load(shots_dir / name)
            sizes = f"{a[0]}x{a[1]} -> {b[0]}x{b[1]}"
            # both at the project's aspect (playrun): compared at one size, the Game view's size
            # following each editor's layout
            a, b = pngdiff.downscale(a, *SHOT_SIZE), pngdiff.downscale(b, *SHOT_SIZE)
            ok, fraction, diff_png = pngdiff.compare(a, b)
            (shots_dir / name.replace(".png", ".diff.png")).write_bytes(diff_png)
            report.notes.setdefault("screenshots", {})[name] = {"same": ok, "differing_fraction": round(fraction, 4),
                                                                "sizes": sizes}
            print(f"note: screenshot {name} ({sizes}): {fraction:.2%} of the pixels differ", flush=True)
    if cpp and (fixture / "cpp-project").is_dir():
        report.notes["cpp"] = await check_cpp(cmd, fixture, out, report)
    snap3 = snapshot(work)
    played = diff(snap2, snap3)
    report.check(not any(played.values()), "playing changes no project file"
                 + ("" if not any(played.values()) else f": {played}"))

    (out / "report.json").write_text(json.dumps({"failures": report.failures, **report.notes}, indent=1) + "\n")
    md = ["# Upgrade check", "", f"Fixture: {fixture}", ""]
    md += ["## First open changes", "", "```", json.dumps(first, indent=1), "```", ""]
    md += ["## Failures", ""] + [f"- {f}" for f in report.failures] + [""]
    (out / "report.md").write_text("\n".join(md))
    print(f"\n{'PASS' if not report.failures else 'FAIL'}: {len(report.failures)} failure(s); report {out}/report.md")
    return report


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--editor", default=str(REPO / "build/linux/editor-install/bin/jasperengine-editor"))
    ap.add_argument("--fixture", type=Path, default=FIXTURE)
    ap.add_argument("--out", type=Path, default=REPO / "build/upgrade-from-main/check")
    ap.add_argument("--tolerance", type=float, default=0.0,
                    help="largest difference allowed between decimals of the transcripts")
    args = ap.parse_args()
    report = await run(Path(args.editor), args.fixture, args.out, args.tolerance)
    return 0 if not report.failures else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
