# Particle system performance

The budgets of [S20](../specs/S20-particles.md) (R20-R26, AC10-AC12), how they are measured and
what the benchmark server measured. The benchmark is `je-bench-particles` (built next to the core
tests); `tools/perf/check_bench.py` gates its JSON with `tools/perf/bench-thresholds.json`.

```sh
cmake --build build/linux/core-tests --target je-bench-particles
build/linux/core-tests/tests/core/je-bench-particles --count-allocs --json particles.json [--workers N]
python3 tools/perf/check_bench.py particles.json
```

## What it measures

- **Scenarios** (AC11): `ref_100k` (the spec's reference stack at 100 000 live particles: Rate +
  Cone, start ranges, gravity, Limit Velocity drag, Noise High one octave, Color and Size over
  Lifetime curves, an 8 x 8 sheet with frame blending, additive billboards), `ref_100k_nonoise`,
  `ref_100k_sorted` (the same alpha-blended and sorted by distance), `ref_10k`, `trails_10k`
  (per-particle trails, 16 points), `collision_world_10k` (World collision at Medium quality
  against a floor answered by a ray provider) and `many_small_1000x100` (1 000 emitters of 100).
- **Simulation**: the wall time of one tick of every live particle through `ParticleWorld`
  (spawn, the update kernels, collisions, deaths and compaction, trails), median of 40 ticks
  at the steady state; with all workers, on one thread, and on one thread with the scalar
  kernels forced (`setForceScalarKernels`).
- **Render prep**: cull, interpolation, the appearance curves, sort keys, thinning, packing
  into the 32-byte records (and trail strips), for one view.
- **Memory**: channel bytes per particle and trail bytes per point.
- **Frame costs without work**: the particle scheduler of a scene without systems, and what
  each out-of-sight Automatic looping emitter adds (100 of them).
- **Allocations** (`--count-allocs`): heap allocations of 600 steady-state frames of
  `ref_100k` + `trails_10k` + an emitter with a death sub-emitter, simulation and render prep.

### Machines with fewer cores than the reference

