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
# T4, the module terrainworld (JASPERENGINE_TERRAIN_WORLD): its pure code, under the same flags as the
# rest of this folder, then its runtime, streaming and collision
set(JASPERENGINE_TERRAIN_WORLD_PURE_SRCS
    core/terrain/world/TerrainTileFormat.cpp
    core/terrain/world/TerrainWorldData.cpp
    core/terrain/world/TerrainWorldSelect.cpp
    core/terrain/world/TiledMapStore.cpp
    core/terrain/world/TileStreamer.cpp
    core/terrain/world/WorldChunkSamples.cpp
    core/terrain/world/WorldImport.cpp
)
set(JASPERENGINE_TERRAIN_WORLD_SRCS
    ${JASPERENGINE_TERRAIN_WORLD_PURE_SRCS}
    core/object/TerrainWorld.cpp
    core/physics/TerrainWorldCollision.cpp
    core/script/binding/TerrainWorldClassesLua.cpp
    core/subsystem/TerrainWorldRuntime.cpp
    core/subsystem/TerrainWorldSystem.cpp
)
