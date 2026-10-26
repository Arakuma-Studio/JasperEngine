// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The GPU format table (spec S14 D9): block sizes, level bytes, names and the KTX2 vkFormats.

#include "texture/TextureFormat.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(TextureFormat, LevelBytesRoundBlocksUp){
    EXPECT_EQ(gpuFormatLevelBytes(GpuTextureFormat::RGBA8, 3, 5), 3u * 5u * 4u);
    EXPECT_EQ(gpuFormatLevelBytes(GpuTextureFormat::BC1, 5, 5), 2u * 2u * 8u);
    EXPECT_EQ(gpuFormatLevelBytes(GpuTextureFormat::BC7, 1, 1), 16u);
    EXPECT_EQ(gpuFormatLevelBytes(GpuTextureFormat::ASTC_6x6, 2048, 2048), 342u * 342u * 16u);
    EXPECT_EQ(gpuFormatLevelBytes(GpuTextureFormat::ASTC_12x12, 13, 1), 2u * 1u * 16u);
    EXPECT_EQ(gpuFormatLevelBytes(GpuTextureFormat::RGB9E5, 10, 10), 400u);
    EXPECT_EQ(gpuFormatLevelBytes(GpuTextureFormat::RGBA16F, 2, 2), 32u);
    EXPECT_EQ(gpuFormatLevelBytes(GpuTextureFormat::RGB565, 7, 3), 42u);
}

TEST(TextureFormat, MipCounts){
    EXPECT_EQ(fullMipCount(2048, 2048), 12);
    EXPECT_EQ(fullMipCount(1000, 600), 10);
    EXPECT_EQ(fullMipCount(1, 1), 1);
    EXPECT_EQ(mipDimension(1000, 3), 125);
    EXPECT_EQ(mipDimension(5, 9), 1);
}

TEST(TextureFormat, NamesRoundTrip){
    for (size_t f = 0; f < kGpuTextureFormatCount; f++){
        const GpuTextureFormat format = (GpuTextureFormat)f;
        GpuTextureFormat parsed;
        ASSERT_TRUE(gpuFormatFromName(gpuFormatName(format), parsed)) << gpuFormatName(format);
        EXPECT_EQ(parsed, format);
    }
    GpuTextureFormat parsed;
    EXPECT_TRUE(gpuFormatFromName("astc_6x6", parsed));
    EXPECT_EQ(parsed, GpuTextureFormat::ASTC_6x6);
    EXPECT_FALSE(gpuFormatFromName("BC2", parsed));
}

TEST(TextureFormat, VkFormatsRoundTrip){
    for (size_t f = 0; f < kGpuTextureFormatCount; f++){
        const GpuTextureFormat format = (GpuTextureFormat)f;
        for (bool srgb : {false, true}){
            const uint32_t vk = gpuFormatToVkFormat(format, srgb);
            GpuTextureFormat back;
            bool backSrgb = false;
            ASSERT_TRUE(gpuFormatFromVkFormat(vk, back, backSrgb)) << gpuFormatName(format);
            EXPECT_EQ(back, format);
            // formats with an sRGB variant say so; linear ones never do
            if (backSrgb){
                EXPECT_TRUE(srgb);
            }
        }
    }
    EXPECT_EQ(gpuFormatToVkFormat(GpuTextureFormat::BC7, true), 146u);
    EXPECT_EQ(gpuFormatToVkFormat(GpuTextureFormat::ASTC_8x8, false), 171u);
    EXPECT_EQ(gpuFormatToVkFormat(GpuTextureFormat::BC1, false, false), 131u);
    EXPECT_EQ(gpuFormatToVkFormat(GpuTextureFormat::BC1, false, true), 133u);
}

TEST(TextureFormat, InfoMatchesTheFormats){
    EXPECT_TRUE(gpuFormatInfo(GpuTextureFormat::BC1).compressed);
    EXPECT_FALSE(gpuFormatInfo(GpuTextureFormat::BC1).alpha);
    EXPECT_TRUE(gpuFormatInfo(GpuTextureFormat::BC7).alpha);
    EXPECT_TRUE(gpuFormatInfo(GpuTextureFormat::BC6H).hdr);
    EXPECT_EQ(gpuFormatInfo(GpuTextureFormat::ASTC_10x10).blockDim, 10);
    EXPECT_EQ(gpuFormatInfo(GpuTextureFormat::EAC_RG11).channels, 2);
    EXPECT_FALSE(gpuFormatInfo(GpuTextureFormat::R16).compressed);
}
