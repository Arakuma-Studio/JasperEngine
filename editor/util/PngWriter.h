// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "io/AtomicFile.h"

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
// The file is replaced atomically (a Document write: terrain maps are user data).
bool writeGray16Png(const std::filesystem::path& path, int width, int height,
                    const unsigned char* pixels, std::size_t pixelByteCount);

// An 8-bit PNG (1-4 channels) encoded in memory (stbi_write_png_to_func) and written with
// io::writeFileAtomic: a crash never leaves half an image. Cache durability for thumbnails and
// screenshots, Document for maps the user paints.
bool writePng(const std::filesystem::path& path, int width, int height, int channels, const void* pixels,
              int strideBytes, io::Durability durability = io::Durability::Document);

// Encodes a 16-bit PNG in memory: `channels` 1 (grey), 2 (grey+alpha), 3 (RGB) or 4 (RGBA)
// interleaved 16-bit samples per pixel, native little-endian like writeGray16Png's. Same
// filtering and compression. Returns false on invalid arguments or a compress failure.
// (The texture synthesis writes 16-bit companion maps with it, editor/texsynth.)
bool encodePng16(int width, int height, int channels, const unsigned char* pixels,
                 std::size_t pixelByteCount, std::vector<unsigned char>& png);

} // namespace jasperengine::editor
