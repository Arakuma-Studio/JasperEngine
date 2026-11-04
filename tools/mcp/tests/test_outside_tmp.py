"""A project outside the system temporary folder: the strict MCP write scope's refusal path is
exercised for real (the temporary folder is one of the allowed outside folders, so projects under
it hide refusals). Legitimate writes work: the editor's config, logs and workspace, files and a
screenshot inside the project, a save, a build into the default export location."""

import os
from pathlib import Path

from harness import Editor

BASE = "/var/tmp" if os.path.isdir("/var/tmp") and os.access("/var/tmp", os.W_OK) else None
EDITOR = {"create": True, "name": "Outside Tmp", "scratch_prefix": (BASE + "/jasper-mcp-outside-") if BASE else "jasper-mcp-outside-"}
CI = True
TIMEOUT = 900


async def test_legitimate_writes_work_outside_tmp(ed: Editor):
    summary = await ed.call("get_project_summary")
    project = Path(summary["path"])
    ed.check(BASE is None or str(project).startswith(BASE), f"the project is outside /tmp: {project}")
    await ed.call("write_file", path="notes/hello.txt", content="hello\n")
    ed.check_eq((project / "notes" / "hello.txt").read_text(encoding="utf-8"), "hello\n")
    shot = await ed.call_raw("screenshot", {"view": "scene", "save_path": "shots/scene.png"})
    ed.check(not shot.is_error, f"a screenshot saved inside the project: {shot.text[:300]}")
    ed.check((project / "shots" / "scene.png").is_file(), "the screenshot file")
    saved = await ed.call_raw("save_project", {})
    ed.check(not saved.is_error, f"save_project: {saved.text[:300]}")
    ed.check((project / "project.yaml").is_file(), "project.yaml written")
    # the editor's own folders outside the project (its config under HOME, its logs) keep working
    r = await ed.call_raw("set_project_settings", {"values": {"player.product_name": "Outside"}})
    ed.check(not r.is_error, f"a project setting: {r.text[:300]}")
    # a build into the default export location (the scratch dir here, outside the project)
    r = await ed.call_raw("build_project", {"mode": "source", "platform": "linux", "timeout_seconds": 600})
    ed.check(not r.is_error, f"a build into the default export folder: {r.text[:400]}")
    out = Path(r.structured["result"]["output_dir"])
    ed.check((out / "CMakeLists.txt").is_file(), f"exported into {out}")
    # and a write outside the project and the editor's folders is still refused
    r = await ed.call_raw("write_file", {"path": "../escape.txt", "content": "x\n"})
    ed.check(r.is_error, "outside the project")
