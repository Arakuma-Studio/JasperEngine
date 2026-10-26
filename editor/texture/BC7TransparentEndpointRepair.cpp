// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Copied from OpenEngine (Engine/Include/Assets/Textures/BC7TransparentEndpointRepair.h and
// Engine/Source/Assets/Textures/BC7TransparentEndpointRepair.cpp), Copyright (c) 2026 Gamecrafters
// Guild Limited and contributors, MIT License (see NOTICE.md). Changes: the namespace and the
// integer type names.

#include "BC7TransparentEndpointRepair.h"

#include <algorithm>
#include <cassert>
#include <cstddef>

namespace jasperengine::editor {

namespace
{
    constexpr uint32_t kBlockDim = 4;
    constexpr size_t kBlockBytes = 16;
    constexpr size_t kTexelBytes = 4;
    constexpr size_t kAlphaOffset = 3;

    // Where one alpha mode with p-bits stores its endpoint alphas and p-bits. Endpoint e
    // keeps its alpha bits at FirstAlphaBit + e * AlphaBits and its p-bit at FirstPBit + e.
    struct EndpointAlphaLayout
    {
        uint32_t AlphaBits;
        uint32_t FirstAlphaBit;
        uint32_t FirstPBit;
        uint32_t EndpointCount;
    };

    constexpr EndpointAlphaLayout kMode6Layout{7, 49, 63, 2};
    constexpr EndpointAlphaLayout kMode7Layout{5, 74, 94, 4};

    // The mode is the position of the lowest set bit: mode 6 is 0b1000000, mode 7 is 0b10000000.
    constexpr uint8_t kMode6Mask = 0x7F;
    constexpr uint8_t kMode6Bits = 0x40;
    constexpr uint8_t kMode7Bits = 0x80;

    const EndpointAlphaLayout* FindEndpointAlphaLayout(const uint8_t* block)
    {
        if ((block[0] & kMode6Mask) == kMode6Bits)
            return &kMode6Layout;
        if (block[0] == kMode7Bits)
            return &kMode7Layout;
        return nullptr;
    }

    bool ReadBit(const uint8_t* block, uint32_t bit)
    {
        return ((block[bit / 8] >> (bit % 8)) & 1u) != 0;
    }

    bool AreBitsZero(const uint8_t* block, uint32_t firstBit, uint32_t count)
    {
        for (uint32_t bit = firstBit; bit < firstBit + count; ++bit)
            if (ReadBit(block, bit))
                return false;
        return true;
    }

    void ClearBit(uint8_t* block, uint32_t bit)
    {
        block[bit / 8] = static_cast<uint8_t>(block[bit / 8] & ~(1u << (bit % 8)));
    }

    // Only texels inside the image count: the encoder pads a partial edge block with
    // texels that are not part of the source.
    bool BlockHasTransparentTexel(const uint8_t* rgba, uint32_t width, uint32_t height, uint32_t blockX, uint32_t blockY)
    {
        const uint32_t x0 = blockX * kBlockDim;
        const uint32_t y0 = blockY * kBlockDim;
        const uint32_t x1 = std::min(x0 + kBlockDim, width);
        const uint32_t y1 = std::min(y0 + kBlockDim, height);
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x)
                if (rgba[(static_cast<size_t>(y) * width + x) * kTexelBytes + kAlphaOffset] == 0)
                    return true;
        return false;
    }

    void ClearTransparentEndpointPBits(uint8_t* block, const EndpointAlphaLayout& layout)
    {
        for (uint32_t endpoint = 0; endpoint < layout.EndpointCount; ++endpoint)
        {
            const uint32_t pBit = layout.FirstPBit + endpoint;
            if (AreBitsZero(block, layout.FirstAlphaBit + endpoint * layout.AlphaBits, layout.AlphaBits) &&
                ReadBit(block, pBit))
                ClearBit(block, pBit);
        }
    }
}

void RepairBC7TransparentEndpoints(const uint8_t* rgba, uint32_t width, uint32_t height, std::span<uint8_t> blocks)
{
    const uint32_t blocksWide = (width + kBlockDim - 1) / kBlockDim;
    const uint32_t blocksHigh = (height + kBlockDim - 1) / kBlockDim;
    const size_t expectedBytes = static_cast<size_t>(blocksWide) * blocksHigh * kBlockBytes;
    assert(blocks.size() == expectedBytes && "BC7 payload does not match the source extent");
    if (blocks.size() != expectedBytes)
        return;

    for (uint32_t blockY = 0; blockY < blocksHigh; ++blockY)
    {
        for (uint32_t blockX = 0; blockX < blocksWide; ++blockX)
        {
            uint8_t* block = blocks.data() + (static_cast<size_t>(blockY) * blocksWide + blockX) * kBlockBytes;
            const EndpointAlphaLayout* layout = FindEndpointAlphaLayout(block);
            if (layout && BlockHasTransparentTexel(rgba, width, height, blockX, blockY))
                ClearTransparentEndpointPBits(block, *layout);
        }
    }
}

} // namespace jasperengine::editor
