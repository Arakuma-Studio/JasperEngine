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
