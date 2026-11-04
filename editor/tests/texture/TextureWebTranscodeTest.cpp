// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The web runtime's trimmed Basis Universal transcoder (spec S14 R21, deviations 12 and 23):
// every web cook (ETC1S colour, alpha, single channel and normal; UASTC colour, alpha, normal and
// single channel; UASTC HDR) transcoded by the engine's BasisTranscode, built with only the D5
// targets, to every format of its transcode list (TextureFallback, the last resorts included)
// gives the bytes of upstream's full transcoder, the editor's copy (basisu_encoder), level by
// level, whole and in the main thread's bands of block rows, and from a level other than the
// first. The engine's copy lives in the shared engine, the editor's in this binary.

#include <gtest/gtest.h>
#include "stream/UpstreamParticles.h"

#include "support/TextureCorpus.h"

#include "texture/BasisTranscode.h"
#include "texture/Ktx2.h"
#include "texture/TextureCooker.h"
#include "texture/TextureEncoderParts.h"
#include "texture/TextureFallback.h"

#include "basisu_transcoder.h"
#include "basisu_transcoder_uastc.h"

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
namespace fs = std::filesystem;

namespace {

    struct WebCase {
        const char* what;
        const char* corpus;
        TextureImportType type;
        TextureCookQuality quality;
        const char* format;         // an explicit web format, or "" for the table's
        const char* codec;          // what the cook must have written
    };

    // small sizes: the transcode paths, not the encoders, are under test (odd-sized mips included)
    constexpr int kWebTranscodeSize = 72;

    struct WebCookCache {
        ScratchDir scratch{"web-transcode"};
        TextureCookCache cache;
        WebCookCache(){ cache.open(scratch.path / "cache", "", 1ull << 32); }
        ~WebCookCache(){ cache.close(); }
    };

}

