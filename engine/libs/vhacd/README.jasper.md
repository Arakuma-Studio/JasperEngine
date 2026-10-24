# V-HACD in JasperEngine

- Upstream: https://github.com/kmammou/v-hacd, `master` at `f900e4236149` (2025-09-18): release
  `v4.1.0` plus the later header fixes (the last change to `include/VHACD.h` is `b1658a70d8f4`,
  2025-04-02). BSD-3-Clause, `LICENSE`.
- Used by: convex decomposition of Physics Placement
  (`engine/core/physics/ConvexDecomposition.cpp`, design spec section 5.3).

## Vendored (byte for byte from that commit)
`include/VHACD.h` as `VHACD.h`, and `LICENSE`. Nothing else: the single header holds the whole
library.

## Ours
- No changes to the header. `ConvexDecomposition.cpp` is the only translation unit that includes it,
  with `ENABLE_VHACD_IMPLEMENTATION` and its warnings silenced around the include.
- Compiled only when `JASPERENGINE_CONVEX_DECOMPOSITION` is on (the default on Windows, Linux and
  macOS; off for Web, Android and iOS, where the cache still reads and writes hulls but cannot
  decompose).

## Updating
Copy `include/VHACD.h` and `LICENSE` from the new commit, bump `ALGORITHM_VERSION` in
`ConvexDecomposition.cpp` (cached hulls of the old version are then never served), update the
commit above and in `NOTICE.md`, and run `jasperengine-editor-tests --gtest_filter='ConvexDecomposition*'`.
