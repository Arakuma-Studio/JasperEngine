# Animator performance

The budgets of [S21](../specs/S21-animation.md) (R19, R20, AC12), how they are measured and what
the benchmark server measured. The benchmark is `je-bench-anim` (built next to the core tests);
`tools/perf/check_bench.py` gates its JSON with `tools/perf/bench-thresholds.json`.

```sh
cmake --build build/linux/core-tests --target je-bench-anim
build/linux/core-tests/tests/core/je-bench-anim --workers 3 --count-allocs --json anim.json
python3 tools/perf/check_bench.py anim.json
tools/perf/bench-anim-wasm.sh                  # the same program as WebAssembly, with and without -msimd128
```

## What it measures

- **AnimBench.Crowd200** (R19): 200 skinned humanoids of 65 bones (Mixamo names: the hero's 43
  bones with four-joint fingers), all animating every frame. Per character: a 2D Freeform
  Cartesian blend tree of 5 one-second clips (every bone's rotation at 30 keys a second, the hips
  moving: root motion), a quarter of the crowd crossfading between two such states for the whole
  run (10 clips), an upper-body override layer with an Avatar Mask (arms, hands, head), an
  additive layer (the spine), Foot IK on both legs (a raycast per foot against a static Jolt box),
  a Look At over upper chest, neck and head, root motion applied to the Transform, a clip event
  per cycle, `writeBones: Exposed` (hips, head, hands) and the skin matrices of one skin over all
  65 bones. Reported: the `AnimatorSystem` update's wall time per frame (median and p95 of 600
  frames after 120 warm-up frames) with the workers and serially (`setParallel(false)`: the S06
  0-worker path), the main thread's part (prepare: loads, bindings, IK raycasts; finalize: root
  motion, events, callbacks) and, with `--count-allocs`, the heap allocations of 600 steady-state
  frames. Then, as information, where the serial time goes: the same run without the IK, without
  the two upper layers, without the crossfades and without root motion.