TEST(TextureWebTranscode, TrimmedTranscoderMatchesUpstreamOnEveryTarget){
    ASSERT_TRUE(BasisTranscode::isAvailable());
    WebCookCache web;
    TextureCookCache& cache = web.cache;

    const WebCase cases[] = {
        {"ETC1S colour", "photo", TextureImportType::Default, TextureCookQuality::Normal, "", "ETC1S"},
        {"ETC1S alpha", "foliage", TextureImportType::Default, TextureCookQuality::Normal, "", "ETC1S"},
        {"ETC1S single channel", "mask", TextureImportType::SingleChannel, TextureCookQuality::Normal, "", "ETC1S"},
        {"ETC1S normal map", "normal", TextureImportType::Normal, TextureCookQuality::Normal, "ETC1S", "ETC1S"},
        {"UASTC colour", "albedo", TextureImportType::Default, TextureCookQuality::High, "", "UASTC"},
        {"UASTC alpha", "ui", TextureImportType::Sprite, TextureCookQuality::Normal, "", "UASTC"},
        {"UASTC normal map", "normal", TextureImportType::Normal, TextureCookQuality::Normal, "", "UASTC"},
        {"UASTC single channel", "mask", TextureImportType::SingleChannel, TextureCookQuality::High, "", "UASTC"},
        {"UASTC HDR", "sky", TextureImportType::HDR, TextureCookQuality::Normal, "", "UASTC_HDR"},
    };
    int compared = 0;
    for (const WebCase& c : cases){
        SCOPED_TRACE(c.what);
        const fs::path file = writeCorpusImage(corpusImage(c.corpus, kWebTranscodeSize, kWebTranscodeSize), web.scratch.path / "src");
        TextureCookRequest request;
        request.source = file;
        request.assetPath = file.filename().generic_string();
        request.settings.type = c.type;
        request.settings.quality = c.quality;
        if (c.format[0]){
            TexturePlatformSettings& platform = request.settings.platforms[(size_t)TextureFamily::Web];
            platform.override = true;
            platform.format = c.format;
        }
        request.family = TextureFamily::Web;
        const TextureCookResult result = TextureCooker::cook(cache, request, nullptr);
        ASSERT_TRUE(result.ok) << result.error;
        ASSERT_EQ(result.record.format, c.codec);
        std::vector<uint8_t> bytes;
        ASSERT_TRUE(cache.readArtifact(result.record.key, bytes));

        // the engine's way in: Ktx2::read (Zstandard inflated), a session per target
        Ktx2MemorySource source(bytes);
        Ktx2Image image;
        std::string error;
        ASSERT_TRUE(Ktx2::read(source, 0, image, error)) << error;
        const TextureContent content = TextureFallback::contentOf(image.info);
        const Ktx2Codec codec = image.info.codec;

        for (GpuTextureFormat target : TextureFallback::transcodeCandidates(codec, content, true)){
            SCOPED_TRACE(gpuFormatName(target));
            std::vector<std::vector<uint8_t>> upstream;
            ASSERT_TRUE(transcodeBasisLevels(bytes, target, upstream, error)) << error;
            BasisTranscode::Session session;
            ASSERT_TRUE(session.open(image, error)) << error;
            ASSERT_EQ(session.levelCount(), upstream.size());
            EXPECT_EQ(session.transcodesInRows(), codec != Ktx2Codec::ETC1S);
            for (uint32_t level = 0; level < session.levelCount(); level++){
                std::vector<uint8_t> trimmed;
                ASSERT_TRUE(session.transcodeLevel(level, target, content, trimmed, error)) << error;
                EXPECT_EQ(trimmed, upstream[level]) << "level " << level;
                if (!session.transcodesInRows())
                    continue;
                // the main thread's bands (3 block rows: the last band of most levels is short)
                std::vector<uint8_t> banded(session.levelBytes(level, target), 0);
                for (uint32_t row = 0; row < session.blockRows(level); row += 3){
                    ASSERT_TRUE(session.transcodeRows(level, row, std::min(3u, session.blockRows(level) - row), target, content, banded, error)) << error;
                }
                EXPECT_EQ(banded, upstream[level]) << "level " << level << " in bands";
            }
            compared++;
        }
        // a session on the levels from 2 down, as a load that drops the top mips makes one (the
        // sprite has no mips)
        if (image.info.levelCount <= 2)
            continue;
        Ktx2Image dropped;
        ASSERT_TRUE(Ktx2::read(source, 2, dropped, error)) << error;
        const GpuTextureFormat first = TextureFallback::transcodeCandidates(codec, content, true).front();
        std::vector<std::vector<uint8_t>> upstream;
        ASSERT_TRUE(transcodeBasisLevels(bytes, first, upstream, error)) << error;
        BasisTranscode::Session session;
        ASSERT_TRUE(session.open(std::move(dropped), error)) << error;
        ASSERT_EQ(session.levelCount() + 2, upstream.size());
        std::vector<uint8_t> level0;
        ASSERT_TRUE(session.transcodeLevel(0, first, content, level0, error)) << error;
        EXPECT_EQ(level0, upstream[2]);
    }
    // ETC1S 4 + 4 + 3 + 3, UASTC 6 + 6 + 4 + 4, UASTC HDR 3 (the low-memory last resorts included)
    EXPECT_EQ(compared, 37);
}

