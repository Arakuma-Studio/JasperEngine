# Grid tilemaps' editor code (docs/specs/S50-grid-tilemap.md): tileset editing, brushes, undo,
# importers, the Tile Palette and the Tileset Editor.

list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/tilemap/import/ImportJsonReader.cpp
    ${EDITOR_DIR}/tilemap/import/LdtkImporter.cpp
    ${EDITOR_DIR}/tilemap/import/TiledImporter.cpp
    ${EDITOR_DIR}/tilemap/import/TilemapFromAseprite.cpp
)
