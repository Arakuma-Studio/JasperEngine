// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/V1-vertex-colors.md R8, R131: the editor compresses the .vcolors files it writes (zstd,
// level 3). The engine links only zstd's decoder (a game never writes the file), so the compressor is
// the editor's, set while the editor starts.

#include "vertexcolor/VertexColorFile.h"

#include "zstd.h"

namespace {

    bool vertexColorZstd(const std::vector<uint8_t>& body, std::vector<uint8_t>& packed) {
        packed.resize(ZSTD_compressBound(body.size()));
        const size_t size = ZSTD_compress(packed.data(), packed.size(), body.data(), body.size(), 3);
        if (ZSTD_isError(size)) return false;
        packed.resize(size);
        return true;
    }

    [[maybe_unused]] const bool vertexColorCompressorSet = [] {
        jasperengine::vertexcolor::setBodyCompressor(&vertexColorZstd);
        return true;
    }();

}
