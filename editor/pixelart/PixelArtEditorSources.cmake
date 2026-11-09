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
    ${EDITOR_DIR}/sprite/SpriteAtlasCook.cpp
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
        ${EDITOR_DIR}/sprite/SpriteAtlasCook.cpp
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

# S43 (Palettes): palette formats, Lospec, the built-in palettes, the ramp generator, Apply Palette
# and the palette edits, the Palette window and the pixel_art MCP tools.
# palette formats, Lospec, built-in palettes, ramp generator (S43 M2, R5-R10)
list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/pixelart/PaletteFormats.cpp
    ${EDITOR_DIR}/pixelart/Lospec.cpp
    ${EDITOR_DIR}/pixelart/BuiltinPalettes.cpp
    ${EDITOR_DIR}/pixelart/RampGenerator.cpp
)

# Apply Palette / Reduce Colours and usage counts (S43 M5, R23, R24, R33)
list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/pixelart/RgbaImage.cpp
    ${EDITOR_DIR}/pixelart/PaletteUsage.cpp
    ${EDITOR_DIR}/pixelart/ApplyPalette.cpp
)

# the palette edits shared by edit_palette and the Palette window (S43 R10, R11, R32, R39)
list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/pixelart/PaletteEdit.cpp
    ${EDITOR_DIR}/pixelart/PaletteAssets.cpp
)

# S49 (Pixel Particles): the presets, the offscreen capture (contact sheets, Bake to Sprite
# Sheet) and its pixel-art checks
list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/pixelart/particles/PixelParticlePresets.cpp
    ${EDITOR_DIR}/pixelart/particles/PixelPresetCreate.cpp
)

# the colour decisions give the same bits on every platform, as engine/core/pixelart's
if(MSVC)
    set(JASPERENGINE_EDITOR_PIXELART_FLAGS /fp:precise)
else()
    set(JASPERENGINE_EDITOR_PIXELART_FLAGS -ffp-contract=off)
endif()
set_source_files_properties(
    ${EDITOR_DIR}/pixelart/ApplyPalette.cpp
    ${EDITOR_DIR}/pixelart/PaletteUsage.cpp
    ${EDITOR_DIR}/pixelart/RampGenerator.cpp
    PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_EDITOR_PIXELART_FLAGS}")

# inspectors and checks (S43 M4, M6): the Palette Swap checks, using a named palette, the
# inspectors' palette field
list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/pixelart/PaletteSwapChecks.cpp
    ${EDITOR_DIR}/pixelart/PaletteUse.cpp
    ${EDITOR_DIR}/window/PaletteAssetField.cpp
)

# the Palette window and its dialogs (S43 M3, M5)
list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/window/PaletteWindow.cpp
    ${EDITOR_DIR}/window/ApplyPaletteDialog.cpp
    ${EDITOR_DIR}/widget/PaletteSwatchAdapter.cpp
)

# S42 (Pixel Art Project Mode & Validator): Project Settings > Pixel Art, the validator (its input
# from a project, the rules, the fixes), Pixel Art Mode's hooks, the preset, the Pixel Art Check
# window and check_pixel_art
list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/pixelart/PixelArtSettings.cpp
    ${EDITOR_DIR}/pixelart/PixelArtCheck.cpp
    ${EDITOR_DIR}/pixelart/PixelArtRules.cpp
    ${EDITOR_DIR}/pixelart/PixelArtInput.cpp
    ${EDITOR_DIR}/pixelart/PixelArtFix.cpp
    ${EDITOR_DIR}/pixelart/PixelArtMode.cpp
    ${EDITOR_DIR}/pixelart/PixelArtPreset.cpp
    ${EDITOR_DIR}/pixelart/PixelArtAssign.cpp
    ${EDITOR_DIR}/command/type/ProjectSettingsValuesCmd.cpp
    ${EDITOR_DIR}/mcp/tools/PixelArtTools.cpp
    ${EDITOR_DIR}/window/PixelArtCheckWindow.cpp
    ${EDITOR_DIR}/window/dialog/PixelArtPresetDialog.cpp
)
jasperengine_skip_unity_build(
    REASON "S42's validator: file-local helpers (option, readText, isFontPath) shared in name with the sprite and Aseprite modules"
    SOURCES
        ${EDITOR_DIR}/pixelart/PixelArtRules.cpp
        ${EDITOR_DIR}/pixelart/PixelArtInput.cpp
        ${EDITOR_DIR}/pixelart/PixelArtFix.cpp
        ${EDITOR_DIR}/mcp/tools/PixelArtTools.cpp
)

# S51 (Pixel Fonts): preview_font, the font parts of get_asset_info and write_file, the Bitmap Font
# Inspector, Create > Bitmap Font, the Text Inspector's pixel rows, hot reload and cooking
list(APPEND JASPERENGINE_EDITOR_SOURCES
    ${EDITOR_DIR}/mcp/tools/FontTools.cpp
    ${EDITOR_DIR}/reload/handlers/BitmapFonts.cpp
    ${EDITOR_DIR}/pixelart/BitmapFontCreate.cpp
    ${EDITOR_DIR}/window/BitmapFontInspector.cpp
    ${EDITOR_DIR}/window/InspectorPixelFont.cpp
)