// Every UASTC mode, not only the ones the corpus cooks pick: random blocks (those upstream's
// copy transcodes to each target) as a 256² level, transcoded by both copies, with and without
// alpha, as colour and as a normal map
TEST(TextureWebTranscode, TrimmedTranscoderMatchesUpstreamOnRandomUastcBlocks){
    using T = basist::transcoder_texture_format;
    basist::basisu_transcoder_init();   // the editor's copy (once; later calls return)
    struct Target { GpuTextureFormat format; T basis; };
    const Target targets[] = {
        {GpuTextureFormat::BC7, T::cTFBC7_RGBA}, {GpuTextureFormat::ASTC_4x4, T::cTFASTC_LDR_4x4_RGBA},
        {GpuTextureFormat::ETC2_RGB8, T::cTFETC1_RGB}, {GpuTextureFormat::ETC2_RGBA8, T::cTFETC2_RGBA},
        {GpuTextureFormat::BC1, T::cTFBC1_RGB}, {GpuTextureFormat::BC3, T::cTFBC3_RGBA},
        {GpuTextureFormat::BC4, T::cTFBC4_R}, {GpuTextureFormat::BC5, T::cTFBC5_RG},
        {GpuTextureFormat::EAC_R11, T::cTFETC2_EAC_R11}, {GpuTextureFormat::EAC_RG11, T::cTFETC2_EAC_RG11},
        {GpuTextureFormat::RGBA8, T::cTFRGBA32}, {GpuTextureFormat::RGB565, T::cTFRGB565}, {GpuTextureFormat::RGBA4, T::cTFRGBA4444},
    };
    constexpr uint32_t kBlocks = 64;
    basist::basisu_lowlevel_uastc_ldr_4x4_transcoder upstream;
    std::mt19937 random(2026);
    std::vector<uint8_t> blocks;
    std::vector<int> modes(20, 0);
    while (blocks.size() < (size_t)kBlocks * kBlocks * 16){
        uint8_t block[16];
        for (uint8_t& byte : block)
            byte = (uint8_t)random();
        // a mode with alpha carries an ETC2 EAC hint whose multiplier is 1 to 15 in every block an
        // encoder writes (both transcoders assert it: a Debug or sanitizer build stopped here);
        // random bits with a 0 are no UASTC block
        basist::unpacked_uastc_block hinted;
        if (!basist::unpack_uastc(*reinterpret_cast<const basist::uastc_block*>(block), hinted, false, true))
            continue;
        if (basist::g_uastc_mode_has_alpha[hinted.m_mode] && hinted.m_mode != basist::UASTC_MODE_INDEX_SOLID_COLOR &&
            (hinted.m_etc2_hints >> 4) == 0)
            continue;
        bool valid = true;
        for (const Target& t : targets){
            uint8_t out[64];
            const uint32_t units = basist::basis_transcoder_format_is_uncompressed(t.basis) ? 16 : 1;
            valid = valid && upstream.transcode_image(t.basis, out, units, block, 16, 1, 1, 4, 4, 0, 0, 16, basist::cDecodeFlagsHighQuality);
        }
        if (!valid)
            continue;
        basist::unpacked_uastc_block unpacked;
        if (basist::unpack_uastc(*reinterpret_cast<const basist::uastc_block*>(block), unpacked, false, false))
            modes[unpacked.m_mode]++;
        blocks.insert(blocks.end(), block, block + 16);
    }
    // every mode but the reserved ones shows up
    int seen = 0;
    for (int count : modes)
        seen += count > 0 ? 1 : 0;
    EXPECT_GE(seen, 19);

    for (uint32_t channel : {0u, 3u}){     // KTX2's UASTC RGB and RGBA
        for (TextureContent content : {TextureContent::ColorOpaque, TextureContent::Normal}){
            Ktx2Image image;
            image.info.codec = Ktx2Codec::UASTC;
            image.info.width = image.info.height = kBlocks * 4;
            image.info.levelCount = 1;
            image.info.levels.resize(1);
            image.info.dfdOffset = 0;
            image.metadata.assign(32, 0);
            image.metadata[31] = (uint8_t)channel;
            image.levels.push_back(blocks);
            BasisTranscode::Session session;
            std::string error;
            ASSERT_TRUE(session.open(image, error)) << error;
            for (const Target& t : targets){
                SCOPED_TRACE(std::string(gpuFormatName(t.format)) + (channel ? " RGBA" : " RGB") + (content == TextureContent::Normal ? " normal" : ""));
                std::vector<uint8_t> trimmed;
                ASSERT_TRUE(session.transcodeLevel(0, t.format, content, trimmed, error)) << error;
                const bool uncompressed = basist::basis_transcoder_format_is_uncompressed(t.basis);
                const uint32_t units = uncompressed ? kBlocks * kBlocks * 16 : kBlocks * kBlocks;
                std::vector<uint8_t> reference((size_t)units * basist::basis_get_bytes_per_block_or_pixel(t.basis));
                uint32_t flags = basist::cDecodeFlagsHighQuality;
                if (t.format == GpuTextureFormat::BC1)
                    flags |= basist::cDecodeFlagsBC1ForbidThreeColorBlocks;
                const int channel0 = content == TextureContent::Normal ? 0 : -1, channel1 = content == TextureContent::Normal ? 1 : -1;
                ASSERT_TRUE(upstream.transcode_image(t.basis, reference.data(), units, blocks.data(), (uint32_t)blocks.size(), kBlocks, kBlocks,
                                                     kBlocks * 4, kBlocks * 4, 0, 0, (uint32_t)blocks.size(), flags, channel == 3, false, 0,
                                                     nullptr, 0, channel0, channel1));
                EXPECT_EQ(trimmed, reference);
            }
        }
    }
}
