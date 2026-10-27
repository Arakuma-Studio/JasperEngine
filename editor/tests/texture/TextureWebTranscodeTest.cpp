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

#include "support/TextureCorpus.h"

#include "texture/BasisTranscode.h"
#include "texture/Ktx2.h"
#include "texture/TextureCooker.h"
#include "texture/TextureEncoderParts.h"
#include "texture/TextureFallback.h"

#include <algorithm>
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
