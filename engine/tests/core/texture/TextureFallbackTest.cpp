// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// What a device that cannot sample a cooked format gets (spec S14 AC9, R11, D5): with "no BPTC"
// a BC7 file loads as RGBA8 and logs once; a browser with only S3TC transcodes Basis to BC1 /
// BC3; with nothing compressed and the low-memory fallback on, RGB565 / RGBA4. The decodes are
// checked against hand-built blocks, so no encoder is needed.

#include "texture/Ktx2.h"
#include "texture/TextureData.h"
#include "texture/TextureDecode.h"
#include "texture/TextureFallback.h"

#include "LogCapture.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

using namespace jasperengine;

namespace {

    using Block = std::array<uint8_t, 16>;

    void setBits(uint8_t* block, uint32_t firstBit, uint32_t count, uint32_t value){
        for (uint32_t i = 0; i < count; i++){
            const uint32_t bit = firstBit + i;
            const uint8_t mask = (uint8_t)(1u << (bit % 8));
            if ((value >> i) & 1u)
                block[bit / 8] |= mask;
            else
                block[bit / 8] &= (uint8_t)~mask;
        }
    }

    // BC7 mode 6, both endpoints (r, g, b, a) with p-bits 0: every texel decodes to it exactly
    // (even 8-bit values: the 7-bit endpoint is the value / 2)
    Block bc7Solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a){
        Block block{};
        setBits(block.data(), 0, 7, 0x40);
        const uint8_t channels[4] = {r, g, b, a};
        for (int c = 0; c < 4; c++){
            setBits(block.data(), 7 + (uint32_t)c * 14, 7, channels[c] >> 1);
            setBits(block.data(), 7 + (uint32_t)c * 14 + 7, 7, channels[c] >> 1);
        }
        return block;
    }

    // BC4: both endpoints v, every index 0
    std::array<uint8_t, 8> bc4Solid(uint8_t v){
        return {v, v, 0, 0, 0, 0, 0, 0};
    }

    std::vector<uint8_t> repeat(const uint8_t* block, size_t blockBytes, size_t count){
        std::vector<uint8_t> out;
        for (size_t i = 0; i < count; i++)
            out.insert(out.end(), block, block + blockBytes);
        return out;
    }

    std::vector<uint8_t> writeFile(GpuTextureFormat format, int width, int height, std::vector<std::vector<uint8_t>> levels,
                                   std::map<std::string, std::string> keyValues = {}){
        Ktx2::WriteDesc desc;
        desc.format = format;
        desc.width = (uint32_t)width;
        desc.height = (uint32_t)height;
        for (const auto& level : levels)
            desc.levels.push_back(level);
        desc.keyValues = std::move(keyValues);
        std::vector<uint8_t> file;
        std::string error;
        EXPECT_TRUE(Ktx2::write(desc, file, error)) << error;
        return file;
    }

    TextureFormatSupport everythingBut(std::initializer_list<GpuTextureFormat> missing){
        TextureFormatSupport support;
        for (size_t f = 0; f < kGpuTextureFormatCount; f++)
            support.set((GpuTextureFormat)f, true);
        for (GpuTextureFormat format : missing)
            support.set(format, false);
        return support;
    }

    TextureFormatSupport only(std::initializer_list<GpuTextureFormat> present){
        TextureFormatSupport support;
        for (GpuTextureFormat format : present)
            support.set(format, true);
        return support;
    }

    Ktx2Info basisInfo(Ktx2Codec codec, const std::string& flags, const std::string& type = "default"){
        Ktx2Info info;
        info.codec = codec;
        info.keyValues["JE.type"] = type;
        info.keyValues["JE.flags"] = flags;
        return info;
    }

}

