// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The GPU formats a texture can be stored in (spec S14 D9): what the cook writes into KTX2
// files and what the loader uploads. Colour textures stay UNORM on upload (shaders decode sRGB
// themselves, D7); the sRGB flag travels next to the format.

#ifndef TEXTUREFORMAT_H
#define TEXTUREFORMAT_H

#include "Export.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace jasperengine {

    enum class GpuTextureFormat : uint8_t {
        R8, RG8, RGBA8, R16, RGB565, RGBA4, RGB9E5, RGBA16F,
        BC1, BC3, BC4, BC5, BC6H, BC7,
        ETC2_RGB8, ETC2_RGBA8, EAC_R11, EAC_RG11,
        ASTC_4x4, ASTC_5x5, ASTC_6x6, ASTC_8x8, ASTC_10x10, ASTC_12x12
    };

    constexpr size_t kGpuTextureFormatCount = (size_t)GpuTextureFormat::ASTC_12x12 + 1;

    // one mip level of an upload: every face (or array slice) of it, contiguous
    struct TextureLevel {
        int width = 0;
        int height = 0;
        const uint8_t* data = nullptr;
        size_t size = 0;
    };

    // which formats the device samples with filtering (SystemRender::isTextureFormatSupported)
    struct TextureFormatSupport {
        std::array<bool, kGpuTextureFormatCount> supported{};
        bool operator()(GpuTextureFormat format) const { return supported[(size_t)format]; }
        void set(GpuTextureFormat format, bool value){ supported[(size_t)format] = value; }
    };

    struct GpuFormatInfo {
        uint8_t blockDim;       // texels per block side (1 for uncompressed formats)
        uint8_t bytesPerBlock;  // bytes per block (per texel when uncompressed)
        bool compressed;
        bool alpha;             // stores an alpha channel
        bool hdr;               // float data
        uint8_t channels;       // channels the shader reads meaningful data from
        const char* name;       // "BC7", "ASTC_6x6": the enum string of Lua, MCP and reports
    };

    JASPERENGINE_API const GpuFormatInfo& gpuFormatInfo(GpuTextureFormat format);
    JASPERENGINE_API const char* gpuFormatName(GpuTextureFormat format);
    // case-insensitive, "ASTC_6x6" or "astc_6x6"
    JASPERENGINE_API bool gpuFormatFromName(std::string_view name, GpuTextureFormat& format);

    // bytes of one face of a width x height level (blocks rounded up)
    JASPERENGINE_API size_t gpuFormatLevelBytes(GpuTextureFormat format, int width, int height);
    // the dimension of mip `level` of a `size` base
    inline int mipDimension(int size, int level){ int d = size >> level; return d < 1 ? 1 : d; }
    // levels of a full chain down to 1x1
    JASPERENGINE_API int fullMipCount(int width, int height);

    // KTX2's vkFormat of a format (a VkFormat value), and back (UNDEFINED, 0, is a Basis file)
    JASPERENGINE_API uint32_t gpuFormatToVkFormat(GpuTextureFormat format, bool srgb, bool alpha = true);
    JASPERENGINE_API bool gpuFormatFromVkFormat(uint32_t vkFormat, GpuTextureFormat& format, bool& srgb);

}

#endif // TEXTUREFORMAT_H
