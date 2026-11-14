"""check_shader reports a failed check as an MCP error (S98 REQ-22): the result an agent asked about
is a failure, and `isError` is how a client sees it; the CLI exits 1."""

from __future__ import annotations

import asyncio
import os
import subprocess
import tempfile
from pathlib import Path

import harness
from harness import Editor, free_port, wait_for_port

CI = True

PORT = free_port()
TOKEN = "s98-check-shader-token"
EDITOR = {"create": True, "name": "Shader checks", "extra_args": ["--http-port", str(PORT)],
          "settings": {"mcp": {"http_token": TOKEN}}}

VERT = "#version 450\n#include \"includes/surface_vs.glsl\"\n"
GOOD_FRAG = ("#version 450\n// @shader mesh\nout vec4 g_finalColor;\n#include \"includes/output.glsl\"\n"
             "void main() { g_finalColor = finalizeColor(vec4(1.0, 0.5, 0.25, 1.0)); }\n")
# a vec2 where the output takes a vec4: no overload of finalizeColor matches
BAD_FRAG = ("#version 450\n// @shader mesh\nout vec4 g_finalColor;\n#include \"includes/output.glsl\"\n"
            "void main() { vec2 uv = vec2(0.5); g_finalColor = finalizeColor(uv); }\n")


async def test_check_shader_failure_is_error(ed: Editor):
    for name, frag in (("Good", GOOD_FRAG), ("Bad", BAD_FRAG)):
        await ed.call("write_file", path=f"shaders/{name}.vert", content=VERT, timeout=300)
        await ed.call("write_file", path=f"shaders/{name}.frag", content=frag, timeout=300)

    good = await ed.call_raw("check_shader", {"shader": "shaders/Good", "shader_type": "mesh"}, timeout=300)
    ed.check(not good.is_error and good.value["failed"] == 0, f"a compiling shader is a success: {good.text[:800]}")

    bad = await ed.call_raw("check_shader", {"shader": "shaders/Bad", "shader_type": "mesh"}, timeout=300)
    ed.check(bad.is_error, f"a failed compile is an error: {bad.text[:800]}")
    data = bad.value
    ed.check_eq(data.get("failed"), 1, f"structuredContent keeps the result: {str(data)[:800]}")
    ed.check(isinstance(data.get("results"), list) and data["results"] and not data["results"][0]["ok"], f"results: {str(data)[:800]}")
    ed.check("findings" in data, "and the findings")
    ed.check("failed to compile" in bad.text and "FAILED" in bad.text, f"the same text: {bad.text[:800]}")

    # the CLI exits 1 on it, 0 on the compiling one
    await wait_for_port(PORT)
    root = Path(tempfile.mkdtemp(prefix="jasper-check-shader-cli-"))
    env = {k: v for k, v in os.environ.items() if k not in ("JASPER_MCP_URL", "JASPER_MCP_TOKEN", "DISPLAY", "XDG_CONFIG_HOME")}
    env["HOME"] = str(root)

    def cli(*args: str) -> subprocess.CompletedProcess:
        return subprocess.run(harness.editor_command() + ["call", "--port", str(PORT), "--token", TOKEN, *args], cwd=str(root),
                              env=env, capture_output=True, text=True, timeout=600)

    p = await asyncio.to_thread(cli, "check_shader", "--shader", "shaders/Bad", "--shader-type", "mesh")
    ed.check_eq(p.returncode, 1, f"the CLI exits 1 on a failed check: {p.stdout[-800:]} {p.stderr[-800:]}")
    p = await asyncio.to_thread(cli, "check_shader", "--shader", "shaders/Good", "--shader-type", "mesh")
    ed.check_eq(p.returncode, 0, f"and 0 on a passing one: {p.stdout[-800:]} {p.stderr[-800:]}")
