// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef PIXELART_PALETTEQUANTIZE_H
#define PIXELART_PALETTEQUANTIZE_H

// Palette Quantize's table (docs/specs/S43-palettes.md R19-R21, R30): per cell of the display
// colour's N^3 grid (N 32 or 64), the mix (a, b, ratio) of Yliluoma's positional dither, baked on
// the CPU; the uber pass reads the cell, compares the Bayer threshold of the art pixel with the
// ratio and shows a or b. Laid out as texelFetch textures:
// - cells, 256 x 128 (N 32) or 512 x 512 (N 64), texel (r + N (b % 8), g + N (b / 8)): r index a,
//   g index b, b ratio in 1/(dither size)^2 steps (0: a alone): 4 levels for Bayer 2 x 2, 16 for
//   4 x 4, 64 for 8 x 8, each the cell count of its matrix;
// - row, 256 x 1: the palette's colours.
// A cell that holds a palette colour bakes that colour alone, so an image already in the palette
// passes through unchanged with any dither. The bake is deterministic and single-threaded (one
// Background job, or 8 ms slices a frame on the web build without threads).

#include "Export.h"
#include "pixelart/Palette.h"
#include "pixelart/PixelColor.h"
#include "render/TextureRender.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jasperengine::pixelart {

    struct JASPERENGINE_API QuantizeLut {
        int cellsPerAxis = 32;
        int width = 0;
        int height = 0;
        std::vector<uint8_t> cells;     // width x height x 4
        std::vector<uint8_t> row;       // 256 x 4
        // palette colours sharing a cell: only the first keeps its identity (High precision helps)
        std::vector<Diagnostic> warnings;

        // the CPU twin of the uber pass's pick for a display colour at dither cell (x, y)
        Rgba8 pick(Rgba8 display, int ditherSize, int x, int y) const;
    };

    // a bake in progress, stepped until done
    class JASPERENGINE_API QuantizeLutJob {
    public:
        QuantizeLutJob(const Palette& palette, ColorMetric metric, int ditherSize, float strength, int cellsPerAxis);

        // bakes until done or the budget is spent (seconds; <= 0: to the end); true when done
        bool step(double budgetSeconds);
        // the whole table on the job system's workers (every cell is its own: the result is the
        // same bytes whatever the thread count)
        void bakeParallel();
        bool done() const { return finished; }
        float progress() const;
        const QuantizeLut& result() const { return lut; }

    private:
        void bakeRange(int begin, int end);

        PaletteMatcher matcher;
        std::vector<int> identity;      // per cell: the palette colour it holds, or -1
        float strength = 0.0f;
        bool dither = false;
        int steps = 16;
        int next = 0;
        int total = 0;
        bool finished = false;
        QuantizeLut lut;
    };

    JASPERENGINE_API std::unique_ptr<QuantizeLutJob> startQuantizeBake(const Palette& palette, ColorMetric metric, int ditherSize,
                                                                       float strength, int cellsPerAxis);
    // the whole bake at once (tests, the editor)
    JASPERENGINE_API QuantizeLut bakeQuantizeLut(const Palette& palette, ColorMetric metric, int ditherSize, float strength, int cellsPerAxis);

    // The renderer's Palette Quantize: the textures of the settings it was last given, baked in
    // the background (the previous ones stay drawn meanwhile). Main thread.
    class JASPERENGINE_API PaletteQuantizer {
    public:
        struct Request {
            std::string palette;        // the asset path
            ColorMetric metric = ColorMetric::OkLab;
            int ditherSize = 4;         // 0, 2, 4, 8
            float strength = 1.0f;
            int cellsPerAxis = 32;

            bool operator==(const Request& other) const = default;
        };

        PaletteQuantizer() = default;
        PaletteQuantizer(const PaletteQuantizer&) = delete;
        PaletteQuantizer& operator=(const PaletteQuantizer&) = delete;
        ~PaletteQuantizer();

        // once a frame while the uber pass quantizes: starts a bake when the request or the
        // palette changed (a hot reload), advances or adopts the running one
        void update(const Request& request);
        // whether textures are there to draw with
        bool ready() const { return hasTextures; }
        bool baking() const { return bake != nullptr; }
        TextureRender& cells() { return cellsTexture; }
        TextureRender& row() { return rowTexture; }
        int cellsPerAxis() const { return readyCells; }
        int ditherSize() const { return readyDither; }
        // the textures go, a running bake is left to finish unseen
        void release();

    private:
        struct Bake {
            std::unique_ptr<QuantizeLutJob> job;
            std::atomic<bool> done{false};
        };

        void adopt(const QuantizeLut& lut, int ditherSize);

        Request current;
        uint32_t currentVersion = 0;
        bool started = false;
        std::shared_ptr<Bake> bake;
        int bakeDither = 0;
        TextureRender cellsTexture;
        TextureRender rowTexture;
        bool hasTextures = false;
        int readyCells = 32;
        int readyDither = 0;
    };

}

#endif // PIXELART_PALETTEQUANTIZE_H
