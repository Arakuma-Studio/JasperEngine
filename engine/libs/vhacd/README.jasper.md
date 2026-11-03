# V-HACD in JasperEngine

- Upstream: https://github.com/kmammou/v-hacd, `master` at `f900e4236149` (2025-09-18): release
  `v4.1.0` plus the later header fixes (the last change to `include/VHACD.h` is `b1658a70d8f4`,
  2025-04-02). BSD-3-Clause, `LICENSE`.
- Used by: convex decomposition of Physics Placement
  (`engine/core/physics/ConvexDecomposition.cpp`, design spec section 5.3).

## Vendored
`include/VHACD.h` as `VHACD.h` and `LICENSE`, from that commit; the single header holds the whole
library. `VHACD.h` carries the patch below (`jasper.patch`); `LICENSE` is byte for byte.

## Ours
- `jasper.patch` (each hunk marked "JasperEngine patch" in the header), for the multithreaded
  decomposition (`m_asyncACD`):
  - `VHACD_THREAD_COUNT` sets the size of the job pool (upstream: always 8; still the default).
  - `VoxelHull::m_voxelHullCount` is atomic: hulls are constructed on several threads (and by
    concurrent decompositions). It only names debug dumps, so results do not change.
  - The cost-matrix jobs are waited for even when the decomposition is cancelled. Upstream skips
    the wait on cancel and returns while queued jobs still read the task list and the hulls it
    frees (a use after free whenever a cancel lands in that stage).
  - A cancel during the last stage (merged hulls reduced and handed to the output) frees the hulls
    it did not hand over. Upstream's loop stops on the cancel and then clears the map that owns
    the rest, which leaked every merged hull of a decomposition cancelled at that point (~1 MB in
    the editor tests under LeakSanitizer). Each hull now leaves the map as it moves to the output,
    and what the map still holds after the loop is released
    (`ConvexDecomposition.CancelWhileFinalizingFreesTheHulls`).
- `ConvexDecomposition.cpp` is the only translation unit that includes the header, with
  `ENABLE_VHACD_IMPLEMENTATION` and its warnings silenced around the include. It runs the
  synchronous `CreateVHACD()`/`Compute()` on the cache's worker with `m_asyncACD` on and a pool of
  one thread less than the machine has (at least 1, at most 8), so the editor's main thread keeps
  a core.
- Compiled only when `JASPERENGINE_CONVEX_DECOMPOSITION` is on: by default the editor on Windows,
  Linux and macOS. Exported games (the engine as the top-level build) and Web, Android and iOS
  builds leave V-HACD out; their cache still reads and writes hulls but cannot decompose.

## Measured (2026-10-24)
Linux, 8 hardware threads (pool of 7), GCC 13 `-O3`, the settings of `ConvexDecomposition.cpp`
(flood fill, shrink wrap). Mean of 5 runs each. Torus: major radius 1, minor 0.3, 32 x 12
segments (768 triangles); cube: 12 triangles. Quality presets as in
`DecompositionParams::fromQuality` (LOW 4 hulls / 100 000 voxels / 32 vertices, MEDIUM 8 / 400 000
/ 64, HIGH 16 / 1 000 000 / 64).

| Mesh | Quality | Hulls | Single-threaded | Job pool, 7 threads | Job pool, 3 threads |
|---|---|---|---|---|---|
| torus | LOW | 4 | 2094 ms | 1001 ms | 1189 ms |
| torus | MEDIUM | 8 | 5288 ms | 2130 ms | 2758 ms |
| torus | HIGH | 16 | 9670 ms | 3357 ms | 4637 ms |
| cube | LOW | 1 | 198 ms | 194 ms | |
| cube | MEDIUM | 1 | 525 ms | 532 ms | |
| cube | HIGH | 1 | 1078 ms | 1071 ms | |
| torus 256 x 128 (65 536 triangles) | HIGH | 16 | 11594 ms (1 run) | 3659 to 3958 ms | |

- Determinism: every mesh and quality gave bit-identical hull points (float32 bits) in all 5 runs
  with the pool, and the same bits as the single-threaded run, with 3 and with 7 threads. The jobs
  write only their own hull or cost entry; the results are gathered in submission order.
- A convex mesh does not speed up: it stops after the first hull, whose voxelization and raycast
  tree are serial.
- Cancel (`ConvexDecompositionCache::cancel`) of the dense torus at HIGH, at 11 points from 0 to
  3.5 s into the job: done within 0 to 584 ms (single-threaded it took up to about 1 s).

## Updating
Copy `include/VHACD.h` and `LICENSE` from the new commit, reapply `jasper.patch`
(`patch -p1 -d engine/libs/vhacd < engine/libs/vhacd/jasper.patch`, or by hand where upstream
moved), bump `ALGORITHM_VERSION` in `ConvexDecomposition.cpp` (cached hulls of the old version are
then never served), update the commit above and in `NOTICE.md`, re-measure, and run
`jasperengine-editor-tests --gtest_filter='ConvexDecomposition*'` (it checks determinism too).
