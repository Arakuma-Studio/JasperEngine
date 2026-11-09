"""Shared by the S47 sprite drawing tests: calls that return (data, images) or the typed error."""

from __future__ import annotations

import base64
import hashlib
from pathlib import Path

from harness import Editor


async def enable(ed: Editor) -> None:
    await ed.call("tool_groups", enable=["sprites"])


async def raw(ed: Editor, tool: str, **args):
    return await ed.call_raw(tool, args)


async def ok(ed: Editor, tool: str, **args) -> dict:
    r = await ed.call_raw(tool, args)
    if r.is_error:
        ed.fail(f"{tool} {args}: {r.text}")
    return r.structured if r.structured is not None else r.value


async def err(ed: Editor, tool: str, **args) -> dict:
    """The typed error of a call that must fail"""
    r = await ed.call_raw(tool, args)
    ed.check(r.is_error, f"{tool} {args} must fail: {r.structured}")
    ed.check(isinstance(r.structured, dict) and "error" in r.structured, f"typed error: {r.text}")
    e = dict(r.structured["error"])
    e["text"] = r.text
    return e


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


async def images(ed: Editor, tool: str, **args):
    r = await ed.call_raw(tool, args)
    if r.is_error:
        ed.fail(f"{tool}: {r.text}")
    return r.structured, [img for img, _ in r.images]
