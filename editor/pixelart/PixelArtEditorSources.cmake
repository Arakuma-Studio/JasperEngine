# The editor's pixel-art family code (editor/pixelart, PIXEL-ART-LEDGER.md): S43's palette formats,
# Lospec, the built-in palettes, the ramp generator, Apply Palette and the palette edits, the
# Palette window and the pixel_art MCP tools. One list per part, so parallel work never meets on a line.

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
