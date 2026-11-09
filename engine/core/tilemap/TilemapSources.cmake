# Grid tilemaps (docs/specs/S50-grid-tilemap.md): the cell store, layouts, tilesets, terrains,
# chunk building and collider building.

set(JASPERENGINE_TILEMAP_SRCS
    core/tilemap/TerrainSolver.cpp
    core/tilemap/TerrainTemplates.cpp
    core/tilemap/TileAlphaTrace.cpp
    core/tilemap/TileCells.cpp
    core/tilemap/TileGridLayout.cpp
    core/tilemap/Tileset.cpp
    core/tilemap/TilesetPool.cpp
    core/subsystem/TilemapColliders.cpp
)
