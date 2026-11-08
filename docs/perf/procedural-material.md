# Procedural Material performance

The editor budgets of [S90](../specs/S90-procedural-material.md) (R23 to R25, AC17), how they are
measured and what this host measured. A Procedural Material costs a game nothing: the evaluator, the
interpreter and the window are editor code, and a game ships only the baked PNGs and `.material`
(AC18, the player size check below).

```sh
UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=ProcMatBudgetsGpuSlow.*' tools/build.sh unit-tests
# the bake CLI end to end (tier 2: a one-shot headless editor; tier 3: no editor can start)
jasperengine-editor procedural-material bake <copy>/assets/brick_wall.procmat --size 1024 --overwrite
env -u DISPLAY PATH=<an empty dir> JASPER_CALL_SPAWN_TIMEOUT=0.001 \
    jasperengine-editor procedural-material bake <copy>/assets/brick_wall.procmat --size 1024 --overwrite
```

## What it measures

`ProcMatBudgetsGpuSlow` (`engine/tests/core/procmat/ProcMatBudgetsGpuSlowTest.cpp`) runs on the
30-node graph of `tools/fixtures/procmat-baked` (bricks, two Perlin noises, two FBMs, a Voronoi,
four colorizes, three blends, eleven math nodes, a blur, the Material Output: four passes, four
outputs). Each test prints a `[bench]` line; R25's wall-clock budgets are enforced only with
`JASPER_TEST_PERF_SLACK` (`perfSlack()`, docs/testing.md).

