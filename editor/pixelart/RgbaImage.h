// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// The RGBA8 picture Apply Palette and the usage counts work on (docs/specs/S43-palettes.md §6.4):
// a plain buffer until S44's PixelImage lands, which then takes its place.

#include "pixelart/Rgba8.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace jasperengine::editor::pixelart {

    struct RgbaImage {
        int width = 0, height = 0;
        std::vector<jasperengine::pixelart::Rgba8> pixels;   // straight alpha, rows top-left first

        RgbaImage() = default;
        RgbaImage(int w, int h, jasperengine::pixelart::Rgba8 fill = {0, 0, 0, 0})
            : width(w > 0 ? w : 0), height(h > 0 ? h : 0), pixels((size_t)width * (size_t)height, fill) {}

        size_t pixelCount() const { return (size_t)width * (size_t)height; }
        bool empty() const { return width <= 0 || height <= 0; }
        // the buffer holds exactly width x height pixels
        bool valid() const { return width >= 0 && height >= 0 && pixels.size() == pixelCount(); }

        // unchecked: (x, y) must lie inside the image
        const jasperengine::pixelart::Rgba8& at(int x, int y) const { return pixels[(size_t)y * (size_t)width + (size_t)x]; }
        void set(int x, int y, jasperengine::pixelart::Rgba8 color) { pixels[(size_t)y * (size_t)width + (size_t)x] = color; }

        bool operator==(const RgbaImage& other) const = default;
    };

    // PNG, BMP, TGA or GIF (its first frame) as RGBA8; nullopt with `error` set when the file is
    // missing or not an image
    std::optional<RgbaImage> loadRgbaImage(const std::filesystem::path& path, std::string* error = nullptr);
    // the same from bytes already read
    std::optional<RgbaImage> decodeRgbaImage(const void* bytes, size_t size, std::string* error = nullptr);

    // An RGBA8 PNG, written atomically (a Document write: textures are user data)
    bool saveRgbaPng(const RgbaImage& image, const std::filesystem::path& path);
    // the PNG's bytes; empty for an empty or invalid image
    std::string encodeRgbaPng(const RgbaImage& image);

}