TEST(TextureFallback, NoBptcLoadsBc7AsRgba8AndLogsOnce){
    TextureFallback::resetLogOnce();
    const Block level0 = bc7Solid(200, 100, 50, 254);
    const Block level1 = bc7Solid(10, 20, 30, 40);
    const std::vector<uint8_t> file = writeFile(GpuTextureFormat::BC7, 8, 8,
                                                {repeat(level0.data(), 16, 4), repeat(level1.data(), 16, 1)},
                                                {{"JE.flags", "alpha"}, {"JE.logical", "8,8"}});
    const TextureFormatSupport noBptc = everythingBut({GpuTextureFormat::BC6H, GpuTextureFormat::BC7});

    test::LogCapture log;
    for (int load = 0; load < 2; load++){
        TextureData data;
        std::string error;
        ASSERT_TRUE(data.loadKtx2(file.data(), (unsigned int)file.size(), 0, &noBptc, false, &error)) << error;
        EXPECT_EQ(data.getGpuFormat(), GpuTextureFormat::RGBA8);
        ASSERT_EQ(data.getLevels().size(), 2u);
        const uint8_t* p0 = data.getLevels()[0].data;
        EXPECT_EQ(p0[0], 200); EXPECT_EQ(p0[1], 100); EXPECT_EQ(p0[2], 50); EXPECT_EQ(p0[3], 254);
        const uint8_t* p1 = data.getLevels()[1].data;
        EXPECT_EQ(p1[0], 10); EXPECT_EQ(p1[3], 40);
        EXPECT_EQ(data.getLevels()[0].size, (size_t)8 * 8 * 4);
        EXPECT_TRUE(data.isTransparent());
    }
    EXPECT_EQ(log.count(0, "BC7 is not supported by this GPU"), 1u);

    // a device with BPTC uploads the blocks as they are
    TextureData native;
    const TextureFormatSupport all = everythingBut({});
    std::string error;
    ASSERT_TRUE(native.loadKtx2(file.data(), (unsigned int)file.size(), 0, &all, false, &error));
    EXPECT_EQ(native.getGpuFormat(), GpuTextureFormat::BC7);
    EXPECT_TRUE(native.isCompressed());
    EXPECT_EQ(native.getData(), nullptr);
}

TEST(TextureFallback, DesktopDecodesSingleAndTwoChannelFormats){
    const auto bc4 = bc4Solid(77);
    const std::vector<uint8_t> file = writeFile(GpuTextureFormat::BC4, 4, 4, {repeat(bc4.data(), 8, 1)}, {{"JE.type", "single_channel"}});
    const TextureFormatSupport noRgtc = everythingBut({GpuTextureFormat::BC4, GpuTextureFormat::BC5});
    TextureData data;
    std::string error;
    ASSERT_TRUE(data.loadKtx2(file.data(), (unsigned int)file.size(), 0, &noRgtc, false, &error)) << error;
    EXPECT_EQ(data.getGpuFormat(), GpuTextureFormat::R8);
    EXPECT_EQ(data.getLevels()[0].data[0], 77);
    EXPECT_EQ(TextureDecode::decodedFormat(GpuTextureFormat::BC5), GpuTextureFormat::RG8);
    EXPECT_EQ(TextureDecode::decodedFormat(GpuTextureFormat::BC6H), GpuTextureFormat::RGBA16F);
    EXPECT_EQ(TextureDecode::decodedFormat(GpuTextureFormat::RGB9E5), GpuTextureFormat::RGBA16F);
}

TEST(TextureFallback, WebWithOnlyS3tcPicksBc1AndBc3){
    const TextureFormatSupport s3tc = only({GpuTextureFormat::BC1, GpuTextureFormat::BC3, GpuTextureFormat::RGBA8,
                                            GpuTextureFormat::R8, GpuTextureFormat::RG8});
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, ""), s3tc, false).upload, GpuTextureFormat::BC1);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, "alpha"), s3tc, false).upload, GpuTextureFormat::BC3);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::UASTC, ""), s3tc, false).upload, GpuTextureFormat::BC1);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::UASTC, "alpha"), s3tc, false).upload, GpuTextureFormat::BC3);
    // normal and single-channel content has no S3TC entry: its two- and one-channel last resorts
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::UASTC, "normalRG", "normal"), s3tc, false).upload, GpuTextureFormat::RG8);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, "", "single_channel"), s3tc, false).upload, GpuTextureFormat::R8);
}

