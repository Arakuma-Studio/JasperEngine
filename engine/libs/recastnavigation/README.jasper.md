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
- **Local changes:** one patch, the avoidance priority of DetourCrowd (`PATCHES.md`). Every
  changed line is marked `JasperEngine patch`, as the licence requires of altered sources.

To upgrade, vendor the new tag the same way, re-apply `PATCHES.md`, update the version above and
`NOTICE.md`, and run `NavCrowdTest.PriorityYields` and the navigation suites.
