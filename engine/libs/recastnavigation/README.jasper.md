# recastnavigation in JasperEngine

[recastnavigation](https://github.com/recastnavigation/recastnavigation) by Mikko Mononen and
contributors, zlib licence (`License.txt`): the navigation meshes of JasperEngine's navigation
(`engine/core/navigation/`, `docs/navigation.md`, spec `docs/specs/S19-navigation.md`).

- **Version:** 1.6.0, tag `v1.6.0`, commit `6dc1667f580357e8a2154c28b7867bea7e8ad3a7` (the latest
  release when it was vendored, 2026-10-28).
- **Vendored:** `Recast/`, `Detour/`, `DetourCrowd/`, `DetourTileCache/` and `DebugUtils/`
  (`Include/` and `Source/` of each, as upstream has them) and `License.txt`.
- **Not vendored:** RecastDemo, Tests, the examples, SDL, the upstream CMake and pkg-config
  files, the documentation.
- **Build:** `CMakeLists.txt` here replaces upstream's. `recastnavigation` (Recast, Detour,
  DetourCrowd, DetourTileCache) is linked by the engine when `JASPERENGINE_NAVIGATION` is ON;
  `recastnavigation_debugdraw` (`DebugUtils/Source/DebugDraw.cpp` only: the generic
  `duDebugDraw` primitives) by the editor, for the Scene view gizmos. The rest of DebugUtils
  (`DetourDebugDraw`, `RecastDebugDraw`, `RecastDump`) is vendored for reference and debugging
  but not compiled: the overlays build their geometry from the tiles in the engine
  (`NavMesh::debugGeometry`).
- **Configuration:** 32-bit polygon references (`DT_POLYREF64` off) and the native
  `dtQueryFilter` (`DT_VIRTUAL_QUERYFILTER` off): an area is a polygon's flag bit.
- **Local changes:** five patches (`PATCHES.md`). Every changed line is marked
  `JasperEngine patch`, as the licence requires of altered sources.
  1. The avoidance priority of DetourCrowd (ours).
  2. `dtPathCorridor::setCorridor` accepts a full corridor (upstream `9147d09`).
  3. `dtMergeCorridorStartMoved` stays within its buffer (upstream `599fd0f`).
  4. `buildPolyDetail` skips samples when no triangle is left (upstream `9f4ce64`).
  5. The proximity grid's hash wraps in unsigned arithmetic (ours).

To upgrade, vendor the new tag the same way and re-apply `PATCHES.md`: patches 2-4 are upstream
fixes, dropped when the vendored tag contains them. Update the version above and `NOTICE.md`, and
run `NavCrowdTest.PriorityYields`, the tests each patch names (patch 5 under UBSan) and the navigation suites.
