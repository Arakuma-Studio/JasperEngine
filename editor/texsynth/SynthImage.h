// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

// Images of the texture synthesis (editor-only): decoded with stb_image keeping their channel
// count and bit depth, so a companion map (a 16-bit height map, an RGB normal map) is copied texel
// for texel into the synthesized output in its own format.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace jasperengine::editor::texsynth {

    // A decoded image: `channels` interleaved samples per pixel (1 grey, 2 grey+alpha, 3 RGB,
    // 4 RGBA) of `bitDepth` 8 or 16 bits, rows top to bottom, no padding. 16-bit samples are
    // native-endian uint16 values stored in `data` (2 bytes each).
    struct Image {
        int width = 0;
        int height = 0;
        int channels = 0;
        int bitDepth = 8;
        std::vector<uint8_t> data;

        bool empty() const { return width <= 0 || height <= 0 || data.empty(); }
        size_t bytesPerPixel() const { return static_cast<size_t>(channels) * (bitDepth / 8); }
        size_t pixelCount() const { return static_cast<size_t>(width) * static_cast<size_t>(height); }
        const uint8_t* pixel(int x, int y) const { return data.data() + (static_cast<size_t>(y) * width + x) * bytesPerPixel(); }
        uint8_t* pixel(int x, int y) { return data.data() + (static_cast<size_t>(y) * width + x) * bytesPerPixel(); }
        bool sameFormat(const Image& other) const { return channels == other.channels && bitDepth == other.bitDepth; }

        // A zero-filled image (false for an invalid size or format)
        static Image make(int width, int height, int channels, int bitDepth);
    };

    // "8-bit RGB", "16-bit grey+alpha", ...
    std::string describeFormat(const Image& image);

    // Decodes PNG (8/16-bit, any colour type), JPEG, TGA, BMP, PSD, GIF (first frame), HDR is
    // refused. Keeps the channel count and, for 16-bit PNGs, the 16-bit samples. `sha1` (when
    // given) receives the SHA-1 of the file's bytes as 40 hex digits. `maxSide` > 0: an image
    // larger on a side is refused from its header, before anything is decoded.
    bool loadImage(const std::filesystem::path& path, Image& out, std::string& error, std::string* sha1 = nullptr, int maxSide = 0);
    bool loadImageFromMemory(const uint8_t* bytes, size_t size, Image& out, std::string& error, int maxSide = 0);
    // The size of an image file from its header (nothing decoded); false when it is not readable
    bool probeImage(const std::filesystem::path& path, int& width, int& height);

    // PNG bytes of the image (8-bit through stb_image_write, 16-bit through util/PngWriter),
    // without any ancillary chunk
    bool encodePng(const Image& image, std::vector<uint8_t>& png);

    // SHA-1 of a byte buffer, 40 hex digits
    std::string sha1Hex(const uint8_t* bytes, size_t size);

    // The image as 8-bit RGBA (16-bit samples rounded, grey replicated, opaque when it has no
    // alpha): the working format of the synthesis
    Image toRgba8(const Image& image);

    // The image in a format at least as wide (channels: grey -> grey+alpha / RGB -> RGBA, bit
    // depth 8 -> 16 as v * 257): lossless, the original samples are recoverable. False when the
    // target is narrower than the source.
    bool widenTo(const Image& image, int channels, int bitDepth, Image& out);

    // The smallest format holding every image losslessly (colour if any is colour, alpha if any
    // has alpha, 16-bit if any is 16-bit)
    void unionFormat(const std::vector<const Image*>& images, int& channels, int& bitDepth);

    // RGBA8 helpers of the synthesis (Image with channels 4, bitDepth 8)
    Image resizeRgba8(const Image& image, int width, int height);
    // Gaussian blur (separable, clamped edges). `cancel` set: checked every row, an empty image
    // comes back when it is set. Works on a few rows at a time (no full-size float copy).
    Image blurRgba8(const Image& image, float sigma, const std::atomic<bool>* cancel = nullptr);
    Image greyRgba8(const Image& image);   // Rec. 709 luma in R, G and B, opaque
    // Remaps `image`'s grey (R) values so its histogram matches `reference`'s (both grey RGBA8)
    void matchHistogramRgba8(Image& image, const Image& reference);

}
