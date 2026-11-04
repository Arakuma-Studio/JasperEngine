// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The one image type of the pixel-art family (docs/specs/S44-aseprite-import.md R10): width,
// height, straight-alpha RGBA8 rows top-left first, and an optional index plane of the same size.
// While the index plane is present the RGBA is its palette lookup (the transparent index, if
// any, reads as {0, 0, 0, 0}); an operation that cannot keep that invariant drops the plane and
// says so in its return value. No GPU, editor UI or project dependency: the codec, the importer
// and S43/S46/S47/S48/S50 use it from jobs and the CLI.
//
// Pixels are plain RGBA8 bytes: PixelRgba is {r, g, b, a}, and pixels() is the w*h*4 bytes.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart {

    using PixelRgba = std::array<uint8_t, 4>;

    struct IntPoint {
        int x = 0, y = 0;
        bool operator==(const IntPoint& o) const { return x == o.x && y == o.y; }
    };

    struct IntSize {
        int w = 0, h = 0;
        bool operator==(const IntSize& o) const { return w == o.w && h == o.h; }
    };

    struct IntRect {
        int x = 0, y = 0, w = 0, h = 0;
        bool empty() const { return w <= 0 || h <= 0; }
        bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
        IntRect intersect(const IntRect& o) const;
        IntRect unite(const IntRect& o) const;   // an empty side is ignored
        bool operator==(const IntRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
        bool operator!=(const IntRect& o) const { return !(*this == o); }
    };

    class PixelImage {
    public:
        // a side above this is refused (the largest Aseprite canvas)
        static constexpr int kMaxSide = 65535;

        PixelImage() = default;
        // transparent ({0, 0, 0, 0}); a size out of [0, kMaxSide] gives an empty image
        PixelImage(int width, int height);

        // RGBA from a palette (4 bytes per entry, RGBA): indices past the palette and the
        // transparent index (-1: none) read {0, 0, 0, 0}. Empty when the sizes disagree.
        static PixelImage fromIndices(int width, int height, std::vector<uint8_t> indices,
                                      std::span<const uint8_t> paletteRgba, int transparentIndex);

        int width() const { return w; }
        int height() const { return h; }
        bool empty() const { return w == 0 || h == 0; }
        IntRect bounds() const { return {0, 0, w, h}; }

        // the RGBA bytes, w*h*4, rows top-left first
        std::span<const uint8_t> pixels() const { return rgba; }
        // writable RGBA: drops the index plane (true in *droppedIndices when there was one)
        std::span<uint8_t> editPixels(bool* droppedIndices = nullptr);
        const uint8_t* row(int y) const { return rgba.data() + static_cast<size_t>(y) * w * 4; }

        bool hasIndices() const { return indexed; }
        std::span<const uint8_t> indices() const { return idx; }
        int transparentIndex() const { return transparent; }
        // Sets the plane and recomputes the RGBA from the palette; false (unchanged) when the
        // size is not w*h
        bool setIndices(std::vector<uint8_t> indices, std::span<const uint8_t> paletteRgba, int transparentIndex);
        // Recomputes the RGBA of an indexed image from another palette (a palette edit); false without indices
        bool repaint(std::span<const uint8_t> paletteRgba, int transparentIndex);
        void dropIndices();

        // {0, 0, 0, 0} outside the image
        PixelRgba at(int x, int y) const;
        uint8_t indexAt(int x, int y) const;   // 0 without indices or outside
        // Sets one pixel (inside only); drops the index plane: true when there was one
        bool set(int x, int y, PixelRgba color);
        // Sets one pixel's index and its palette colour; false outside or without indices
        bool setIndex(int x, int y, uint8_t index, PixelRgba color);

        // The rect's pixels; parts outside the image are transparent (the transparent index on an
        // indexed image; an indexed image without one loses its plane when the rect leaves it)
        PixelImage crop(const IntRect& rect) const;
        // Copies src's pixels at (x, y), clipped, alpha included (no blending). The plane stays
        // when both images have one (same palette assumed); returns false when it was dropped.
        bool blit(const PixelImage& src, int x, int y);
        // Aseprite's normal blend of src over this image at (x, y) with a 0-255 opacity (its
        // rgba_blender_normal, bit for bit). Drops the index plane: true when there was one.
        bool blendOver(const PixelImage& src, int x, int y, uint8_t opacity);
        // The bounds of the pixels whose alpha is above the threshold; {0, 0, 0, 0} when none is
        IntRect opaqueBounds(uint8_t alphaThreshold = 0) const;
        void flipX();
        void flipY();
        // Fills with a colour (drops the plane: true when there was one)
        bool fill(PixelRgba color);

        // XXH64 of the size, the RGBA and, when present, the index plane: equal images hash equal
        // on every platform and run
        uint64_t hash() const;
        bool operator==(const PixelImage& other) const;
        bool operator!=(const PixelImage& other) const { return !(*this == other); }

        // 8-bit RGBA PNGs (any PNG stb_image reads, converted to RGBA8; no index plane)
        static std::optional<PixelImage> loadPng(const std::filesystem::path& path, std::string* error);
        static std::optional<PixelImage> decodePng(std::span<const uint8_t> bytes, std::string* error);
        // A deterministic 8-bit RGBA PNG (empty string for an empty image)
        std::string encodePng() const;
        // Atomic write; false on failure
        bool savePng(const std::filesystem::path& path) const;

    private:
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        std::vector<uint8_t> idx;
        bool indexed = false;
        int transparent = -1;

        static void lookup(PixelImage& image, std::span<const uint8_t> paletteRgba);
    };

}
