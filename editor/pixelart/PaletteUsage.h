// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// How a texture uses a palette (docs/specs/S43-palettes.md R33): the pixels of each palette
// colour (exact sRGB8 matches) and the off-palette colours with their nearest palette colour.
// No Project, ImGui or GPU: it runs on workers and in the CLI, and callers cache the report by
// the texture's content hash.

#include "pixelart/RgbaImage.h"
#include "pixelart/Rgba8.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace jasperengine::editor::pixelart {

    // The first index of each 24-bit colour of a palette (alpha ignored). With `skipTransparent`,
    // entries of alpha 0 are left out unless every entry is (PaletteMatcher's rule: a
    // transparent slot is not a colour to match).
    class ExactColorIndex {
    public:
        explicit ExactColorIndex(std::span<const jasperengine::pixelart::Rgba8> colors, bool skipTransparent = false);

        // the palette index of the colour's RGB, or -1
        int find(jasperengine::pixelart::Rgba8 color) const;

    private:
        struct Slot { uint32_t key = 0; int index = -1; };   // key: rgb + 1, 0 is empty
        std::vector<Slot> slots;
    };

    // Pixel counts per 32-bit key (a packed colour), in an open-addressed table: a picture
    // repeats few colours many times
    class ColorTally {
    public:
        void add(uint32_t key, int64_t count = 1);
        size_t size() const { return used; }
        // (key, count) for every key seen, sorted by key
        std::vector<std::pair<uint32_t, int64_t>> entries() const;

    private:
        struct Slot { uint64_t key = 0; int64_t count = 0; };   // key + 1, 0 is empty
        void grow();
        std::vector<Slot> slots;
        size_t used = 0;
    };

    // 0xRRGGBBAA
    inline uint32_t packRgba(jasperengine::pixelart::Rgba8 color) {
        return (uint32_t(color.r) << 24) | (uint32_t(color.g) << 16) | (uint32_t(color.b) << 8) | uint32_t(color.a);
    }

    struct PaletteUsageReport {
        std::vector<int64_t> counts;     // per palette index: the counted pixels of exactly its RGB
        int64_t offPalettePixels = 0;
        int64_t offColorCount = 0;       // distinct off-palette RGB colours (offColors may list fewer)

        struct OffColor {
            jasperengine::pixelart::Rgba8 color;   // alpha 255
            int64_t count = 0;
            int nearest = -1;            // OKLab; -1 with an empty palette
            float deltaE = 0.0f;         // OKLab distance x 100
        };
        std::vector<OffColor> offColors; // most used first, then by RGB

        uint64_t contentHash = 0;        // imageContentHash of the measured image
    };

    // Counts the pixels whose alpha is at least `alphaThreshold` (the others are transparent and
    // not counted) by exact RGB. A duplicate palette colour counts on its first index; a slot of
    // alpha 0 matches nothing. `maxOffColors` limits the off-palette colours listed (and their
    // nearest searches); negative lists them all.
    PaletteUsageReport measureUsage(const RgbaImage& image, std::span<const jasperengine::pixelart::Rgba8> palette,
                                    int alphaThreshold = 1, int maxOffColors = -1);

    // FNV-1a (64-bit) over the width, the height and the pixel bytes: the cache key of a
    // texture's usage, the same on every platform
    uint64_t imageContentHash(const RgbaImage& image);

}
