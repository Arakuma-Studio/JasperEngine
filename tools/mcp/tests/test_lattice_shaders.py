"""Compile the documented lattice vertex pipelines on every supported backend.

The feature combinations come from lattice design §2.1/§5: skinning with morphs,
instancing with vertex-color masks, and sprite UV rectangles in color/depth passes.
Use valid layouts below the vertex-attribute limit, so any compiler error fails.
"""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

import harness
from harness import TestFailure

TIMEOUT = 900
CI = True

# Deliberately selected independent shader contracts, rather than a Cartesian
# product containing thousands of layouts that exceed the attribute budget.
VARIANTS = (
    "mesh:Ult",
    "mesh:Ult,Ski,Mta,Mnr,Mtg,Nor,Tan,Nmp,Uv1,Vc4",
    "mesh:Puc,Shw,Ist,Ifd,Nor,Tan,Nmp,Uv1,Txr,Vc3",
    "mesh:Vtl,Nor,Uv1,Uv2",
    "depth",
    "depth:Tex,Ams,Ski,Mta,Mnr,Mtg,Uv1,Nor,Vc4",
    "depth:Tex,Ist,Ifd,Uv1,Txr,Vc3",
    "gbuffer",
    "gbuffer:Bct,Mrt,Nor,Ski,Mta,Mnr,Mtg,Vc4",
    "gbuffer:Bct,Nor,Ist,Vc3",
)


async def test_lattice_variants_every_backend():
    with tempfile.TemporaryDirectory(prefix="jasper-lattice-shaders-") as out:
        args = ["shaders", "--out", out, "--backend", "all"]
        for spec in VARIANTS:
            lattice = spec + ("," if ":" in spec else ":") + "Ltc"
            args += ["--shader", spec, "--shader", lattice]
        result = subprocess.run(harness.editor_command() + args, capture_output=True,
                                text=True, timeout=TIMEOUT)
        if result.returncode:
            raise TestFailure(f"lattice vertex pipelines failed:\n{result.stdout[-4000:]}{result.stderr[-4000:]}")
        artifacts = list(Path(out).rglob("*.sdat"))
        # --backend all covers GL, GLES, D3D11, macOS/iOS Metal and Vulkan.
        expected = len(VARIANTS) * 2 * 6
        if len(artifacts) != expected:
            raise TestFailure(f"expected {expected} requested shader artifacts, got {len(artifacts)}")
