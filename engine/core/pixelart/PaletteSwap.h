// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PALETTESWAP_H
#define PIXELART_PALETTESWAP_H

// Palette Swap's lookup textures (docs/specs/S43-palettes.md R13-R17, R29, §6.5), read with
// texelFetch by includes/palette.glsl:
// - cells, 256 x 128 RGBA8: the 32^3 grid of 5-bit colours, texel (r + 32 (b % 8), g + 32 (b / 8)),
//   each channel a candidate source index + 1 (0: none);
// - rows, 256 x (16 + 1 + cycling ramps) RGBA8: row 0 the source's colours, rows 1-15 the targets
//   (rows past the last one repeat it, so a row number past the end shows the last row), row 16
//   the cycle table (per index: position in its ramp + 1, the ramp's length, its order row - 17)
//   and rows 17... each cycling ramp's indices in order, with its speed at x = 255 (signed 8.8 in r, g).
// One set of textures per distinct content, shared and reference counted (PaletteSwapCache).

#include "Export.h"
#include "pixelart/Palette.h"
#include "pixelart/PaletteBinding.h"
#include "render/TextureRender.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::pixelart {

    constexpr int kSwapCellsWidth = 256;
    constexpr int kSwapCellsHeight = 128;
    constexpr int kSwapRowsWidth = 256;
    constexpr int kSwapTargetRows = 16;
    constexpr int kSwapCycleRow = 16;
    constexpr int kSwapMaxSourceColors = 255;

    // one row of a binding, resolved: its name ("" for the source's own colours) and a colour
    // per source index
    struct PaletteSwapRow {
        std::string name;
        std::vector<Rgba8> colors;
    };

    // The textures' bytes, built on the CPU (deterministic; tests read them directly)
    struct JASPERENGINE_API PaletteSwapData {
        std::vector<uint8_t> cells;      // kSwapCellsWidth x kSwapCellsHeight x 4
        std::vector<uint8_t> rows;       // kSwapRowsWidth x rowsHeight x 4
        int rowsHeight = 0;
        int rowCount = 0;                // the rows defined (1 + targets)
        std::vector<std::string> rowNames;
        uint64_t hash = 0;               // of the bytes: equal content, one texture set

        // the source index a colour swaps from (-1: not a source colour): the shader's lookup
        int sourceIndex(Rgba8 texel) const;
        // what the shader outputs for a texel on `row` at `time` seconds (cycleSpeed: the
        // material's multiplier): the CPU twin of includes/palette.glsl's paletteSwapTexel
        Rgba8 swap(Rgba8 texel, int row, float time = 0.0f, float cycleSpeed = 1.0f, bool cycling = false) const;
    };

    // Builds the textures' bytes for a source and its rows (row 0 is taken from the source).
    // Errors: palette_too_large (more than 255 colours), palette_cell_overflow (more than 4
    // colours in one 5-bit cell, naming them); warnings: duplicate colours (the first index
    // wins), rows past 16 dropped. False on an error (out is then the identity: nothing swaps).
    JASPERENGINE_API bool buildPaletteSwapData(const Palette& source, const std::vector<PaletteSwapRow>& targets, PaletteSwapData& out,
                                               std::vector<Diagnostic>& diagnostics);
    // the identity: no cell has a candidate, so every texel draws unchanged
    JASPERENGINE_API void identityPaletteSwapData(PaletteSwapData& out);

    // The rows of a binding: the source's variants (useVariants), then each target ("<palette>"
    // by index, missing indices keeping the source's colour, or "<palette>#<variant>"), every
    // palette through PaletteLibrary. A target that does not resolve keeps the source's colours
    // (warning palette_target_missing).
    JASPERENGINE_API std::vector<PaletteSwapRow> resolvePaletteRows(const Palette& source, const PaletteBinding& binding,
                                                                    std::vector<Diagnostic>& diagnostics,
                                                                    std::vector<std::shared_ptr<const Palette>>* keepAlive = nullptr);

    struct JASPERENGINE_API PaletteSwapTextures {
        PaletteSwapTextures() = default;
        PaletteSwapTextures(const PaletteSwapTextures&) = delete;
        PaletteSwapTextures& operator=(const PaletteSwapTextures&) = delete;
        // frees the GPU textures (TextureRender is a handle that never does)
        ~PaletteSwapTextures();

        TextureRender cells;
        TextureRender rows;
        int rowCount = 1;
        std::vector<std::string> rowNames;
        uint64_t hash = 0;
        bool identity = false;
        // the runtime palettes (Palette.new) its rows were made from: a row keeps them alive
        std::vector<std::shared_ptr<const Palette>> palettes;
    };

    // The shared swap textures, by content. Main thread (it creates GPU textures). An entry no
    // binding holds any more is freed 2 s later (sweep, once a frame).
    class JASPERENGINE_API PaletteSwapCache {
    public:
        // the textures of a binding (the identity, with diagnostics saying why, when it has no
        // source or the source cannot be read or built)
        static std::shared_ptr<PaletteSwapTextures> acquire(const PaletteBinding& binding, std::vector<Diagnostic>& diagnostics);
        // the identity textures every unbound slot shares (made on first use)
        static std::shared_ptr<PaletteSwapTextures> identity();
        static void sweep(double nowSeconds);
        static void clear();
        static size_t size();   // entries alive, the identity included
        // the swap data built so far (tests: a draw that only rebinds builds nothing)
        static size_t builds();
    };

}

#endif // PIXELART_PALETTESWAP_H
