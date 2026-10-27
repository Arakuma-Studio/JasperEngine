// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TextureFormat.h"

#include <array>
#include <cctype>

using namespace jasperengine;

namespace {
    // in enum order
    constexpr std::array<GpuFormatInfo, kGpuTextureFormatCount> formatTable = {{
        {1, 1, false, false, false, 1, "R8"},
        {1, 2, false, false, false, 2, "RG8"},
        {1, 4, false, true, false, 4, "RGBA8"},
        {1, 2, false, false, false, 1, "R16"},
        {1, 2, false, false, false, 3, "RGB565"},
        {1, 2, false, true, false, 4, "RGBA4"},
        {1, 4, false, false, true, 3, "RGB9E5"},
        {1, 8, false, true, true, 4, "RGBA16F"},
        {4, 8, true, false, false, 3, "BC1"},
        {4, 16, true, true, false, 4, "BC3"},
        {4, 8, true, false, false, 1, "BC4"},
        {4, 16, true, false, false, 2, "BC5"},
        {4, 16, true, false, true, 3, "BC6H"},
        {4, 16, true, true, false, 4, "BC7"},
        {4, 8, true, false, false, 3, "ETC2_RGB8"},
        {4, 16, true, true, false, 4, "ETC2_RGBA8"},
        {4, 8, true, false, false, 1, "EAC_R11"},
        {4, 16, true, false, false, 2, "EAC_RG11"},
        {4, 16, true, true, false, 4, "ASTC_4x4"},
        {5, 16, true, true, false, 4, "ASTC_5x5"},
        {6, 16, true, true, false, 4, "ASTC_6x6"},
        {8, 16, true, true, false, 4, "ASTC_8x8"},
        {10, 16, true, true, false, 4, "ASTC_10x10"},
        {12, 16, true, true, false, 4, "ASTC_12x12"},
    }};

    // VkFormat values (vulkan_core.h), the vkFormat field of KTX2
    enum : uint32_t {
        VK_R4G4B4A4_UNORM_PACK16 = 2,
        VK_R5G6B5_UNORM_PACK16 = 4,
        VK_R8_UNORM = 9, VK_R8_SRGB = 15,
        VK_R8G8_UNORM = 16, VK_R8G8_SRGB = 22,
        VK_R8G8B8A8_UNORM = 37, VK_R8G8B8A8_SRGB = 43,
        VK_R16_UNORM = 70,
        VK_R16G16B16A16_SFLOAT = 97,
        VK_E5B9G9R9_UFLOAT_PACK32 = 123,
        VK_BC1_RGB_UNORM = 131, VK_BC1_RGB_SRGB = 132, VK_BC1_RGBA_UNORM = 133, VK_BC1_RGBA_SRGB = 134,
        VK_BC3_UNORM = 137, VK_BC3_SRGB = 138,
        VK_BC4_UNORM = 139, VK_BC5_UNORM = 141,
        VK_BC6H_UFLOAT = 143,
        VK_BC7_UNORM = 145, VK_BC7_SRGB = 146,
        VK_ETC2_R8G8B8_UNORM = 147, VK_ETC2_R8G8B8_SRGB = 148,
        VK_ETC2_R8G8B8A8_UNORM = 151, VK_ETC2_R8G8B8A8_SRGB = 152,
        VK_EAC_R11_UNORM = 153, VK_EAC_R11G11_UNORM = 155,
        VK_ASTC_4x4_UNORM = 157, VK_ASTC_4x4_SRGB = 158,
        VK_ASTC_5x5_UNORM = 161, VK_ASTC_5x5_SRGB = 162,
        VK_ASTC_6x6_UNORM = 165, VK_ASTC_6x6_SRGB = 166,
        VK_ASTC_8x8_UNORM = 171, VK_ASTC_8x8_SRGB = 172,
        VK_ASTC_10x10_UNORM = 179, VK_ASTC_10x10_SRGB = 180,
        VK_ASTC_12x12_UNORM = 183, VK_ASTC_12x12_SRGB = 184,
    };

    struct VkPair {
        GpuTextureFormat format;
        uint32_t unorm;
        uint32_t srgb;   // 0: no sRGB variant (the data is linear, the flag is the DFD's alone)
    };