- **Micro** (R20, median of 15 runs): a two-pose blend of 128 bones with the native kernels
  (SSE2 / NEON / simd128) and the scalar ones; one sample of a 65-bone clip into a bound rig; a
  state-machine update of 20 states and 60 transitions (no pose); an Animator's runtime state
  (`AnimatorInstance::runtimeStateBytes`: the instance, parameters, layer / montage / IK runs and
  output buffers; the rig, poses and clip bindings scale with bones and clips and are part of
  the crowd's cost instead, spec deviation 8).

## Results

Benchmark server **E** (2026-10-27): AMD EPYC-Milan, 2 cores / 4 threads (KVM), Linux x64, the
core-tests build (RelWithDebInfo, GCC 11, SSE2 pose kernels), `--workers 3 --count-allocs`
(3 workers + the main thread); three runs, the median shown (the range in brackets).

| Metric | Measured | Budget |
|---|---|---|
| Crowd200, wall time p95, 3 workers + main | 2.37 ms (2.36-2.43) | ≤ 2.5 ms (R19) |
| Crowd200, wall time p50 | 2.29 ms (2.28-2.29) | |
| Crowd200, main thread p95 | 0.29 ms (0.29-0.30) | ≤ 0.4 ms (R19) |
| Crowd200, serial (0 workers) p95 | 5.61 ms (5.00-6.58) | ≤ 8.8 ms (R19) |
| Crowd200, heap allocations, 600 frames | 0 | 0 (R19) |
| Two-pose blend, 128 bones (SSE2) | 0.73 µs | ≤ 1.1 µs (R20) |
| the same, scalar | 28.9 µs | |
| SIMD against scalar | 39.7 x | ≥ 2 x (R20) |
| One 65-bone clip sample | 1.85 µs | ≤ 2.2 µs (R20) |
| State-machine update, 20 states / 60 transitions | 0.04 µs | ≤ 1.1 µs (R20) |
| Runtime state of an Animator | 4 640 B | ≤ 8 KB (R20) |

Re-measured after the S29 merge (2026-10-29, five runs on an idle E): Crowd200 p95 2.39 ms
(2.38-2.50), p50 2.29 ms, main thread p95 0.30 ms; the micro numbers unchanged. The p95 has
about 0.1 ms of headroom: an earlier batch while E was busy with another job measured 3.0 and
3.7 ms, so gate it on an idle server. After the S18 / BF8 merge (2026-10-30, five runs): p95 2.37 ms
(2.33-2.45), serial p95 5.95 ms, 0 allocations; `check_bench.py`: all budgets hold.

After the retarget absorption (2026-11-01, `absorb/retarget`: one retarget path for the Animator
and the Animation Player, humanoid clips, state Mirror / Foot IK; E idle, five runs, medians with
the range): Crowd200 p95 2.23 ms (2.18-2.30), p50 2.08 ms, main thread p95 0.28 ms (0.28-0.29),
serial p95 5.43 ms (4.69-5.65), 0 allocations; one 65-bone clip sample 1.75 µs, the 128-bone
blend 0.67 µs (40 x scalar), state-machine update 0.04 µs, runtime state 5 000 B;
`check_bench.py`: all budgets hold in every run. The crowd does not retarget: the absorption's
only cost on its path is a branch per clip sample (a retargeted clip slot), which the numbers do
not show.

Where the serial time goes (p50, 4.90 ms in all): the IK about 1.1 ms (the 400 raycasts run on the
main thread in prepare), the upper and additive layers 0.2 ms, the crossfading quarter 0.4 ms,
root motion 0.35 ms; the base tree of 5 clips with the rig space, the skins and the writes
2.8 ms (14 µs a character).

The parallel speedup (about 2.1 x serial) is the machine's: E's 4 threads are 2 cores with two
hyper-threads each, and the pose kernels keep a core's SIMD units busy, so the second thread of a
core adds little. The wave itself has nothing serial: it is split in chunks of 4 Animators, each
independent (no locks, no shared writes), so more physical cores shorten it further.

Two changes made the budget: the IK refreshes only the moved bone's subtree of the rig-space
cache (a rig is in parents-first depth-first order, so a subtree is a contiguous range), and the
root-motion lap of a looping clip (its root at 0 and at its length) is sampled once at bind
instead of every frame. A handled clip event builds no string (the "no handler" warning's key is
hashed in place): before, each event cost two allocations.

## S74: clip rows, the hot paths, WebAssembly

The rows of [S74](../specs/S74-clip-quality.md) (R10) are information in `check_bench.py`, except the
owner's two budgets of R44 (`web30_serial_p95_ms`, `web30_clip_heap_bytes`). `--clip <file.anim>` adds
`real_<name>_*` rows for clips kept out of the repository; `clip_dense` and `clip_reduced` are
generated shapes (201 tracks, 8,241 keys; 132 tracks, 911 keys).

**The machine of everything in this section:** an AMD EPYC-Genoa VM, 8 vCPUs, 15 GB, Linux x64, shared
with other agents' jobs (one heavy job at a time, so mostly idle while these ran), through the build
images. **It is not server E:** it measures about 2 x slower on the crowd, so these numbers compare with
each other, never with the budgets of the sections above (Crowd200 p95 is over 2.5 ms here both before and
after). **ARM is unmeasured (the owner's decision): no number here says anything about a phone.**

### Native, before and after the hot paths

`je-bench-anim --workers 3 --count-allocs`, core-tests build (RelWithDebInfo, GCC 11, SSE2), five runs,
medians with the range. Before: `23f235324` (the rows, the engine unchanged). After: `b8c32e34f`.

| Metric | Before | After | Change |
|---|---|---|---|
| One 65-bone clip sample (R43: at least 1.5 x) | 4.07 µs (3.96-4.22) | 2.28 µs (2.21-2.31) | **1.79 x** |
| The same on 8 key groups | 4.17 µs | 2.21 µs | 1.89 x |
| `sampleQuaternion`, keys 4 degrees apart | 54.3 ns | 24.3 ns | -55 % |
| `sampleQuaternion`, keys 20 degrees apart | 54.5 ns | 33.3 ns | -39 % |
| `sampleQuaternion`, keys 60 degrees apart (exact slerp) | 55.2 ns | 67.4 ns | **+22 %** |
| Crowd200, 3 workers + main, p95 | 4.92 ms (4.57-5.15) | 4.47 ms (4.17-4.62) | -9 % |
| Crowd200, serial, p50 | 11.63 ms (11.31-11.81) | 9.98 ms (9.62-11.70) | -14 % |
| Crowd200, serial, p95 | 12.95 ms (12.74-13.72) | 12.30 ms (11.85-14.69) | -5 % (ranges overlap) |
| Crowd200, heap allocations, 600 frames | 0 | 0 | |
| Runtime state of an Animator | 5,000 B | 5,144 B | +144 B (3 layers: the skipped state of R14) |
| Web30, one thread, p95 (native) | 1.99 ms (1.64-2.27) | 1.52 ms (1.29-1.89) | -24 % |
| Animation Player crowd of 200, p50 | 2.66 ms (2.48-2.89) | 2.09 ms (2.00-2.20) | -21 % |
| The same, allocations of 200 frames | 44,836 | 4,836 | -89 % |
| 200 idle Animation Players, allocations of 200 frames | 40,000 | 0 | |
| Retargeted crowd of 50, serial, p50 | 0.560 ms | 0.568 ms | none |

**Full clips gain far less than the 65-bone micro.** That micro's keys are 30 a second with small swings,
so every segment takes the fastest tier and rotations are all it samples. A real clip also samples
translations and scales, searches keys, and has wider segments:

| Clip (sample, then sample + hierarchy) | Keys | Sample | Sample + hierarchy |
|---|---|---|---|
| `real_walk_1mm` (reduced) | 911 | 5.87 -> 4.92 µs, -16 % | 8.97 -> 8.01 µs, -11 % |
| `real_atlas_1mm` (reduced) | 7,407 | 7.21 -> 6.09 µs, -16 % | 10.39 -> 9.19 µs, -11 % |
| `real_pab_atlas` (dense) | 90,651 | 8.37 -> 7.69 µs, -8 % | 11.63 -> 11.02 µs, -5 % |
| `real_q_Rig_Walk_Loop` (dense) | 6,519 | 6.60 -> 6.13 µs, -7 % | 8.99 -> 8.77 µs, -3 % |
| `real_pab_walk` (dense) | 8,241 | 7.94 -> 7.84 µs, -1 % | 11.15 -> 10.71 µs, -4 % |
| `clip_dense` (generated) | 8,241 | 8.91 -> 7.82 µs, -12 % | 11.71 -> 10.97 µs, -6 % |
| `clip_reduced` (generated, wide segments) | 911 | 7.41 -> 7.45 µs, none | 9.37 -> 9.44 µs, none |

Single-run noise on these rows is several percent. A clip whose segments are mostly over 30 degrees pays
the extra dot product (the 60 degree row above) and gains nothing; a clip of nothing else would get slower.
The real clips are ozz samples, a Quaternius clip and an earlier Python reduction of them at 1 mm (not `ClipReduction`), kept out of the repository.
Parse + build of a `.anim` costs about 50 ms per MB here (`real_pab_atlas`: 4.3 MB, 228 ms), unchanged by
S74; with async loading it now happens on a worker where there are threads.

### WebAssembly

`tools/perf/bench-anim-wasm.sh`: the same program, Emscripten Release in the web image, under the image's
node 24 (V8, not a browser), one thread (`--workers 0`, the single-thread web path), on the machine named above;
three runs, medians with the range, at `6a2d1c51a`. The kernels column is `JASPERENGINE_WASM_SIMD`:
`-msimd128` applies to the pose kernels only (`AnimatorPose.cpp`), as in a shipped web build, so the sampler is
scalar wasm in both.

| Metric | With `-msimd128` | Without | Budget |
|---|---|---|---|
| **Web30, one thread, p95** | **1.65 ms (1.54-1.81)** | **2.19 ms (2.15-2.27)** | under 4 ms (R44): holds |
| Web30, one thread, p50 | 1.16 ms (1.15-1.21) | 1.45 ms (1.40-1.46) | |
| **Web30's clips, heap** | **446 KB** | **446 KB** | under 16 MB (R44): holds |
| Crowd200, one thread, p50 | 9.70 ms (8.93-9.85) | 10.89 ms (10.57-11.02) | |
| Two-pose blend, 128 bones | 1.49 µs | 3.41 µs | |
| One 65-bone clip sample | 2.10 µs | 2.14 µs | |
| Rig space, 65 bones | 2.30 µs | 2.33 µs | |
| Skin matrices, 65 bones | 5.09 µs | 4.85 µs | |
| `real_pab_walk` (dense), sample + hierarchy | 8.17 µs | 8.72 µs | |
| `real_walk_1mm` (reduced), sample + hierarchy | 5.94 µs | 6.53 µs | |
| `real_pab_atlas` (4.3 MB), parse + build | 655 ms | 686 ms | |
| `real_pab_atlas`, heap | 1.99 MB | 1.99 MB | |
| Animation Player crowd of 200, p50 | 2.46 ms | 2.35 ms | |

Against `Quaternion::slerp` in the same process, with `-msimd128`: `sampleQuaternion` is 34 against 56 ns with
keys 4 degrees apart, 46 against 54 ns at 20 degrees, and **80 against 54 ns at 60 degrees**, where it calls slerp.
There is no run of the old sampler on wasm, so R43's 1.79 x is a native number; the wasm gain is what these rows show.

What these numbers do not say:

- **The budgets hold on this x64 VM under node. No phone or browser was measured.** The owner's budgets are for
  the web; a low-end device can be several times slower, and ARM is unmeasured.
- The 446 KB is the bench character's seven clips, not a game: a project's own figure is its clip memory
  (R7's monitor). One dense 90,000-key clip alone is 2 MB.
- These are not comparable with the native table, which is a RelWithDebInfo build with its allocator and
  libm: wasm beating it on a row does not mean wasm is faster than a native Release build.
- A `.anim` parses about 2.8 x slower here than natively, about 150 ms per MB. Without threads that happens in
  the call (R16).

## GPU skinning on a GPU (GPU campaign)

RTX 3080 Ti, `tools/performance/vat_crowd.py --gpu-passes` (Game view 752x362): the skinned crowd's
GPU time is 28.8 / 61.2 / 101.8 ms on GL and 2.9 / 6.6 / 19.1 ms on Vulkan at 1,000 / 2,000 / 5,000
heroes (its frame is CPU-bound either way: 115-1,624 ms posing). GL uploads each hero's bones as a
texture every frame, Vulkan a storage buffer. No S21 GPU budget; recorded for a follow-up (the
crowd's bones in one texture or buffer per frame). The VAT crowd draws 5,000 heroes in 3-4 ms of
GPU on either backend (`tools/performance/vat-crowd.md`).
