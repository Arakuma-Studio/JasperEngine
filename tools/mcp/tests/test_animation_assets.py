"""Animation clip assets on disk (animation clips spec 4.1, 9.R27): the file tools accept `.anim` and
`.poseanim`, and the automatic pick-up (McpAutoSync -> w4::syncFiles) reports their load problems in
`external_changes.problems` ({path, line, col?, severity?, message}), in edit mode and during Play
(clip and pose animation documents reload at once, they are not deferred)."""

import time
from pathlib import Path

from harness import Editor

TIMEOUT = 600

CLIP = """{
  "format": "jasperengine.anim",
  "version": 1,
  "frame_rate": 30,
  "wrap": "loop",
  "tracks": [
    {"path": "", "channel": "position", "keys": [
      [0, [0, 0, 0]],
      [1, [0, 1, 0]]
    ]}
  ],
  "events": []
}
"""

BROKEN_CLIP = '{\n  "format": "jasperengine.anim",\n  "version": 1,\n  "tracks": [\n    oops\n  ]\n}\n'
BROKEN_POSE = '{\n  "format": "jasperengine.poseanim",\n  nope\n}\n'


def external(r) -> dict | None:
    s = r.structured if isinstance(r.structured, dict) else {}
    return s.get("external_changes")


def write(path: Path, text: str) -> None:
    """A write whose mtime surely differs from the previous one (coarse-mtime filesystems)."""
    time.sleep(0.05)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def problems_of(ext: dict | None, rel: str) -> list:
    return [p for p in (ext or {}).get("problems", []) if p.get("path") == rel]


async def status(ed: Editor):
    r = await ed.call_raw("editor_status")
    ed.check(not r.is_error, f"editor_status failed: {r.text[:300]}")
    return r


async def test_file_tools_accept_clips_and_pose_animations(ed: Editor):
    ed.require_tools("read_file", "write_file")
    await status(ed)
    await ed.call("write_file", path="clips/walk.anim", content=CLIP)
    r = await ed.call_raw("read_file", {"path": "clips/walk.anim"})
    ed.check(not r.is_error, f"read_file refused .anim: {r.text[:300]}")
    ed.check('"jasperengine.anim"' in r.text, f"read_file text: {r.text[:300]}")
    await ed.call("write_file", path="clips/walk.anim", edits=[{"old_text": '"loop"', "new_text": '"once"'}])
    r = await ed.call_raw("read_file", {"path": "clips/walk.anim"})
    ed.check('"once"' in r.text, f"edit not applied: {r.text[:300]}")
    await ed.call("write_file", path="clips/pose.poseanim", content='{"format": "jasperengine.poseanim", "version": 1}\n')
    r = await ed.call_raw("read_file", {"path": "clips/pose.poseanim"})
    ed.check(not r.is_error, f"read_file refused .poseanim: {r.text[:300]}")
    ed.check("jasperengine.poseanim" in r.text, r.text[:300])


async def test_autosync_reports_clip_problems(ed: Editor):
    await status(ed)
    path = Path(ed.project) / "clips" / "broken.anim"
    # not JSON: an error at its line and column
    write(path, BROKEN_CLIP)
    ext = external(await status(ed))
    found = problems_of(ext, "clips/broken.anim")
    ed.check(found, f"no problem for the broken clip: {ext}")
    ed.check_eq(found[0].get("line"), 5, f"line of the syntax error: {found[0]}")
    ed.check(found[0].get("col", 0) > 0, f"column of the syntax error: {found[0]}")
    ed.check("Not a valid animation clip" in found[0].get("message", ""), f"message: {found[0]}")

    # lenient: a warning with its JSON path
    write(path, CLIP.replace('"wrap": "loop"', '"wrap": "sideways"'))
    ext = external(await status(ed))
    found = problems_of(ext, "clips/broken.anim")
    ed.check(found, f"no warning for the bad wrap: {ext}")
    ed.check(found[0].get("message", "").startswith("wrap:"), f"warning names its JSON path: {found[0]}")
    ed.check_eq(found[0].get("severity"), "warning", f"severity: {found[0]}")

    # a clean clip: nothing reported, and the asset database gave it a GUID
    write(path, CLIP)
    ext = external(await status(ed))
    ed.check(not problems_of(ext, "clips/broken.anim"), f"clean clip: {ext}")
    await ed.call("sync_files", paths=["clips/broken.anim"])
    # the asset database's refresh walks the disk as a Background job: its GUID line comes after
    deadline = time.monotonic() + 15
    while '"guid"' not in path.read_text() and time.monotonic() < deadline:
        time.sleep(0.05)
    ed.check('"guid"' in path.read_text(), f"no GUID after the refresh: {path.read_text()[:200]}")


