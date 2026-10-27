#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""R24's web rows (docs/specs/S20-particles.md): the particle kernels' SIMD speedup on the web.

    python3 tools/perf/check_web_particles.py <simd128 transcript> <scalar transcript>

The transcripts are the web gate's (build/test-results/web/particles.log and particles-scalar.log)
of the particles test project run with ?particles_bench=1, a Release build with simd128 and one
without (WASM_SIMD=0), on the same machine. Its "[bench] particles web <metric> <value>" lines
give the reference stack's tick at 30 000 particles with and without Noise; the simd128 build
must be at least 2x faster on the tick and 2.5x on what Noise adds. Exit 1 when it is not, 2 when
a figure is missing.
"""

import re
import sys

LINE = re.compile(r"\[bench\] particles web (\w+) ([0-9.]+)")
TICK_SPEEDUP = 2.0
NOISE_SPEEDUP = 2.5


def figures(path):
    found = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.search(line)
            if m:
                found[m.group(1)] = float(m.group(2))
    return found


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    simd, scalar = figures(sys.argv[1]), figures(sys.argv[2])
    needed = ("ref_30k_sim_ms", "ref_30k_nonoise_sim_ms")
    for name, found in (("simd128", simd), ("scalar", scalar)):
        missing = [m for m in needed if m not in found]
        if missing:
            print(f"check_web_particles: the {name} transcript has no {', '.join(missing)}", file=sys.stderr)
            return 2
    tick = scalar["ref_30k_sim_ms"] / max(simd["ref_30k_sim_ms"], 1e-9)
    noise_simd = max(simd["ref_30k_sim_ms"] - simd["ref_30k_nonoise_sim_ms"], 1e-9)
    noise_scalar = max(scalar["ref_30k_sim_ms"] - scalar["ref_30k_nonoise_sim_ms"], 0.0)
    noise = noise_scalar / noise_simd
    ok = True
    for metric, value, budget in (("ref_30k_simd_speedup", tick, TICK_SPEEDUP), ("noise_simd_speedup", noise, NOISE_SPEEDUP)):
        passed = value >= budget
        ok = ok and passed
        print(f"{metric:28s} {value:8.2f} x  (>= {budget}) {'ok' if passed else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
