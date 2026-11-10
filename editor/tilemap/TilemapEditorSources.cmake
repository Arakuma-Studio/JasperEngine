# Grid tilemaps' editor code (docs/specs/S50-grid-tilemap.md): tileset editing, brushes, undo,
# importers, the Tile Palette and the Tileset Editor.

list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/tilemap/TilemapPixelArt.cpp
    ${EDITOR_DIR}/tilemap/import/ImportJsonReader.cpp
    ${EDITOR_DIR}/tilemap/import/LdtkImporter.cpp
    ${EDITOR_DIR}/tilemap/import/TiledImporter.cpp
    ${EDITOR_DIR}/tilemap/import/TilemapFromAseprite.cpp
    ${EDITOR_DIR}/tilemap/import/TilemapImportApply.cpp
    ${EDITOR_DIR}/scenetool/SceneTool.cpp
    ${EDITOR_DIR}/tilemap/TilePageTextures.cpp
    ${EDITOR_DIR}/tilemap/TilePaletteWindow.cpp
    ${EDITOR_DIR}/tilemap/TilesetEditorWindow.cpp
    ${EDITOR_DIR}/tilemap/TilemapSceneTools.cpp
    ${EDITOR_DIR}/reload/handlers/Tilesets.cpp
    ${EDITOR_DIR}/tilemap/TilemapBrush.cpp
    ${EDITOR_DIR}/tilemap/TilemapCellsCmd.cpp
    ${EDITOR_DIR}/tilemap/TilemapComposite.cpp
    ${EDITOR_DIR}/tilemap/TilemapConvert.cpp
    ${EDITOR_DIR}/tilemap/TilesetEdit.cpp
)