async def test_autosync_reports_pose_animation_problems(ed: Editor):
    await status(ed)
    pose = Path(ed.project) / "clips" / "broken.poseanim"
    write(pose, BROKEN_POSE)
    ext = external(await status(ed))
    found = problems_of(ext, "clips/broken.poseanim")
    ed.check(found, f"no problem for the broken pose animation: {ext}")
    ed.check_eq(found[0].get("line"), 3, f"line: {found[0]}")
    ed.check("Not a valid pose animation" in found[0].get("message", ""), f"message: {found[0]}")
    write(pose, '{"format": "jasperengine.poseanim", "version": 1}\n')
    ext = external(await status(ed))
    ed.check(not problems_of(ext, "clips/broken.poseanim"), f"fixed pose animation: {ext}")


async def test_play_reports_clip_and_pose_problems_at_once(ed: Editor):
    """During Play scripts and shaders are deferred; clips and pose animations reload now."""
    ed.require_tools("play", "save_scene")
    await ed.call("save_scene")
    clip = Path(ed.project) / "clips" / "play.anim"
    pose = Path(ed.project) / "clips" / "play.poseanim"
    write(clip, CLIP)
    write(pose, '{"format": "jasperengine.poseanim", "version": 1}\n')
    await status(ed)
    r = await ed.call_raw("play", {"action": "start", "timeout_seconds": 300})
    ed.check(not r.is_error, f"play failed: {r.text[:500]}")
    try:
        write(clip, BROKEN_CLIP)
        write(pose, BROKEN_POSE)
        ext = external(await ed.call_raw("get_play_state"))
        ed.check(ext is not None, "edits during play not reported")
        ed.check(problems_of(ext, "clips/play.anim"), f"clip problem not reported during play: {ext}")
        ed.check(problems_of(ext, "clips/play.poseanim"), f"pose problem not reported during play: {ext}")
        ed.check("deferred" not in ext, f"clips and pose animations are not deferred: {ext}")
    finally:
        await ed.call("play", action="stop")
    write(clip, CLIP)
    write(pose, '{"format": "jasperengine.poseanim", "version": 1}\n')
    await status(ed)


async def test_cli_check_changes_no_clip():
    """`jasperengine-editor check --shaders` loads the project read-only: a clip without a GUID and
    a broken clip keep their bytes (no GUID upgrade, the broken one's error keeps its line)."""
    import shutil
    import subprocess
    import tempfile

    import harness

    tmp = Path(tempfile.mkdtemp(prefix="jasper-anim-check-"))
    try:
        project = tmp / "project"
        shutil.copytree(harness.default_fixture(), project, symlinks=True,
                        ignore=shutil.ignore_patterns(".git", "build"))
        good = project / "clips" / "plain.anim"
        broken = project / "clips" / "broken.anim"
        good.parent.mkdir(parents=True, exist_ok=True)
        good.write_text(CLIP)
        broken.write_text(BROKEN_CLIP)
        cmd = harness.editor_command() + ["check", "--project", str(project), "--shaders", "--anims"]
        r = subprocess.run(cmd, cwd=str(project), input="", capture_output=True, text=True, timeout=300)
        if good.read_text() != CLIP or broken.read_text() != BROKEN_CLIP:
            raise harness.TestFailure(f"check modified the clips (exit {r.returncode}):\n"
                                      f"plain: {good.read_text()[:300]}\nbroken: {broken.read_text()[:300]}\n"
                                      f"stderr: {r.stderr[-1500:]}")
        if "clips/broken.anim" not in r.stdout:
            raise harness.TestFailure(f"the broken clip is not reported:\n{r.stdout[-2000:]}\n{r.stderr[-1500:]}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