TEST(TextureFallback, TranscodeListsFollowD5){
    using F = GpuTextureFormat;
    const TextureFormatSupport everything = everythingBut({});
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, ""), everything, false).upload, F::ETC2_RGB8);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, "alpha"), everything, false).upload, F::ETC2_RGBA8);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::UASTC, ""), everything, false).upload, F::ASTC_4x4);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::UASTC, "normalRG", "normal"), everything, false).upload, F::BC5);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, "", "single_channel"), everything, false).upload, F::BC4);
    Ktx2Info hdr = basisInfo(Ktx2Codec::UASTC_HDR, "", "hdr");
    EXPECT_EQ(TextureFallback::plan(hdr, everything, false).upload, F::BC6H);
    EXPECT_EQ(TextureFallback::plan(hdr, everythingBut({F::BC6H}), false).upload, F::RGB9E5);
    EXPECT_EQ(TextureFallback::plan(hdr, everythingBut({F::BC6H, F::RGB9E5}), false).upload, F::RGBA16F);

    // the whole list of a desktop browser without ETC2: BC1 for ETC1S, BC7 for UASTC
    const TextureFormatSupport desktopWeb = everythingBut({F::ETC2_RGB8, F::ETC2_RGBA8, F::EAC_R11, F::EAC_RG11, F::ASTC_4x4,
                                                           F::ASTC_5x5, F::ASTC_6x6, F::ASTC_8x8, F::ASTC_10x10, F::ASTC_12x12});
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, ""), desktopWeb, false).upload, F::BC1);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::UASTC, "alpha"), desktopWeb, false).upload, F::BC7);
    const TextureUploadPlan plan = TextureFallback::plan(basisInfo(Ktx2Codec::UASTC, ""), desktopWeb, false);
    EXPECT_EQ(plan.reason, "ASTC_4x4 is not supported by this GPU, transcoded to BC7");

    // ETC1S has no ASTC 4x4 or BC7 entry (deviation 26): a device with neither ETC2 nor S3TC
    // takes the last resort, whatever else it samples
    const TextureFormatSupport astcBc7 = only({F::ASTC_4x4, F::BC7, F::BC5, F::BC4, F::RGBA8, F::R8, F::RG8});
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, ""), astcBc7, false).upload, F::RGBA8);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, "alpha"), astcBc7, false).upload, F::RGBA8);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, "", "single_channel"), only({F::ASTC_4x4, F::R8}), false).upload, F::R8);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::UASTC, ""), astcBc7, false).upload, F::ASTC_4x4);
    for (TextureContent content : {TextureContent::ColorOpaque, TextureContent::ColorAlpha, TextureContent::Normal, TextureContent::Single}){
        for (F format : TextureFallback::transcodeCandidates(Ktx2Codec::ETC1S, content, true)){
            EXPECT_NE(format, F::ASTC_4x4);
            EXPECT_NE(format, F::BC7);
        }
    }
}

