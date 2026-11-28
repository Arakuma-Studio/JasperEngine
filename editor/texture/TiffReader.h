// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S94-rendering-anim-gaps.md F50, AC47)
// - Purpose: the editor's baseline TIFF reader, so a .tif / .tiff is a texture source the cook
//   turns into KTX2 (a game never decodes TIFF). It reads both byte orders (II, MM), the first
//   image (IFD) only, strips or tiles, compression None (1), LZW (5), Deflate (8, 32946) and
//   PackBits (32773), Predictor 2 (horizontal differencing), 8 or 16 unsigned bits per sample,
//   grey (WhiteIsZero, BlackIsZero) or RGB with one optional extra sample read as alpha
//   (associated alpha is unpremultiplied; an unspecified extra sample counts as associated, as
//   libtiff reads it), planar configuration 1 (chunky) or 2 (planar). Written from the TIFF 6.0
//   specification.
// - Everything else (JPEG or other compression, palette, CMYK, YCbCr, Lab, float or signed
//   samples, 1 / 4 / 32 bits, BigTIFF, Predictor 3, FillOrder 2) is refused with a message that
//   names the supported set (TIFF_SUPPORTED_SET). Orientation is ignored: rows are top first.
// - Untrusted input: every offset and byte count is checked against the file, sizes are capped
//   (32,768 px a side, 2^27 pixels, 2^26 pixels a strip or tile), a compressed chunk never
//   inflates past its expected size; a malformed file fails with a message, never a crash.
// - Thread-safety: pure functions.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::editor {

    // the tail of every refusal message
    inline constexpr const char* TIFF_SUPPORTED_SET =
        "supported: baseline TIFF, first image, strips or tiles, compression None, LZW, Deflate or PackBits, "
        "8 or 16 bits per sample, grey or RGB with optional alpha";

    struct TiffImage {
        int width = 0, height = 0;
        int bitDepth = 8;               // 8 or 16: the file's
        // straight RGBA, rows top first, 4 samples a pixel of 1 byte or 2 (native uint16)
        std::vector<uint8_t> rgba;
    };

    // false with `error` (one sentence)
    bool readTiff(const uint8_t* data, size_t size, TiffImage& out, std::string& error);

    // the image as 8-bit RGBA (16-bit samples rounded)
    std::vector<uint8_t> tiffToRgba8(const TiffImage& image);

}
