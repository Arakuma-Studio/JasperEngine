"""S07 R31: the new ECS saves scenes and prefabs exactly as the editor did before it (entity ids,
hierarchy, sibling and component order unchanged).

Every project of tools/fixtures and the MCP fixture is copied, loaded and saved with
`jasperengine-editor scene-dump --project <copy> --save` (every scene); each prefab is opened in
Prefab Mode and saved. The fixtures are hand-written, so the first save writes the editor's full
form; checked:

- with JASPER_BASELINE_EDITOR (an editor built from the revision before S07, e.g. oe-port), the
  saved files are byte-identical to what that editor saves from the same files, and so are the
  files saved again from those;
- without it, the saved files load and save again byte-identical (stable; for a prefab but its
  nextEntity counter, which each Prefab Mode session raises, before S07 as after it).

(StreamRoundTripTest.ExampleScenesByteIdentical of the spec: the editor has no unit-test target
for Stream yet, so the round trip runs through the editor binaries.)
"""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
from pathlib import Path

import harness
from harness import Editor, TestFailure

TIMEOUT = 3600

REPO = Path(__file__).resolve().parents[3]


def check(cond, message: str) -> None:
    if not cond:
        raise TestFailure(message)


def baseline_command() -> list[str] | None:
    value = os.environ.get("JASPER_BASELINE_EDITOR")
    return shlex.split(value) if value else None


def projects() -> list[Path]:
    found = sorted(p.parent for p in (REPO / "tools" / "fixtures").glob("*/project.yaml"))
    found.append(harness.default_fixture())
    return found


def scratch_copy(project: Path, target: Path) -> Path:
    shutil.copytree(project, target, symlinks=True,
                    ignore=lambda directory, names: ({"build", "export"} if Path(directory).name == ".jasperengine" else {".git"}) & set(names))
    return target


def env(tmp: Path) -> dict[str, str]:
    e = dict(os.environ)
    for k in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR"):
        e.pop(k, None)
    (tmp / "home").mkdir(exist_ok=True)
    e["HOME"] = str(tmp / "home")
    e["LIBGL_ALWAYS_SOFTWARE"] = "1"
    return e


def save_scenes(command: list[str], project: Path, tmp: Path) -> None:
    r = subprocess.run(command + ["scene-dump", "--project", str(project), "--out", str(tmp / "dump.yaml"), "--save"],
                       env=env(tmp), capture_output=True, text=True, timeout=900)
    check(r.returncode == 0, f"scene-dump --save {project}: exit {r.returncode}\n{r.stderr[-3000:]}")


def files(root: Path, pattern: str) -> list[str]:
    return sorted(p.relative_to(root).as_posix() for p in root.rglob(pattern) if ".jasperengine" not in p.relative_to(root).parts)


def differing(a: Path, b: Path, rels: list[str]) -> list[str]:
    return [rel for rel in rels if (a / rel).read_bytes() != (b / rel).read_bytes()]


async def test_example_scenes_byte_identical():
    tmp = Path(tempfile.mkdtemp(prefix="jasper-ecs-roundtrip-"))
    ours = harness.editor_command()
    baseline = baseline_command()
    try:
        checked = 0
        unstable, changed = [], []
        for index, project in enumerate(projects()):
            scenes = files(project, "*.scene")
            if not scenes:
                continue
            first = scratch_copy(project, tmp / f"{index}-first")
            save_scenes(ours, first, tmp)
            second = scratch_copy(first, tmp / f"{index}-second")
            save_scenes(ours, second, tmp)
            unstable += [f"{project.name}/{rel}" for rel in differing(first, second, scenes)]
            if baseline:
                before = scratch_copy(project, tmp / f"{index}-baseline")
                save_scenes(baseline, before, tmp)
                changed += [f"{project.name}/{rel}" for rel in differing(first, before, scenes)]
            checked += len(scenes)
        check(checked > 0, "no scene was checked")
        check(not unstable, "scenes not stable across a second load and save: " + ", ".join(unstable))
        check(not changed, "scenes saved differently than before S07: " + ", ".join(changed))
        print(f"[roundtrip] {checked} scenes stable" + (", identical to the baseline editor" if baseline else
                                                      " (set JASPER_BASELINE_EDITOR to compare with the editor before S07)"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def without_counter(data: bytes) -> bytes:
    return b"\n".join(line for line in data.split(b"\n") if not line.startswith(b"nextEntity:"))


async def save_prefabs(project: Path, prefabs: list[str], command: list[str] | None) -> dict[str, bytes]:
    saved = {}
    # the harness opens a scratch copy of the project
    async with Editor(project=project, command=command) as ed:
        for prefab in prefabs:
            await ed.call("prefab_mode", action="open", prefab=prefab, auto_save=False)
            await ed.call("prefab_mode", action="save")
            await ed.call("prefab_mode", action="close")
        for prefab in prefabs:
            saved[prefab] = (ed.project / prefab).read_bytes()
    return saved


async def test_example_prefabs_byte_identical():
    tmp = Path(tempfile.mkdtemp(prefix="jasper-ecs-roundtrip-prefabs-"))
    baseline = baseline_command()
    try:
        checked = 0
        unstable, changed = [], []
        for index, project in enumerate(projects()):
            prefabs = files(project, "*.prefab")
            if not prefabs:
                continue
            first = await save_prefabs(project, prefabs, None)
            resaved = scratch_copy(project, tmp / f"{index}-first")
            for rel, data in first.items():
                (resaved / rel).write_bytes(data)
            second = await save_prefabs(resaved, prefabs, None)
            if baseline:
                # both passes as the editor before S07 makes them (a second save of a prefab may
                # differ from the first there too: then it must differ the same way)
                before = await save_prefabs(project, prefabs, baseline)
                before_again = await save_prefabs(resaved, prefabs, baseline)
                changed += [f"{project.name}/{rel}" for rel in prefabs
                            if first[rel] != before[rel] or second[rel] != before_again[rel]]
            else:
                # nextEntity (the next id the prefab hands out) grows by one per Prefab Mode
                # session, before S07 as after it; everything else must stay
                unstable += [f"{project.name}/{rel}" for rel in prefabs if without_counter(first[rel]) != without_counter(second[rel])]
            checked += len(prefabs)
        check(checked > 0, "no prefab was checked")
        check(not unstable, "prefabs not stable across a second load and save: " + ", ".join(unstable))
        check(not changed, "prefabs saved differently than before S07: " + ", ".join(changed))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
