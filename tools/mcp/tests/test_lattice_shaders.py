"""The lattice variants of the built-in mesh, depth and G-buffer shaders
(docs/superpowers/specs/2026-10-23-lattice-deformer-design.md §5): every combination of the
'Ltc' bit with the bits that change the vertex stage (skinning, morph targets, normals and
tangents, instancing and its fade, vertex colours, the sprite texture rect, UV sets, the
lighting outputs) compiles wherever the same variant without the lattice does, through
`jasperengine-editor shaders`. A variant without the lattice can fail by itself (more than
16 vertex attributes, e.g. eight morph targets with instancing): the lattice adds no
attribute to it, so it must fail the same way, never alone. The library surface shaders are
covered by the compile matrix (`shaders --library`, test_shader_library.py), which pairs every
mesh case with its lattice twin. Terrain variants never take the lattice."""

from __future__ import annotations

import itertools
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

import harness
from harness import TestFailure

TIMEOUT = 3600
CI_EXCLUDED = ("long-running (about 18 minutes: every lattice shader variant cross-compiled for every graphics API); "
               "in the full suite (MCP_SUITE=all) on the build servers")

# every backend token the CLI takes besides opengl
ALL_BACKENDS = "all"

FAILED = re.compile(r"\[ERROR\] Shader (\w+):([\w,]*) \((\w+)\) failed")


def fail(message: str) -> None:
    raise TestFailure(message)


def product(*axes):
    for parts in itertools.product(*axes):
        yield [token for part in parts for token in part]


MORPH = [[], ["Mta"], ["Mta", "Mnr"], ["Mta", "Mtg"], ["Mta", "Mnr", "Mtg"]]
VERTEX_COLOR = [[], ["Vc3"], ["Vc4"]]


def mesh_variants(full: bool):
    lighting = [["Ult"], ["Puc", "Shw"], ["Vtl"]] if full else [["Ult"], ["Puc", "Shw"]]
    normals = [[], ["Nor"], ["Nor", "Tan"], ["Nor", "Tan", "Nmp"]] if full else [[], ["Nor", "Tan"]]
    rect = [[], ["Txr"]] if full else [[]]
    uv = [[], ["Uv1"], ["Uv1", "Uv2"]] if full else [[], ["Uv1"]]
    return product(lighting, [[], ["Ski"]], MORPH, [[], ["Ist"], ["Ist", "Ifd"]], normals, VERTEX_COLOR, rect, uv)


def depth_variants(full: bool):
    texture = [[], ["Tex"], ["Tex", "Ams"]] if full else [[], ["Tex"]]
    uv = [[], ["Uv1"], ["Uv1", "Txr"]] if full else [[], ["Uv1"]]
    normals = [[], ["Nor"]] if full else [[]]
    return product(texture, [[], ["Ski"]], MORPH, [[], ["Ist"], ["Ist", "Ifd"]], VERTEX_COLOR, uv, normals)


def gbuffer_variants(full: bool):
    textures = [[], ["Bct"], ["Bct", "Mrt"]] if full else [[], ["Bct"]]
    return product(textures, [[], ["Nor"]], [[], ["Ski"]], MORPH, [[], ["Ist"]], VERTEX_COLOR)


def compile_specs(specs: list[str], backend: str) -> set[tuple[str, frozenset, str]]:
    """the (type, properties without 'Ltc', target) of each variant that failed"""
    out = Path(tempfile.mkdtemp(prefix="jasper-lattice-shaders-"))
    try:
        failed = set()
        # in chunks: a command line of thousands of specs is fine, but a chunk keeps the
        # output of one failure readable
        for start in range(0, len(specs), 400):
            args = ["shaders", "--out", str(out), "--backend", backend]
            for spec in specs[start:start + 400]:
                args += ["--shader", spec]
            r = subprocess.run(harness.editor_command() + args, capture_output=True, text=True, timeout=TIMEOUT)
            text = r.stdout + r.stderr
            for match in FAILED.finditer(text):
                tokens = frozenset(t for t in match.group(2).split(",") if t and t != "Ltc")
                failed.add((match.group(1), tokens, match.group(3), "Ltc" in match.group(2).split(",")))
            if r.returncode != 0 and not FAILED.search(text):
                fail(f"jasperengine-editor shaders failed without a variant error:\n{text[-3000:]}")
        return failed
    finally:
        shutil.rmtree(out, ignore_errors=True)


def check_lattice_twins(full: bool, backend: str) -> None:
    specs = []
    count = 0
    for kind, variants in (("mesh", mesh_variants(full)), ("depth", depth_variants(full)), ("gbuffer", gbuffer_variants(full))):
        for tokens in variants:
            specs.append(f"{kind}:{','.join(tokens)}" if tokens else kind)
            specs.append(f"{kind}:{','.join(tokens + ['Ltc'])}")
            count += 1
    failed = compile_specs(specs, backend)
    plain = {(kind, tokens, target) for kind, tokens, target, lattice in failed if not lattice}
    alone = sorted((kind, ",".join(sorted(tokens)), target) for kind, tokens, target, lattice in failed
                   if lattice and (kind, tokens, target) not in plain)
    if alone:
        fail(f"{len(alone)} lattice variants fail where the variant without the lattice compiles, e.g. {alone[:10]}")
    lattice_failed = sum(1 for f in failed if f[3])
    print(f"{count} variant pairs on {backend}: {lattice_failed} lattice variants fail, each as its twin without the lattice "
          f"({len(plain)} plain failures, over the vertex attribute limit)")


async def test_lattice_variants_every_combination_opengl():
    """every combination, on desktop GL (whose limits the other backends share)"""
    check_lattice_twins(True, "opengl")


async def test_lattice_variants_every_backend():
    """the main combinations on GLSL ES, HLSL, MSL and SPIR-V too"""
    check_lattice_twins(False, ALL_BACKENDS)
