// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What the encoders share (editor/texture/*Encoder.cpp): the band size, block gathering and the
// raw KTX2 writer. Not used outside the encoders.

#pragma once

#include "TextureEncoders.h"

namespace jasperengine::editor {

    // block rows per band: fixed, so banded results never depend on the thread count (D11)
    constexpr int kTextureBandRows = 16;

    // a 4x4 block of an 8-bit level as RGBA (edge texels repeated past the border):
    // RGBA as it is, RG as (r, g, 0, 255), R as (r, r, r, 255)
    void gatherBlockRgba(const MipLevel& level, int channels, int blockX, int blockY, uint8_t rgba[64]);
    // an NxM block for ASTC-style block sizes
    void gatherBlockRgba(const MipLevel& level, int channels, int blockX, int blockY, int blockWidth, int blockHeight, uint8_t* rgba);

    // the request's levels (every face of each) as a raw-payload KTX2 with its key/values,
    // Zstandard at the choice's level
    bool writeRawKtx2(const TextureEncodeRequest& request, GpuTextureFormat format, const std::vector<std::vector<uint8_t>>& levels,
                      std::vector<uint8_t>& ktx2, std::string& error);

    const ITextureEncoder& rawTextureEncoder();
    const ITextureEncoder& bcTextureEncoder();
    const ITextureEncoder& astcTextureEncoder();
    const ITextureEncoder& basisTextureEncoder();

    // astcenc and basisu sides of the preview decoders
    bool decodeAstcLevel(GpuTextureFormat format, int width, int height, const uint8_t* data, size_t size, std::vector<uint8_t>& out);
    bool decodeEtcLevel(GpuTextureFormat format, int width, int height, const uint8_t* data, size_t size, std::vector<uint8_t>& out);
    bool decodeExr(const unsigned char* bytes, size_t length, std::vector<float>& rgba, int& width, int& height, std::string& error);
    // a Basis KTX2 to a target, every level (basisu's transcoder in the editor)
    bool transcodeBasisLevels(const std::vector<uint8_t>& file, GpuTextureFormat target, std::vector<std::vector<uint8_t>>& levels,
                              std::string& error);

}
