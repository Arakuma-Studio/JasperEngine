# engine/core/terrain (T1 R1, R8): the operation engine, its stores and the surface query. The
# sources build with no floating-point contraction, so a map's bytes are the same on every CPU, and
# outside unity batches, since those flags would reach their neighbours.
set(JASPERENGINE_TERRAIN_SRCS
    core/terrain/HeightfieldErosion.cpp
    core/terrain/FoliageCull.cpp
    core/terrain/FoliageOps.cpp
    core/terrain/FoliageRules.cpp
    core/terrain/FoliageScatter.cpp
    core/terrain/OverlayMapStore.cpp
    core/terrain/RawMapStore.cpp
    core/terrain/TerrainErosion.cpp
    core/terrain/TerrainHoles.cpp
    core/terrain/TerrainMacroNormal.cpp
    core/terrain/TerrainMaterialCost.cpp
    core/terrain/TerrainOps.cpp
    core/terrain/TerrainPlacement.cpp
    core/terrain/TerrainQuality.cpp
    core/terrain/TerrainShape.cpp
    core/terrain/TerrainStroke.cpp
    core/terrain/TerrainSurface.cpp
    core/terrain/TreeStore.cpp
)
