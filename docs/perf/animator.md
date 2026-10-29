# Animator performance

The budgets of [S21](../specs/S21-animation.md) (R19, R20, AC12), how they are measured and what
the benchmark server measured. The benchmark is `je-bench-anim` (built next to the core tests);
`tools/perf/check_bench.py` gates its JSON with `tools/perf/bench-thresholds.json`.

```sh
cmake --build build/linux/core-tests --target je-bench-anim
build/linux/core-tests/tests/core/je-bench-anim --workers 3 --count-allocs --json anim.json
python3 tools/perf/check_bench.py anim.json
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
