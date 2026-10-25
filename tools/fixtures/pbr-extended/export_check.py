"""AC15 (docs/specs/S11-materials-lighting.md): the HTML5 and Android exports of
tools/fixtures/pbr-extended-export carry every shader its scenes draw precompiled, so the game
compiles none at runtime (the project turns runtime shader compilation off: a missing one would be
an error, as tools/smoke-test.sh shows on GLES3).

For each of web and android: a source export (build_project mode source) of a copy of the
fixture, then each shader get_build_info lists (the scenes' collected keys: the built-in Lit's
lobe variants, Lit Quality, the mobile tier) is looked up in the export's GLSL ES 3.00 shader
header. Prints one line per platform; exit code 1 when a shader is missing.

    uv run --no-project --with-requirements tools/mcp/requirements.txt python tools/fixtures/pbr-extended/export_check.py
"""
import asyncio
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent.parent
sys.path.insert(0, str(REPO / "tools" / "mcp"))
from harness import Editor  # noqa: E402

FIXTURE = REPO / "tools" / "fixtures" / "pbr-extended-export"


async def wait_job(ed, job_id, timeout=3600):
    deadline = time.monotonic() + timeout
    while True:
        job = await ed.call("get_job", job_id=job_id, log_lines=10)
        if job["status"] != "running":
            return job
        if time.monotonic() > deadline:
            raise RuntimeError(f"job {job_id} still running after {timeout}s")
        await asyncio.sleep(5)


async def main() -> int:
    failed = False
    async with Editor(FIXTURE) as ed:
        for platform in ("web", "android"):
            info = await ed.call("get_build_info", include_shaders=True)
            # the variant names the exported headers hold (ShaderPool::getShaderStr)
            names = sorted({s["shader"] for s in info.get("shaders", [])})
            lit = [n for n in names if n.startswith("mesh_lit_")]
            out = Path(tempfile.mkdtemp(prefix=f"pbx-{platform}-"))
            raw = await ed.call_raw("build_project", {"mode": "source", "output_dir": str(out), "platform": platform,
                                                      "overwrite": True, "timeout_seconds": 1800})
            r = raw.value
            if isinstance(r, dict) and r.get("status") == "running":
                r = await wait_job(ed, r["job_id"])
            if not isinstance(r, dict) or r.get("status") != "succeeded":
                print(f"FAIL {platform}: export: {raw.text[:1500]}")
                failed = True
                continue
            headers = [p for p in out.rglob("glsl300es.h")]
            text = "\n".join(h.read_text(errors="ignore") for h in headers)
            missing = [n for n in names if n not in text]
            ok = headers and not missing
            failed |= not ok
            print(f"{'PASS' if ok else 'FAIL'} {platform}: {len(names)} shaders listed ({len(lit)} built-in Lit lobe/quality "
                  f"variants), {len(headers)} GLSL ES header(s); missing {missing[:8]}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
