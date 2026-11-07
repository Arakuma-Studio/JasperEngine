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
  frames. Then, as information, where the serial time goes: the same run without the Look At,
  without the IK, without the two upper layers, without the crossfades and without root motion.
- **Micro** (R20, median of 15 runs): a two-pose blend of 128 bones with the native kernels
  (SSE2 / NEON / simd128) and the scalar ones; one sample of a 65-bone clip into a bound rig; a
  state-machine update of 20 states and 60 transitions (no pose); an Animator's runtime state
  (`AnimatorInstance::runtimeStateBytes`: the instance, parameters, layer / montage / IK runs and
  output buffers; the rig, poses and clip bindings scale with bones and clips and are part of
  the crowd's cost instead, spec deviation 8); one Look At solve over 3 bones of a chain leaning 30
  degrees, from its animated pose each call (S74 R33, information).

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

### S74's IK solvers: an A/B on this repository's host

The chain-walk Look At, the slope-corrected Foot IK and the `reached` measurement against the same
tree without them (`feat/s74-clip-quality`'s `engine/core`), in one build, on the S74 host (AMD
EPYC-Genoa, 8 vCPUs, shared, the linux image's RelWithDebInfo core-tests build, `--workers 3
--count-allocs`; three runs each, the median with the range). This host is about twice as slow as
E: the same five budgets miss with and without the change, so only the difference counts here.

| Metric | Before | After |
|---|---|---|
| Look At solve, 3 bones | 1.33 µs (1.31-1.34) | 2.86 µs (2.85-2.88) |
| Crowd200, wall time p50, 3 workers + main | 4.08 ms (3.96-4.12) | 4.13 ms (4.04-4.15) |
| Crowd200, wall time p95 | 4.48 ms (4.46-4.82) | 4.70 ms (4.51-4.75) |
| Crowd200, main thread p95 | 0.67 ms (0.65-0.71) | 0.63 ms (0.56-0.68) |
| Crowd200, serial p50 | 10.63 ms (10.48-10.70) | 11.28 ms (10.80-11.34) |
| Crowd200, serial p95 | 11.46 ms (11.09-11.56) | 11.87 ms (11.75-12.01) |
| Crowd200, heap allocations, 600 frames | 0 | 0 |
| Runtime state of an Animator | 5 080 B | 5 120 B |

The parallel times stay within their ranges; the serial ones do not: about 0.4 to 0.65 ms over 200
characters, 2 to 3 µs a character, of which the Look At's 1.5 µs. Without the IK the serial p50 is
the same (8.70 and 8.77 ms). ARM and web are unmeasured.

## GPU skinning on a GPU (GPU campaign)

RTX 3080 Ti, `tools/performance/vat_crowd.py --gpu-passes` (Game view 752x362): the skinned crowd's
GPU time is 28.8 / 61.2 / 101.8 ms on GL and 2.9 / 6.6 / 19.1 ms on Vulkan at 1,000 / 2,000 / 5,000
heroes (its frame is CPU-bound either way: 115-1,624 ms posing). GL uploads each hero's bones as a
texture every frame, Vulkan a storage buffer. No S21 GPU budget; recorded for a follow-up (the
crowd's bones in one texture or buffer per frame). The VAT crowd draws 5,000 heroes in 3-4 ms of
GPU on either backend (`tools/performance/vat-crowd.md`).
