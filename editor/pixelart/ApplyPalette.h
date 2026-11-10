// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Apply Palette, Reduce Colours and Remap Indices on one RGBA8 picture (docs/specs/S43-palettes.md
// R23, R24, R40), with the statistics, per-colour usage, lost colours and verdict the dialog,
// `apply_palette` and S42's palette rule report. No Project, ImGui or GPU: it runs on workers and
// in the CLI, single-threaded and deterministic.

#include "pixelart/PixelColor.h"
#include "pixelart/PixelProblem.h"
#include "pixelart/Rgba8.h"
#include "pixelart/RgbaImage.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace jasperengine::editor::pixelart {

    enum class Dither : uint8_t { None, Bayer2, Bayer4, Bayer8, FloydSteinberg };

    // "none", "bayer2", "bayer4", "bayer8", "floyd_steinberg"
    const char* ditherName(Dither dither);
    bool parseDither(std::string_view name, Dither& out);

    // (verdictName: PixelProblem.h)
    // apply_palette measure_only's exit code (R43): 0, 3, 4
    int verdictExitCode(Verdict verdict);

    struct ApplyRequest {
        std::vector<jasperengine::pixelart::Rgba8> palette;
        std::vector<uint16_t> locked;            // indices of `palette` that Reduce Colours keeps
        jasperengine::pixelart::ColorMetric metric = jasperengine::pixelart::ColorMetric::OkLab;
        Dither dither = Dither::None;
        float ditherStrength = 1.0f;             // 0..1
        int alphaThreshold = 0;                  // 0 keeps alpha; 1..255: Binary at that threshold
        int reduce = 0;                          // 2..256: Reduce Colours to that many first (0: off)
    };

    struct ApplyStats {
        int colorsBefore = 0, colorsAfter = 0;   // distinct RGBA of the visible (alpha > 0) pixels
        int64_t pixelsChanged = 0;               // pixels whose bytes differ
        int64_t pixelsMapped = 0;                // pixels that went to a palette colour
        double meanDeltaE = 0.0, maxDeltaE = 0.0;   // OKLab distance x 100, over the mapped pixels
    };

    struct ColorUsage {
        int index = 0;
        jasperengine::pixelart::Rgba8 color;
        int64_t count = 0;                       // mapped pixels that took this index
    };

    // An 8-connected group of mapped pixels of like colour (within 0.06 OKLab of the group's
    // mean) that each ended more than 0.06 OKLab from their own colour (ritmo's lost colours)
    struct LostGroup {
        jasperengine::pixelart::Rgba8 original;  // the group's mean colour
        jasperengine::pixelart::Rgba8 mapped;    // the palette colour most of it became
        int count = 0;
        int x = 0, y = 0, w = 0, h = 0;          // bounding box
    };

    struct ApplyResult {
        RgbaImage image;
        std::vector<jasperengine::pixelart::Rgba8> palette;   // the one applied: the request's, or the one Reduce Colours found
        ApplyStats stats;
        std::vector<ColorUsage> usage;           // every index of `palette`, in order
        std::vector<LostGroup> lost;             // groups of 2 pixels or more, largest first, then in reading order
        Verdict verdict = Verdict::Clean;
    };

    // Every pixel of alpha > 0 (Keep) or alpha >= the threshold (Binary) goes to a palette
    // colour; a pixel of alpha 0 stays as it is, and with Binary one below the threshold becomes
    // {0, 0, 0, 0}. A palette colour of alpha 0 is never matched. An exact palette colour stays
    // itself in every dither mode. With an empty palette (and no reduce) no pixel is mapped.
    ApplyResult applyPalette(const RgbaImage& image, const ApplyRequest& request);

    // Palette Swap's lookup on the CPU (R14, AC9): a pixel whose RGB equals a colour of `from`
    // (the first one; the colour's alpha is ignored) becomes `to` at that index, with alpha
    // pixel.a x target.a / 255 rounded; any other pixel, or an index past the end of `to`, is
    // left unchanged
    RgbaImage remapIndices(const RgbaImage& image, std::span<const jasperengine::pixelart::Rgba8> from,
                           std::span<const jasperengine::pixelart::Rgba8> to);

}
