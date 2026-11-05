"""The checks of the project's shader list (docs/specs/S64-shader-lists.md): what get_build_info,
generate_shaders and an export each name, for tests/test_shader_lists*.py."""

from __future__ import annotations

import asyncio
import re
import time
from pathlib import Path

from harness import Editor


async def finish_job(ed: Editor, job_id: str, deadline: float = 1800.0) -> dict:
    """get_job until it ends; the deadline only guards a hang."""
    end = time.monotonic() + deadline
    while True:
        job = await ed.call("get_job", job_id=job_id)
        if job["status"] != "running":
            return job
        ed.check(time.monotonic() < end, f"job {job_id} still running: {job}")
        await asyncio.sleep(0.2)


async def build_info(ed: Editor) -> dict:
    """get_build_info once its list is complete (the job it names, when past its budget)."""
    while True:
        info = await ed.call("get_build_info", include_shaders=True)
        if info.get("shaders_complete", True):
            return info
        job = await finish_job(ed, info["shaders_job_id"])
        ed.check_eq(job["status"], "succeeded", f"{job}")


def listed(info: dict) -> set[str]:
    return {s["shader"] for s in info["shaders"] if not s.get("excluded")}


async def generated(ed: Editor, out: Path) -> tuple[set[str], dict]:
    """What generate_shaders builds by default, for OpenGL."""
    r = await ed.call("generate_shaders", output_dir=str(out), backends=["opengl"], format="binary", timeout_seconds=1800)
    if r["status"] == "running":
        r = await finish_job(ed, r["job_id"])
    ed.check_eq(r["status"], "succeeded", f"{r}")
    return {p.name[:-len("_glsl410.sdat")] for p in out.rglob("*_glsl410.sdat")}, r


async def exported(ed: Editor, out: Path) -> tuple[set[str], dict]:
    """The shaders a source export really builds for OpenGL, from its shader header."""
    r = await ed.call("build_project", mode="source", output_dir=str(out), backends=["opengl"], overwrite=True,
                      timeout_seconds=1800)
    if r["status"] == "running":
        r = await finish_job(ed, r["job_id"])
    ed.check_eq(r["status"], "succeeded", f"{r}")
    headers = list(out.rglob("glsl410.h"))
    ed.check(headers, "the export wrote its OpenGL shader header")
    names: set[str] = set()
    for header in headers:
        names |= set(re.findall(r'name == "(.+?)_glsl410"', header.read_text(errors="replace")))
    return names, r


async def check_lists_match(ed: Editor) -> None:
    info = await build_info(ed)
    names = listed(info)
    ed.check_eq(info["shader_count"], len(names), "shader_count counts the list")
    ed.check(names, "the project needs shaders")

    from_tool, _ = await generated(ed, ed.scratch.root / "generated-shaders")
    ed.check_eq(sorted(names - from_tool), [], "get_build_info lists what generate_shaders does not build")
    ed.check_eq(sorted(from_tool - names), [], "generate_shaders builds what get_build_info does not list")

    from_export, _ = await exported(ed, ed.scratch.root / "export")
    ed.check_eq(sorted(from_export - names), [], "the export ships shaders get_build_info does not list")
    ed.check_eq(sorted(names - from_export), [], "get_build_info lists shaders the export does not ship")
