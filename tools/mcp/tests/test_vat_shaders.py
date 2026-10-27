"""Compile the Vertex Animation vertex pipelines on every supported backend.

The variants of vertex animation design §3.5: 'Vat' (Vertices mode, a_vatIndex) and 'Vab'
(Bones mode, JOINTS_0/WEIGHTS_0 without skinning) in the colour, depth and G-buffer passes,
alone, instanced (i_vat0/i_vat1), lit with shadows and vertex lights, with a lattice after
them, and with the skinning and morph bits a key may still carry (the compiler drops them).
Every layout stays below the vertex-attribute limit, so any compiler error fails.
"""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

import harness
from harness import TestFailure

TIMEOUT = 900

VARIANTS = (
    "mesh:Ult",
    "mesh:Puc,Shw,Nor,Tan,Nmp,Uv1",
    "mesh:Puc,Shw,Ist,Ifd,Nor,Uv1,Vc4",
    "mesh:Vtl,Nor,Uv1,Uv2,Ltc,Vc4",
    "mesh:Ult,Ski,Mta,Mnr,Nor,Uv1",
    "depth",
    "depth:Tex,Ams,Uv1,Ist,Ifd",
    "depth:Ltc,Vc4",
    "gbuffer:Bct,Nor",
    "gbuffer:Bct,Nor,Ist,Ltc,Vc3",
)


def _with(spec: str, bit: str) -> str:
    return spec + ("," if ":" in spec else ":") + bit


async def test_vat_variants_every_backend():
    with tempfile.TemporaryDirectory(prefix="jasper-vat-shaders-") as out:
        args = ["shaders", "--out", out, "--backend", "all"]
        for spec in VARIANTS:
            args += ["--shader", _with(spec, "Vat"), "--shader", _with(spec, "Vab")]
        result = subprocess.run(harness.editor_command() + args, capture_output=True,
                                text=True, timeout=TIMEOUT)
        if result.returncode:
            raise TestFailure(f"vertex animation pipelines failed:\n{result.stdout[-4000:]}{result.stderr[-4000:]}")
        artifacts = list(Path(out).rglob("*.sdat"))
        # --backend all covers GL, GLES, D3D11, macOS/iOS Metal and Vulkan.
        expected = len(VARIANTS) * 2 * 6
        if len(artifacts) != expected:
            raise TestFailure(f"expected {expected} requested shader artifacts, got {len(artifacts)}")