- **Slider drag (R23)**: 40 float edits (Bricks' `mortar`) of the window's material session at
  256² (`ProceduralSession` with the bake's roots, a 2 ms slice a frame as the window gives it). Per
  edit: the time from `setGraph` to the new image, the frames that took, and the session's own CPU
  work (`setGraph`, which plans the graph or writes a value edit into the held plan, plus each
  frame's `update`); per frame: `update` and the frame drawn after it.
- **Thumbnails (R24)**: `ProcMatThumbnails` asked for an image of every node (29 of 128²) with the
  window's 1 ms share of a frame: each image's cost (what the node shows) and each frame's `update`.
  On the device (the window's take: the image stays on it) and on tier 3 (a job per image).
- **Bake (R25)**: a 1024² plan of every output, on the device from `prepare` (compiles included)
  and again with the programs compiled, then through `evaluateOnCpu`, the CPU tier's bake entry.
- **The CLI**: wall time of `procedural-material bake --size 1024`, editor start and PNG writes
  included.

## Results

Host: this program's shared build host, AMD EPYC-Genoa (KVM), 8 cores, 15 GB; the tests in the
Linux build image (GCC 11.4, RelWithDebInfo core tests, Release editor) capped at 4 CPUs
(`docker --cpus 4`, `slot`), Mesa 23.2.1 llvmpipe (LLVM 15, `GL_RENDERER` "llvmpipe (LLVM 15.0.7,
256 bits)"). The process sees 8 hardware threads, so the CPU tier ran on 7 workers within the 4-CPU
quota. Host load average 9 to 15 during the runs (other builds); 2026-11-08, `56e317217` plus the
M6 change. Two runs of the suite; the second is quoted, the first within 12 % of it.

| Budget | Tier 1 (GPU) | Tier 2 (llvmpipe), measured | Tier 3 (CPU), measured |
|---|---|---|---|
| R23 slider drag at 256², 30 nodes: under 16 ms a frame | not measured: no GPU here; the default stays | edit to image 67 to 71 ms median, 85 ms p95, in 4 frames; a frame 12 ms median, 58 to 62 ms p95; `setGraph` (device-independent) 0.3 ms median, 0.4 ms p95 since value edits are not planned (6.4 to 6.9 ms median, 8.8 to 9.1 ms p95 before, same run) | not measured (a drag on tier 3 redraws through a job) |
| R24 thumbnails: one image a frame | not measured | at most 1 finished a frame (held); an image 112 ms median, 419 ms p90, 514 ms worst; `update` 9.9 ms median, 340 ms p99, 434 ms worst, against its 1 ms share | at most 1 a frame (held); an image 11 ms median, 32 ms p90, 37 ms worst; `update` under 0.01 ms median, 26 ms worst; 29 images in 4.9 s |
| R25 bake 1024²: tier 1 under 2 s, tier 2 under 5 s, tier 3 under 5 min | not measured: the 2 s default stays | 1.70 s (1.56 s compiled): **holds**; CLI end to end 9.6 s (20.9 s with a cold cache) | 111 s (99.9 s in the first run): **holds**; CLI end to end 109 s |

### What the numbers say

- **R23 does not hold as a frame budget on tier 2, and is not shown for tier 1.** A drag frame that
  draws and reads back costs up to about 60 ms on llvmpipe: drawing the passes, which a GPU does.
  The device-independent part was planning the graph again on every edit: 8.2 ms median and 16.2 ms
  p95 in the first measurement (load 9 to 15), once per session holding the graph, enough alone to
  reach 16 ms on a GPU. A value edit is now written into the held plan without planning
  (`planValueEdit`, R7): `setGraph` is 0.3 ms median and 0.4 ms p95. Measured back to back at load 2
  (the fix switched off, then on, twice): 6.4 and 6.9 ms median before, 0.3 ms after; the edit's
  time to its image on llvmpipe is the same within noise (66 to 72 ms), being the drawing.
  Thumbnails still plan an image when they start it (R24, below), not on each edit.
- **R24's "one image a frame" holds; one image is not one frame's work on tier 2.** The first draw
  of a node's image includes compiling its passes, which `update` does in the frame that starts it:
  110 to 500 ms on llvmpipe for this graph's nodes (each with what feeds it), so a thumbnail on tier
  2 is a visible hitch. This is why tier 2 draws only the selection and the Material Output (R24);
  M0's 35 ms median was for single nodes, not chains. Tier 3 keeps the frame free (the work is a job).
- **R25 holds on tiers 2 and 3** for this graph: 1.7 s and 111 s. It is 9 times M0's 0.19 s
  30-node chain on tier 2 (this graph has a blur and a Voronoi, and the host was loaded) and about
  three times M0's 30 to 42 s on tier 3. The CLI adds about 8 s of editor start on tier 2.
- **Tier 1** has no number: this host has no GPU. R23 and R25's tier 1 budgets stay defaults until
  the RTX lane runs `ProcMatBudgetsGpuSlow` on a GPU.

## Player size and frame cost (AC18; S91 AC16)

S34's check (`tools/build.sh players` with `PLAYER_FLAVOURS=release PLAYER_VARIANTS=full
PLAYER_BACKENDS=glcore`, then `tools/players/player_sizes.py --modules --baseline` and
`tools/players/lib_size.py --baseline` on the runtimes' `.debug` symbols) on the full Linux glcore
release player of the merge base with `origin/main` (`43a475707`, built from a detached worktree)
against this tree (`56e317217` plus the M6 change, which adds tests and a fixture only):

| | base `43a475707` | this tree | change |
|---|---|---|---|
| `jasperengine-player` | 42,961,488 B (40.97 MiB) | 43,064,976 B (41.07 MiB) | +103,488 B (+0.24 %) |
| S34 module "core (always)" | 20.49 MB | 20.57 MB | +0.08 MB |
| every other S34 module | | | unchanged (to 0.01 MB) |
| `engine: particles` (symbols) | 709,219 B | 778,840 B | +69,621 B (+9.8 %) |
| `engine: rendering` (symbols) | 3,793,708 B | 3,806,460 B | +12,752 B |
| all symbols | 36,501,786 B | 36,596,826 B | +95,040 B (+0.26 %) |

- The player holds no Procedural Material code: no symbol of `procmat`, the SPIR-V interpreter or
  the evaluators, and neither of the two compiler entry points S90 added under
  `JASPERENGINE_RUNTIME_SHADERS` (`compileSources`, `compileFragmentToSpirv`) nor `isCpuDevice` is
  linked (`nm -C` on the `.debug`). S90's share of the growth is 0 bytes.
- The growth is S91's particle runtime (+69.6 KB of symbols named for particles) and the rest of
  the range's runtime changes (BF23's fixes, S91's other render and Lua changes: rendering +12.8 KB,
  the C++ standard library +9.8 KB, scene and ECS +5.7 KB; inferred from the diff, not attributed
  per commit).
- **Frame cost.** Both players ran the same built game content (this tree's editor built it with each
  player; the copies relabelled to the editor's version, which a build requires) for 600 frames
  `--headless --after-ready --fixed-fps 60` under Xvfb on llvmpipe, the binaries alternated; wall
  time including start-up. The quietest set (load average 2 to 3, six runs of the particles fixture
  `tools/fixtures/particles-export` and three of `tools/fixtures/procmat-baked` per player):

  | Game | base, median (range) | this tree, median (range) | change |
  |---|---|---|---|
  | particles-export | 6.47 s (5.73 to 6.93) | 6.54 s (5.94 to 6.69) | +1.0 % |
  | procmat-baked | 28.96 s (28.40 to 29.63) | 29.27 s (28.34 to 30.36) | +1.1 % |

  Both changes are inside one binary's spread, so no frame cost is measurable; llvmpipe bounds the
  runs (S34's M9 says the same of its method). Two earlier sets at load 13 to 25 spread by a factor
  of two and are not quoted.
