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
