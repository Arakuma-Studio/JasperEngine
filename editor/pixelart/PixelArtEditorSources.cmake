# The pixel-art family's editor code (docs/specs/S41-S52, PIXEL-ART-LEDGER.md). S44 (Aseprite
# Import): PixelImage, the typed error and lint result types, the .aseprite codec (reader,
# writer, the ported Aseprite blenders and render plan, the JSON description), finding Aseprite,
# and the sprite packer (S45 builds atlases on it).

list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/pixelart/PixelImage.cpp
    ${EDITOR_DIR}/pixelart/lint/LintTypes.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteDocument.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteRead.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteWrite.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteBlend.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteRender.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteDescribe.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteLocator.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteImport.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteImporter.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteCli.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteContactSheet.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteSpriteSetup.cpp
    ${EDITOR_DIR}/pixelart/aseprite/AsepriteEditorHooks.cpp
    ${EDITOR_DIR}/window/AsepriteImporterView.cpp
    ${EDITOR_DIR}/window/SpritePreviewWindow.cpp
    ${EDITOR_DIR}/mcp/tools/SpriteTools.cpp
    ${EDITOR_DIR}/reload/handlers/Aseprite.cpp
    ${EDITOR_DIR}/sprite/SpritePacker.cpp
)

# S45 (Sprite Atlas): texture sheets, the .spriteatlas document, atlas builds, the inline-frame
# migration, the atlas inspector and the MCP tools
list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/sprite/SpriteSheetBuilder.cpp
    ${EDITOR_DIR}/sprite/SpriteAtlasDocument.cpp
    ${EDITOR_DIR}/sprite/SpriteAtlasBuilder.cpp
    ${EDITOR_DIR}/sprite/SpriteMigration.cpp
    ${EDITOR_DIR}/mcp/tools/SpriteAtlasTools.cpp
    ${EDITOR_DIR}/window/SpriteAtlasInspector.cpp
    ${EDITOR_DIR}/window/InspectorSpriteSheet.cpp
    ${EDITOR_DIR}/window/TextureSpriteMode.cpp
    ${EDITOR_DIR}/window/dialog/SpriteConversionDialog.cpp
)
jasperengine_skip_unity_build(
    REASON "S45's sprite modules: file-local helpers (problemJson, readText, invalid) shared in name with the Aseprite importer's"
    SOURCES
        ${EDITOR_DIR}/sprite/SpriteSheetBuilder.cpp
        ${EDITOR_DIR}/sprite/SpriteAtlasDocument.cpp
        ${EDITOR_DIR}/sprite/SpriteAtlasBuilder.cpp
        ${EDITOR_DIR}/sprite/SpriteMigration.cpp
        ${EDITOR_DIR}/mcp/tools/SpriteAtlasTools.cpp
        ${EDITOR_DIR}/window/SpriteAtlasInspector.cpp
        ${EDITOR_DIR}/window/InspectorSpriteSheet.cpp
        ${EDITOR_DIR}/window/TextureSpriteMode.cpp
)

# Unity build: the codec's sources keep file-local reader, writer and composer types, its
# blenders file-local helpers named after Aseprite's (blend_multiply, ...), the packer its bin
jasperengine_skip_unity_build(
    REASON "the .aseprite codec, the Aseprite locator and the packer: file-local Reader/Writer/Composer/bin types and helpers; the importer view: file-scope using-directives"
    SOURCES
        ${EDITOR_DIR}/pixelart/aseprite/AsepriteRead.cpp
        ${EDITOR_DIR}/pixelart/aseprite/AsepriteWrite.cpp
        ${EDITOR_DIR}/pixelart/aseprite/AsepriteBlend.cpp
        ${EDITOR_DIR}/pixelart/aseprite/AsepriteRender.cpp
        ${EDITOR_DIR}/pixelart/aseprite/AsepriteDescribe.cpp
        ${EDITOR_DIR}/pixelart/aseprite/AsepriteLocator.cpp
        ${EDITOR_DIR}/pixelart/aseprite/AsepriteImport.cpp
        ${EDITOR_DIR}/pixelart/aseprite/AsepriteImporter.cpp
        ${EDITOR_DIR}/pixelart/aseprite/AsepriteCli.cpp
        ${EDITOR_DIR}/pixelart/aseprite/AsepriteContactSheet.cpp
        ${EDITOR_DIR}/pixelart/aseprite/AsepriteSpriteSetup.cpp
        ${EDITOR_DIR}/window/AsepriteImporterView.cpp
        ${EDITOR_DIR}/sprite/SpritePacker.cpp
)
