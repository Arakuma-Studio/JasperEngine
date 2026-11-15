"""What a build writes (docs/specs/S98-editor-tooling-gaps.md REQ-4, REQ-5; AC5, AC6).

- A build writes nothing in the project but the editor's .jasperengine/: every other file hashes
  the same after a source export with an open scene and a Prefab Mode stage both modified; each is
  named by W-SCENE-UNSAVED, and the cooked scene and prefab hold what the editor shows.
- Two source exports of the AllComponents fixture into fresh folders are byte-identical.

The same holds for prebuilt builds: with JASPER_SLOW_BUILD=1 both tests build with the run's prebuilt
glcore runtime too (tools/mcp/build_cache.py; docs/testing.md "Compiled and prebuilt game builds").
"""

from __future__ import annotations

import hashlib
import os
from pathlib import Path

import build_cache
from harness import Editor

CI = True
REPO = Path(__file__).resolve().parents[3]
PREBUILT_PLAYERS = ["glcore-release"]
EDITOR = {"create": True, "name": "Export Sources", "env": build_cache.player_env("glcore", "release")}


def slow() -> bool:
    return os.environ.get("JASPER_SLOW_BUILD", "") not in ("", "0")


def hashes(root: Path, skip_internal: bool = True) -> dict[str, str]:
    """{relative path: sha256} of every file below root (the editor's .jasperengine/ left out)"""
    out = {}
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root)
        if not path.is_file() or (skip_internal and rel.parts and rel.parts[0] == ".jasperengine") or path.name == "editor.lock":
            continue
        out[rel.as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return out


def changed(before: dict[str, str], after: dict[str, str]) -> list[str]:
    return sorted(name for name in set(before) | set(after) if before.get(name) != after.get(name))


def warning_messages(job: dict, code: str) -> list[str]:
    return [w.get("message", "") for w in job.get("result", {}).get("warnings", []) if isinstance(w, dict) and w.get("code") == code]


async def build(ed: Editor, out: Path, **args) -> dict:
    r = await ed.call("build_project", output_dir=str(out), allow_missing_assets=True, timeout_seconds=900, **args)
    ed.check_eq(r["status"], "succeeded", f"{str(r)[:2000]}")
    return r


async def test_export_writes_no_source(ed: Editor):
    sid = (await ed.call("create_scene", name="Level", type="3D", path="scenes/Level.scene"))["scene"]["id"]
    await ed.call("create_entity", scene=sid, type="cube", name="Saved Cube", select=False)
    crate = (await ed.call("create_entity", scene=sid, type="sphere", name="Crate", select=False))["entity_id"]
    await ed.call("create_prefab", scene=sid, entity=crate, path="prefabs/Crate.prefab")
    await ed.call("save_scene", scene=sid)
    await ed.call("edit_build_scenes", action="add", scene=sid)
    await ed.call("edit_build_scenes", action="set_start", scene=sid)
    await ed.call("save_project")

    # the open scene modified, and the prefab's Prefab Mode with an edit not saved yet
    await ed.call("create_entity", scene=sid, type="cube", name="Unsaved Cube", select=False)
    stage = (await ed.call("prefab_mode", action="open", prefab="prefabs/Crate.prefab", auto_save=False, in_context=False))
    stage_id = stage.get("scene_id") or stage.get("scene", {}).get("id")
    ed.check(stage_id, f"the stage's scene: {stage}")
    await ed.call("create_entity", scene=stage_id, parent="Crate", type="cube", name="Stage Lid", select=False)
    builds = [("source", dict(mode="source", platform="linux", backends=["opengl"]))]
    if slow():
        await build_cache.player_runtime(ed, "glcore", "release")
        builds.insert(0, ("prebuilt", dict(mode="desktop", method="prebuilt")))
    # build_project remembers the graphics API it is given in project.yaml (Build Settings does the
    # same): a first build of each kind takes that, the builds checked change no setting
    for label, args in builds:
        await build(ed, ed.scratch.root / f"settings-{label}", **args)
    before = hashes(ed.project)
    for label, args in builds:
        out = ed.scratch.root / f"no-source-{label}"
        job = await build(ed, out, **args)
        ed.check_eq(changed(before, hashes(ed.project)), [], f"{label}: the build wrote no project file")
        unsaved = warning_messages(job, "W-SCENE-UNSAVED")
        ed.check_eq(len(unsaved), 2, f"{label}: the scene and the prefab are named: {unsaved}")
        ed.check(any("Level" in m for m in unsaved) and any("Crate.prefab" in m for m in unsaved), f"{label}: {unsaved}")
        content = out / "project" / "content" if label == "source" else out
        ed.check_in(b"Unsaved Cube", (content / "scenes" / f"{sid}.jscn").read_bytes(), f"{label}: the cooked scene is what the editor shows")
        ed.check(any(b"Stage Lid" in p.read_bytes() for p in (content / "prefabs").glob("*.jpfb")),
                 f"{label}: the cooked prefab is what Prefab Mode shows")

    # the editor still holds the edits, and the files are as they were saved
    ed.check(b"Unsaved Cube" not in (ed.project / "scenes" / "Level.scene").read_bytes(), "the scene's file stays as saved")
    ed.check(b"Stage Lid" not in (ed.project / "prefabs" / "Crate.prefab").read_bytes(), "the prefab's file stays as saved")
    await ed.call("prefab_mode", action="close", unsaved="discard")


async def test_export_is_deterministic():
    """Two builds of the AllComponents fixture (every component type) into fresh folders: the same bytes"""
    async with Editor(project=str(REPO / "tools" / "fixtures" / "reflection-parity"), label="deterministic",
                      env=build_cache.player_env("glcore", "release")) as ed:
        scenes = (await ed.call("list_scenes"))["scenes"]
        ed.check(scenes, "the fixture has its scene")
        builds = [("source", dict(mode="source", platform="linux", backends=["opengl"]))]
        if slow():
            await build_cache.player_runtime(ed, "glcore", "release")
            builds.append(("prebuilt", dict(mode="desktop", method="prebuilt")))
        for label, args in builds:
            first, second = ed.scratch.root / f"{label}-1", ed.scratch.root / f"{label}-2"
            await build(ed, first, **args)
            await build(ed, second, **args)
            a, b = hashes(first, skip_internal=False), hashes(second, skip_internal=False)
            # the build's own log names its folder; the rest is the game
            stamps = {name for name in a if name.endswith(".log")}
            differ = [name for name in changed(a, b) if name not in stamps]
            ed.check_eq(differ, [], f"{label}: two builds of an unchanged project give the same bytes")
            ed.check(any(name.endswith(".jscn") for name in a), f"{label}: the cooked scene is compared: {len(a)} files")