TEST(TextureFallback, NothingAvailableWithLowMemoryPicksRgb565AndRgba4){
    using F = GpuTextureFormat;
    const TextureFormatSupport bare = only({F::R8, F::RG8, F::RGBA8, F::RGB565, F::RGBA4, F::RGBA16F});
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, ""), bare, true).upload, F::RGB565);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::UASTC, "alpha"), bare, true).upload, F::RGBA4);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, ""), bare, false).upload, F::RGBA8);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::UASTC, "normalRG", "normal"), bare, true).upload, F::RG8);
    EXPECT_EQ(TextureFallback::plan(basisInfo(Ktx2Codec::ETC1S, "", "single_channel"), bare, true).upload, F::R8);

    // a raw BC7 file on such a device: decoded, then packed into RGBA4
    TextureFallback::resetLogOnce();
    const Block solid = bc7Solid(255, 0, 0, 128);
    const std::vector<uint8_t> file = writeFile(F::BC7, 4, 4, {repeat(solid.data(), 16, 1)}, {{"JE.flags", "alpha"}});
    TextureData data;
    std::string error;
    ASSERT_TRUE(data.loadKtx2(file.data(), (unsigned int)file.size(), 0, &bare, true, &error)) << error;
    EXPECT_EQ(data.getGpuFormat(), F::RGBA4);
    uint16_t packed;
    std::memcpy(&packed, data.getLevels()[0].data, 2);
    EXPECT_EQ(packed >> 12, 15);           // red
    EXPECT_EQ(packed & 0xF, (128 * 15 + 127) / 255);
    // no low-memory fallback: RGBA8
    TextureData full;
    ASSERT_TRUE(full.loadKtx2(file.data(), (unsigned int)file.size(), 0, &bare, false, &error));
    EXPECT_EQ(full.getGpuFormat(), F::RGBA8);
}

TEST(TextureFallback, UnavailableFormatsFailWithTheReason){
    TextureFormatSupport none;
    const std::vector<uint8_t> file = writeFile(GpuTextureFormat::ASTC_4x4, 4, 4, {std::vector<uint8_t>(16, 0)});
    TextureData data;
    std::string error;
    // the engine has no ASTC decoder (the editor registers one)
    if (!TextureDecode::canDecode(GpuTextureFormat::ASTC_4x4)){
        EXPECT_FALSE(data.loadKtx2(file.data(), (unsigned int)file.size(), 0, &none, false, &error));
        EXPECT_NE(error.find("ASTC_4x4 is not supported by this GPU"), std::string::npos) << error;
    }
}

TEST(TextureFallback, PackedFormatsTheDeviceCannotSampleWidenToRgba8){
    // desktop GL 3.3 core does not report RGB565 (sokol PATCHES.md): a file cooked to it still
    // loads, widened to RGBA8 with each channel's bits replicated
    using F = GpuTextureFormat;
    const TextureFormatSupport noPacked = everythingBut({F::RGB565, F::RGBA4});
    const uint16_t red565 = (uint16_t)((31 << 11) | (32 << 5) | 1);
    const uint16_t half4444 = (uint16_t)((15 << 12) | (8 << 8) | (1 << 4) | 7);
    for (const auto& [format, packed] : {std::pair<F, uint16_t>{F::RGB565, red565}, std::pair<F, uint16_t>{F::RGBA4, half4444}}){
        SCOPED_TRACE(gpuFormatName(format));
        std::vector<uint8_t> level(4 * 4 * 2);
        for (size_t i = 0; i < 16; i++)
            std::memcpy(level.data() + i * 2, &packed, 2);
        const std::vector<uint8_t> file = writeFile(format, 4, 4, {level});
        TextureData data;
        std::string error;
        ASSERT_TRUE(data.loadKtx2(file.data(), (unsigned int)file.size(), 0, &noPacked, false, &error)) << error;
        EXPECT_EQ(data.getGpuFormat(), F::RGBA8);
        ASSERT_EQ(data.getLevels()[0].size, (size_t)4 * 4 * 4);
        const uint8_t* t = data.getLevels()[0].data;
        if (format == F::RGB565){
            EXPECT_EQ(t[0], 255); EXPECT_EQ(t[1], (32 << 2) | (32 >> 4)); EXPECT_EQ(t[2], (1 << 3) | (1 >> 2)); EXPECT_EQ(t[3], 255);
        }else{
            EXPECT_EQ(t[0], 255); EXPECT_EQ(t[1], 0x88); EXPECT_EQ(t[2], 0x11); EXPECT_EQ(t[3], 0x77);
        }
        // a device that samples it keeps the 16-bit texels
        TextureData native;
        const TextureFormatSupport all = everythingBut({});
        ASSERT_TRUE(native.loadKtx2(file.data(), (unsigned int)file.size(), 0, &all, false, &error)) << error;
        EXPECT_EQ(native.getGpuFormat(), format);
    }
}
