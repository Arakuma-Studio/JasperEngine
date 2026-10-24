// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

namespace jasperengine::editor {

// Encodes and writes a 16-bit single-channel (grayscale) PNG. `pixels` holds
// width*height 16-bit samples in native little-endian order (low byte first);
// the encoder swaps to PNG's big-endian, applies adaptive per-row filtering and
// DEFLATEs via stb_image_write's zlib compressor (stb cannot emit 16-bit PNGs).
// Returns false on invalid arguments or a write/compress failure.
//
// This lives in the editor utility layer (not in any feature/UI/AI module) so the
// terrain editor and the AI terrain capability share one encoder.
bool writeGray16Png(const std::filesystem::path& path, int width, int height,
                    const unsigned char* pixels, std::size_t pixelByteCount);

// Encodes a 16-bit PNG in memory: `channels` 1 (grey), 2 (grey+alpha), 3 (RGB) or 4 (RGBA)
// interleaved 16-bit samples per pixel, native little-endian like writeGray16Png's. Same
// filtering and compression. Returns false on invalid arguments or a compress failure.
// (The texture synthesis writes 16-bit companion maps with it, editor/texsynth.)
bool encodePng16(int width, int height, int channels, const unsigned char* pixels,
                 std::size_t pixelByteCount, std::vector<unsigned char>& png);

} // namespace jasperengine::editor
