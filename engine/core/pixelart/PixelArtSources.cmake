# The pixel-art family's runtime (docs/specs/S41-S52, PIXEL-ART-LEDGER.md): colour maths,
# palettes, Palette Swap and Palette Quantize (S43). Built without floating-point contraction,
# so the colour maths gives the same bits on every platform (S43 R12, R47).

set(JASPERENGINE_PIXELART_SRCS
    core/pixelart/Palette.cpp
    core/pixelart/PaletteCodec.cpp
    core/pixelart/PaletteMaterial.cpp
    core/pixelart/PaletteQuantize.cpp
    core/pixelart/PaletteRampTexture.cpp
    core/pixelart/PaletteSwap.cpp
    core/pixelart/PixelColor.cpp
    # S48 Pixel FX
    core/pixelart/PixelFxKey.cpp
    core/pixelart/HitStop.cpp
    core/pixelart/PixelFxPlayer.cpp
    core/pixelart/Glitch.cpp
    core/pixelart/ScreenTransition.cpp
)

if(MSVC)
    set(JASPERENGINE_PIXELART_FLAGS /fp:precise)
else()
    set(JASPERENGINE_PIXELART_FLAGS -ffp-contract=off)
endif()
set_source_files_properties(${JASPERENGINE_PIXELART_SRCS} PROPERTIES COMPILE_OPTIONS "${JASPERENGINE_PIXELART_FLAGS}")