    constexpr std::array<VkPair, kGpuTextureFormatCount> vkTable = {{
        {GpuTextureFormat::R8, VK_R8_UNORM, VK_R8_SRGB},
        {GpuTextureFormat::RG8, VK_R8G8_UNORM, VK_R8G8_SRGB},
        {GpuTextureFormat::RGBA8, VK_R8G8B8A8_UNORM, VK_R8G8B8A8_SRGB},
        {GpuTextureFormat::R16, VK_R16_UNORM, 0},
        {GpuTextureFormat::RGB565, VK_R5G6B5_UNORM_PACK16, 0},
        {GpuTextureFormat::RGBA4, VK_R4G4B4A4_UNORM_PACK16, 0},
        {GpuTextureFormat::RGB9E5, VK_E5B9G9R9_UFLOAT_PACK32, 0},
        {GpuTextureFormat::RGBA16F, VK_R16G16B16A16_SFLOAT, 0},
        {GpuTextureFormat::BC1, VK_BC1_RGB_UNORM, VK_BC1_RGB_SRGB},
        {GpuTextureFormat::BC3, VK_BC3_UNORM, VK_BC3_SRGB},
        {GpuTextureFormat::BC4, VK_BC4_UNORM, 0},
        {GpuTextureFormat::BC5, VK_BC5_UNORM, 0},
        {GpuTextureFormat::BC6H, VK_BC6H_UFLOAT, 0},
        {GpuTextureFormat::BC7, VK_BC7_UNORM, VK_BC7_SRGB},
        {GpuTextureFormat::ETC2_RGB8, VK_ETC2_R8G8B8_UNORM, VK_ETC2_R8G8B8_SRGB},
        {GpuTextureFormat::ETC2_RGBA8, VK_ETC2_R8G8B8A8_UNORM, VK_ETC2_R8G8B8A8_SRGB},
        {GpuTextureFormat::EAC_R11, VK_EAC_R11_UNORM, 0},
        {GpuTextureFormat::EAC_RG11, VK_EAC_R11G11_UNORM, 0},
        {GpuTextureFormat::ASTC_4x4, VK_ASTC_4x4_UNORM, VK_ASTC_4x4_SRGB},
        {GpuTextureFormat::ASTC_5x5, VK_ASTC_5x5_UNORM, VK_ASTC_5x5_SRGB},
        {GpuTextureFormat::ASTC_6x6, VK_ASTC_6x6_UNORM, VK_ASTC_6x6_SRGB},
        {GpuTextureFormat::ASTC_8x8, VK_ASTC_8x8_UNORM, VK_ASTC_8x8_SRGB},
        {GpuTextureFormat::ASTC_10x10, VK_ASTC_10x10_UNORM, VK_ASTC_10x10_SRGB},
        {GpuTextureFormat::ASTC_12x12, VK_ASTC_12x12_UNORM, VK_ASTC_12x12_SRGB},
    }};
}

const GpuFormatInfo& jasperengine::gpuFormatInfo(GpuTextureFormat format){
    const size_t index = (size_t)format;
    return formatTable[index < kGpuTextureFormatCount ? index : (size_t)GpuTextureFormat::RGBA8];
}

const char* jasperengine::gpuFormatName(GpuTextureFormat format){
    return gpuFormatInfo(format).name;
}

bool jasperengine::gpuFormatFromName(std::string_view name, GpuTextureFormat& format){
    for (size_t i = 0; i < kGpuTextureFormatCount; i++){
        const std::string_view candidate = formatTable[i].name;
        if (candidate.size() != name.size())
            continue;
        bool same = true;
        for (size_t c = 0; c < name.size() && same; c++)
            same = std::tolower((unsigned char)candidate[c]) == std::tolower((unsigned char)name[c]);
        if (same){
            format = (GpuTextureFormat)i;
            return true;
        }
    }
    return false;
}

size_t jasperengine::gpuFormatLevelBytes(GpuTextureFormat format, int width, int height){
    const GpuFormatInfo& info = gpuFormatInfo(format);
    if (width <= 0 || height <= 0)
        return 0;
    const size_t blocksX = ((size_t)width + info.blockDim - 1) / info.blockDim;
    const size_t blocksY = ((size_t)height + info.blockDim - 1) / info.blockDim;
    return blocksX * blocksY * info.bytesPerBlock;
}

int jasperengine::fullMipCount(int width, int height){
    int levels = 1;
    int size = width > height ? width : height;
    while (size > 1){
        size >>= 1;
        levels++;
    }
    return levels;
}

uint32_t jasperengine::gpuFormatToVkFormat(GpuTextureFormat format, bool srgb, bool alpha){
    const VkPair& pair = vkTable[(size_t)format < kGpuTextureFormatCount ? (size_t)format : (size_t)GpuTextureFormat::RGBA8];
    if (format == GpuTextureFormat::BC1 && alpha){
        return srgb ? VK_BC1_RGBA_SRGB : VK_BC1_RGBA_UNORM;
    }
    return (srgb && pair.srgb) ? pair.srgb : pair.unorm;
}

bool jasperengine::gpuFormatFromVkFormat(uint32_t vkFormat, GpuTextureFormat& format, bool& srgb){
    if (vkFormat == VK_BC1_RGBA_UNORM || vkFormat == VK_BC1_RGBA_SRGB){
        format = GpuTextureFormat::BC1;
        srgb = vkFormat == VK_BC1_RGBA_SRGB;
        return true;
    }
    for (const VkPair& pair : vkTable){
        if (pair.unorm == vkFormat || (pair.srgb != 0 && pair.srgb == vkFormat)){
            format = pair.format;
            srgb = pair.srgb != 0 && pair.srgb == vkFormat;
            return true;
        }
    }
    return false;
}