R21 and R22 give their desktop budgets for the CI reference machine, 8 workers. A machine with
fewer cores runs the bench with its own worker count (one per physical core but the main
thread's: `--workers 1` on a 2-core server) and gates:

- the single-thread figures against R21's single-thread budgets,
- the parallel efficiency `speedup / (workers + 1)` against R24's floor of 0.6, as measured,
- the 8-worker budgets against `single thread / (9 x 0.6)`: the time 9 threads would take at
  the least parallel efficiency R24 allows (`*_8w_projected_ms`).

The measured all-worker times stay in the JSON as information.

## Results

Benchmark server **E** (the replacement of 2026-10-27): AMD EPYC-Milan, 2 cores / 4 threads
(KVM), Linux x64, the core-tests build (RelWithDebInfo, GCC, SSE2 baseline; the curl noise takes
its AVX2 path), `--workers 1 --count-allocs`, after the oe-port merges of 2026-10-27; three runs,
the median shown (the range in brackets).

| Metric | Measured | Budget |
|---|---|---|
| Reference stack, 100 k, one tick, 1 thread | 3.86 ms (3.81-3.88) | ≤ 4.0 ms (R21) |
| the same, 8 workers (projected) | 0.71 ms | ≤ 1.0 ms (R21) |
| Without Noise, 1 thread | 0.88 ms (0.86-0.89) | ≤ 1.5 ms (R21) |
| Without Noise, 8 workers (projected) | 0.16 ms | ≤ 0.5 ms (R21) |
| Parallel efficiency at 100 k (1 worker + main) | 0.81 | ≥ 0.6 (R24) |
| SIMD against scalar, the reference tick | 64 x | ≥ 2 x (R24) |
| SIMD against scalar, the Noise kernel | 78 x | ≥ 2.5 x (R24) |
| Render prep, 100 k, 1 thread | 2.79 ms | |
| Render prep, 8 workers (projected) | 0.52 ms (0.51-0.53) | ≤ 0.6 ms (R22) |
| Back-to-front sort, 100 k, 1 thread (extra) | 0.45 ms (0.43-0.46) | ≤ 0.5 ms (R22) |
| Channel storage per particle | 56 B | ≤ 64 B (R20) |
| Trail storage per point | 17.3 B | ≤ 24 B (R20) |
| World collision, Medium, 10 k: ray casts per tick | 256 | ≤ 256 (AC5) |
| Off-screen Automatic emitter | 0.027 µs | ≤ 2 µs (AC10) |
| Scheduler, scene without systems | 0.03 µs | ≤ 5 µs (R26) |
| Heap allocations, 600 steady-state frames | 0 | 0 (R25) |

The first runs on this machine missed two budgets (the reference tick at 4.25 ms, the sort's
extra at 0.50-0.54 ms; the same build ran 3.91 ms and 0.43 ms on the previous E): integrate now
works out a constant drag's decay and the limit's share once per call and skips the limit where
no lane exceeds it, the Noise's first octave is written in place, and the sort of a fresh pack
reads its keys in place and fetches the gather's records ahead. All of them keep the same bits.

Information (not budgeted): `ref_10k` 0.25 ms per tick and 0.16 ms prep (2 threads),
`trails_10k` 0.48 ms per tick and 2.12 ms for its 10 041 strips of 17 points,
`collision_world_10k` 0.27 ms per tick, `many_small_1000x100` 1.56 ms per tick and 2.89 ms prep
(the per-emitter overhead dominates: 1.6 µs simulation, 2.9 µs prep per emitter).

The scalar kernels are the correctness reference (and the build without SIMD on the web): they
emulate four lanes element by element, which is why they are so far behind.

**After the upstream absorption** (S20 section 11: arc and radius modes, Align to Direction, offset
curves, Limit Velocity's separate axes, Current inherit, 3D Rotation by Speed, the noise scroll,
burst count curves, mesh alignment and the rest), on E on 2026-11-01, the same build and flags,
three runs (median, range): the reference stack 3.50 ms on 1 thread (3.49-3.52, budget 4.0),
0.65 ms projected for 8 workers (budget 1.0), without Noise 0.81 ms (0.81-0.81, budget 1.5) and
0.15 ms projected (budget 0.5), parallel efficiency 0.82, SIMD 64 x and the Noise kernel 78 x,
render prep 1.35 ms (sorted, 1 thread: 2.96 ms), `trails_10k` 0.47 ms, `collision_world_10k`
0.23 ms, `many_small_1000x100` 1.33 ms, no allocation in the steady state: every budget holds
(`check_bench.py`), the new fields cost nothing when they are off. Rerun after the total-velocity
stretch channels (ChAnimVel*) went in: 3.49 ms on 1 thread, 0.65 ms projected, without Noise
0.81 ms, SIMD 66 x, Noise 81 x, prep 1.41 ms, `many_small_1000x100` 1.44 ms, no allocation: every
budget holds.

**BF23 F1 and F2** (a sub-emitter's children drawn and counted), 2026-11-04. A system without
sub-emitters takes the same paths and allocates nothing new. Measured on the shared 8-core build host
under other builds' load (load average 9-14), the core-tests build, `--workers 3 --count-allocs`, three
runs each (median, range), so these compare with each other and not with server E's table above:
`check_bench.py` on one run of each misses the same 5 budgets here before and after, and only E can say
an absolute budget holds. The sort's code did not change; its range is this host's noise.

| Metric | Before | After |
|---|---|---|
| Reference stack, 100 k, one tick, 1 thread | 8.25 ms (8.22-8.50) | 8.24 ms (8.04-8.32) |
| the same, 8 workers (projected) | 1.53 ms (1.52-1.57) | 1.53 ms (1.49-1.54) |
| Without Noise, 1 thread | 2.51 ms (2.42-2.56) | 2.46 ms (2.39-3.05) |
| Render prep, 100 k, 1 thread | 6.60 ms (6.59-6.81) | 6.63 ms (6.47-6.83) |
| Render prep, 8 workers (projected) | 1.22 ms (1.22-1.26) | 1.23 ms (1.20-1.26) |
| Back-to-front sort, 100 k, 1 thread (extra) | 1.22 ms (1.03-1.49) | 1.61 ms (1.19-2.91) |
| `trails_10k`, one tick | 1.10 ms (0.98-1.11) | 1.08 ms (1.02-1.11) |
| `trails_10k`, prep | 4.60 ms (4.52-4.77) | 4.61 ms (4.43-4.75) |

Channel storage (56 B), trail storage (17.3 B) and 100143 live are the same. Heap allocations in the
steady state: 0, 0, 1 before, 0, 0, 0 after. The draw path of children is not in this bench (it packs
directly); `ParticlesGpu.SubEmittersDrawWithTheirOwnRenderers` checks a system without sub-emitters makes no
child render state.

### Web

The web rows of R21, R22 and R24, measured by the particles test project
(`tools/test-projects/particles`, `?particles_bench=1`) in headless Chrome on the same server E:
the reference stack at 30 000 live particles on the page's one thread, a Release build with
simd128 and one without (`WASM_SIMD=0`). The simd128 build gates R21/R22 (an error line when a
budget is missed); `tools/perf/check_web_particles.py` compares the two builds' ticks for R24
(within one build the scalar kernels are auto-vectorized for simd128, so a browser without
simd128, which runs the scalar build, is the honest reference).

```sh
BUILD_TYPE=Release WEB_PROJECT=particles tools/build.sh web
BUILD_TYPE=Release WASM_SIMD=0 WEB_PROJECT=particles tools/build.sh web
GATE=particles GATE_ARGS='--query=particles_bench=1 --expect=particles_web_budgets=pass' tools/build.sh web-gate
WASM_SIMD=0 GATE=particles GATE_ARGS='--query=particles_bench=1 --expect=particles_web_budgets=scalar' tools/build.sh web-gate
python3 tools/perf/check_web_particles.py build/test-results/web/particles.log build/test-results/web/particles-scalar.log
```

| Metric | simd128 | scalar build | Budget |
|---|---|---|---|
| Reference stack, 30 k, one tick | 2.26 ms | 14.11 ms | ≤ 3.0 ms (R21) |
| Without Noise | 0.43 ms | 1.02 ms | ≤ 1.5 ms (R21) |
| Render prep, 30 k | 0.87 ms | 1.56 ms | ≤ 1.2 ms (R22) |
| SIMD against scalar, the reference tick | 6.2 x | | ≥ 2 x (R24) |
| SIMD against scalar, the Noise kernel | 7.2 x | | ≥ 2.5 x (R24) |

(The replacement E, after the 2026-10-27 merges, before the integrate changes above.)

### Not measured here

- **GPU (R23), desktop:** measured in the GPU campaign (docs/perf/gpu-campaign.md): RTX 3080 Ti,
  GL, 1080p, the reference stack at 92,344 live particles (3-10 px each): 0.245 ms of GPU in one
  draw (≤ 1.5 ms for 100 k), `tools/perf/gpu_campaign.py particles`.
- **Mobile (R21-R23)** and **GPU (R23) on mobile** rows: manual sign-off on the named reference devices
  with the `particles_perf` sample scene (Android GPU timings through the S15 pass timers). The
  CPU paths are the NEON instantiation of the same kernels; the Android emulator run
  (`ANDROID_PROJECT=particles`, `tools/test-projects/particles/android-expect.txt`) checks they
  run, not their speed.

## Comparison

- **OpenEngine** (`Engine/Modules/Particles`, the design S20 follows): its processors run one
  emitter at a time on one thread with scalar loops over the channels; its curl noise is the
  scalar `GradientCurl3D` our golden test matches. The same noise field here runs 4 or 8
  samples at a time with the lattice work shared by the three potentials, and the emitters'
  chunks run on the job system. OpenEngine was not built or run for this comparison (its code
  is reference only), so there are no side-by-side timings.
- **Unity** (Shuriken): the module stack and names are Unity's, and Unity also simulates on
  its job system. Unity is not available on the build and benchmark servers, so there are no
  Unity timings; the comparison stays at the design level.

## Where the time goes

- The Noise module is two thirds of the reference tick: 24 lattice corners per particle (three
  potentials of eight), each a hash, a gradient pick and its weights. AVX2 halves it; the
  lattice cell setup is shared by the potentials.
- Render prep is bound by the appearance curves (LUT gathers per lane), the colour encoding and
  the 32-byte stores; the sort adds the key computation, a two-pass 8-bit radix sort and a
  gather of the records.
- Trails: their strips are 2 x points vertices of 28 bytes; 10 k trails of 17 points are 9.5 MB
  a frame to upload, which is what the GPU side pays.

**S91 runtime features** (Ease mode, distortion, Ring, triggers, exposed values written without
compiling, Follow Parent, sound events), 2026-11-07, on the shared 8-core build host under other
builds' load, the core-tests build, `--workers 3 --count-allocs`, three runs each (median, range):
before on 2c5594c49 (with its test file's missing brace closed), after on 31414622c. A system without
these features compiles and runs the same ops; the new channels exist only for Follow Parent
children and the exposed slots only for effects with exposed parameters.

| Metric | Before | After |
|---|---|---|
| Reference stack, 100 k, one tick, 1 thread | 8.83 ms (8.75-10.80) | 8.46 ms (8.38-8.61) |
| the same, 8 workers (projected) | 1.64 ms (1.62-2.00) | 1.57 ms (1.55-1.60) |
| Without Noise, 1 thread | 2.61 ms (2.55-2.66) | 2.44 ms (2.39-2.60) |
| Render prep, 100 k, 1 thread | 6.65 ms (6.53-7.08) | 6.72 ms (6.56-9.51) |
| Render prep, 8 workers (projected) | 1.23 ms (1.21-1.31) | 1.25 ms (1.21-1.76) |
| `trails_10k`, one tick | 1.10 ms (1.09-1.16) | 1.10 ms (1.08-1.13) |
| `trails_10k`, prep | 4.54 ms (4.40-4.65) | 4.64 ms (4.45-4.80) |

Channel storage (56 B) and 100143 live are the same; steady-state allocations 0, 1, 0 before and
0, 0, 0 after. The differences are within this host's run-to-run range; only server E can say an
absolute budget holds.
