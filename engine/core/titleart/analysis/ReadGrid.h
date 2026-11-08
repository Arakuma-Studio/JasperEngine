// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S83-title-effects-generators.md R13, R14, D8, AC25)
// - Purpose: the CPU's view of a layer for S83's analysis (glint highlights, particle emitters,
//   speed-line silhouettes, lightning edges, ink splatter's distance): the layer box-averaged
//   on the GPU into cells fixed in canvas px, packed into RGBA8 and read back with one
//   FrameReadback::readTexture (4 bytes a pixel works on every backend; no float readback).
//   Pachislop's readGrid (src/effects/b/common.ts) and readAlphaGrid (src/generators/
//   particles.ts) ported onto that packing.
// - Packings: Luma = R mean alpha, G/B mean premultiplied luminance as 16-bit fixed point over
//   0..4 (1/16384 steps), A mean saturation; Color = the mean colour (straight sRGB bytes) and
//   mean alpha, decoded to premultiplied linear.
// - Layout: cell (0, 0)'s lower-left corner at (x0, y0) canvas px about the canvas centre,
//   +y up; rows of the CPU arrays run bottom first (Pachislop's), so ported loops read as theirs.
//   frameGridSpec lays the lattice over the whole render from the canvas centre in blocks of 16
//   cells (Pachislop GRID_ALIGN), so every px_scale reads the same cells.
// - Caching: a grid read with a non-zero key (GeneratorContext::followKey: the source's pixels
//   and place) is kept by (key, spec) and never read back twice (R13); key 0 always reads.
//   Bounded to the 64 most recent grids.
// - Thread-safety: main thread (GPU passes and the readback).

#pragma once

#include "Export.h"
#include "titleart/render/TitleEffectContext.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace jasperengine::titleart::analysis {

    enum class GridPacking : uint8_t { Luma, Color };

    struct GridSpec {
        double x0 = 0.0, y0 = 0.0;      // cell (0, 0)'s lower-left corner, canvas px, +y up
        double cellComp = 4.0;          // cell side, canvas px
        int w = 0, h = 0;
        int maxTaps = 16;               // taps a side per cell, at most (min(maxTaps, round(cell px)))
        bool ceilTaps = false;          // taps = ceil(cell px) instead of round (readAlphaGrid)
        GridPacking packing = GridPacking::Luma;
    };

    struct Grid {
        int w = 0, h = 0;
        double cell = 0.0;              // cell side, target px
        double cellComp = 0.0;          // cell side, canvas px
        double x0 = 0.0, y0 = 0.0;      // as GridSpec
        std::vector<float> alpha;       // mean alpha per cell, row 0 at the bottom
        std::vector<float> lum;         // mean premultiplied luminance (Luma), 0..4
        std::vector<float> sat;         // mean saturation (Luma), 0..1
        std::vector<float> rgba;        // mean premultiplied linear colour and alpha (Color), 4 per cell
        bool empty() const { return w <= 0 || h <= 0; }
    };

    // Pachislop readGrid's lattice over the whole render (ctx.frame) at cellComp canvas px
    JASPERENGINE_API GridSpec frameGridSpec(const GeneratorContext& ctx, double cellComp);

    // `src` (a target of the context's size) read into the grid; null when the pass could not
    // draw (a failure, or its program still compiling: the step runs again)
    JASPERENGINE_API std::shared_ptr<const Grid> readGrid(const GeneratorContext& ctx, const TargetRef& src, uint64_t key, const GridSpec& spec);

    // the Luma grid of an image already on the CPU (the bake's straight sRGB output, 8 bits a
    // channel, rows top first), its pixels at `scale` output px per canvas px over `frame` (canvas
    // px, image space) of a canvas of this size: the lattice of frameGridSpec over the whole
    // canvas, cells past the image empty. The bake's highlight anchors come from it with no GPU
    // pass and no readback of their own (R14)
    JASPERENGINE_API Grid gridFromImage(const std::vector<uint8_t>& rgba, int width, int height, const TitleRectI& frame, double scale,
                                        int canvasWidth, int canvasHeight, double cellComp);

    // the readbacks made since the process started (AC25's counter)
    JASPERENGINE_API uint64_t gridReadbacks();
    // forgets every kept grid (device loss, tests)
    JASPERENGINE_API void clearGridCache();

    // chamfer distance (in cells) from every cell to the nearest cell with alpha >= 0.5
    // (Pachislop gridDistance)
    JASPERENGINE_API std::vector<float> gridDistance(int w, int h, const std::vector<float>& alpha);

    // the packed luminance of one cell as written by the Grid program and decoded here (tests)
    JASPERENGINE_API float decodeGridLuma(uint8_t hi, uint8_t lo);
    JASPERENGINE_API void encodeGridLuma(float lum, uint8_t& hi, uint8_t& lo);

}
