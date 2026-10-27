// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The cooked fixtures of the GPU format test (spec S14 AC8, engine/tests/core/render/
// TextureFormatsGpuTest.cpp): a 32 px corpus image cooked to every GpuTextureFormat, with the
// CPU decode of each level next to it (the engine's test binary has no ASTC / ETC2 decoder: the
// editor's are the reference). JASPER_TEST_UPDATE_FIXTURES=1 writes them; otherwise this checks
// that the committed files are complete and that their decodes still match the editor's
// decoders (the encoders' bytes may differ on another ISA: D11).

#include <gtest/gtest.h>

#include "support/EditorTestSupport.h"
#include "support/TextureCorpus.h"

#include "texture/Ktx2.h"
#include "texture/TextureCookCache.h"
#include "texture/TextureCooker.h"
#include "texture/TextureDecode.h"
#include "texture/TextureEncoders.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
namespace fs = std::filesystem;

namespace {

    struct FixtureFormat {
        GpuTextureFormat format;
        TextureFamily family;           // the family whose override names it
        TextureImportType type;
        const char* source;             // corpus image
    };

    const std::vector<FixtureFormat>& fixtureFormats(){
        using F = GpuTextureFormat;
        const auto D = TextureFamily::Desktop, M = TextureFamily::Mobile, E = TextureFamily::AndroidEtc2;
        const auto C = TextureImportType::Default, N = TextureImportType::Normal, S = TextureImportType::SingleChannel,
                   H = TextureImportType::HDR;
        static const std::vector<FixtureFormat> formats = {
            {F::R8, D, S, "mask"}, {F::RG8, D, N, "normal"}, {F::RGBA8, D, C, "foliage"}, {F::R16, D, S, "mask"},
            {F::RGB565, D, C, "photo"}, {F::RGBA4, D, C, "foliage"}, {F::RGB9E5, M, H, "sky"}, {F::RGBA16F, M, H, "sky"},
            {F::BC1, D, C, "photo"}, {F::BC3, D, C, "foliage"}, {F::BC4, D, S, "mask"}, {F::BC5, D, N, "normal"},
            {F::BC6H, D, H, "sky"}, {F::BC7, D, C, "foliage"},
            {F::ETC2_RGB8, E, C, "photo"}, {F::ETC2_RGBA8, E, C, "foliage"}, {F::EAC_R11, E, S, "mask"}, {F::EAC_RG11, E, N, "normal"},
            {F::ASTC_4x4, M, C, "foliage"}, {F::ASTC_5x5, M, C, "photo"}, {F::ASTC_6x6, M, C, "photo"}, {F::ASTC_8x8, M, C, "photo"},
            {F::ASTC_10x10, M, C, "photo"}, {F::ASTC_12x12, M, C, "photo"},
        };
        return formats;
    }

    fs::path fixtureDir(){
        return fs::path(JASPER_TEST_REPO_DIR) / "engine" / "tests" / "core" / "fixtures" / "texture_formats";
    }

    std::vector<uint8_t> fixtureRead(const fs::path& path){
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }

    // every level's decode (TextureDecode::decodedFormat of the stored format), concatenated
    bool fixtureDecode(const std::vector<uint8_t>& ktx2, std::vector<uint8_t>& out, std::string& error){
        registerEditorTextureCodecs();
        Ktx2Info info;
        if (!Ktx2::parseInfo(ktx2, ktx2.size(), info, error))
            return false;
        Ktx2MemorySource source(ktx2);
        Ktx2Image image;
        if (!Ktx2::read(source, 0, image, error))
            return false;
        out.clear();
        for (uint32_t level = 0; level < info.levelCount; level++){
            std::vector<uint8_t> pixels;
            if (!TextureDecode::decodeFaces(info.format, (int)info.levelWidth(level), (int)info.levelHeight(level), (int)info.faces,
                                            image.levels[level].data(), image.levels[level].size(), pixels)){
                error = std::string(gpuFormatName(info.format)) + " level " + std::to_string(level) + " does not decode";
                return false;
            }
            out.insert(out.end(), pixels.begin(), pixels.end());
        }
        return true;
    }

}

TEST(TextureFormatFixtures, EveryFormatHasACookAndItsDecode){
    const bool update = std::getenv("JASPER_TEST_UPDATE_FIXTURES") && std::string(std::getenv("JASPER_TEST_UPDATE_FIXTURES")) == "1";
    ScratchDir scratch("format-fixtures");
    TextureCookCache cache;
    cache.open(scratch.path / "cache", "", 1ull << 30);
    std::error_code ec;
    if (update)
        fs::create_directories(fixtureDir(), ec);
    for (const FixtureFormat& f : fixtureFormats()){
        const std::string name = gpuFormatName(f.format);
        SCOPED_TRACE(name);
        const fs::path ktx2Path = fixtureDir() / (name + ".ktx2");
        const fs::path decodedPath = fixtureDir() / (name + ".decoded");
        if (update){
            std::vector<uint8_t> bytes;
            if (f.format == GpuTextureFormat::R16){
                // a 16-bit height map: a corpus mask widened, its mips box filtered
                std::vector<std::vector<uint8_t>> levels;
                std::vector<uint16_t> current(32 * 32);
                const CorpusImage image = corpusImage("mask", 32, 32);
                for (size_t i = 0; i < current.size(); i++)
                    current[i] = (uint16_t)(image.rgba[i * 4] * 257 + (i % 7) * 13);
                int size = 32;
                while (true){
                    levels.emplace_back(current.size() * 2);
                    std::memcpy(levels.back().data(), current.data(), levels.back().size());
                    if (size == 1)
                        break;
                    const int next = size / 2;
                    std::vector<uint16_t> smaller((size_t)next * next);
                    for (int y = 0; y < next; y++)
                        for (int x = 0; x < next; x++)
                            smaller[(size_t)y * next + x] = (uint16_t)(((uint32_t)current[(size_t)(2 * y) * size + 2 * x] + current[(size_t)(2 * y) * size + 2 * x + 1] +
                                                                        current[(size_t)(2 * y + 1) * size + 2 * x] + current[(size_t)(2 * y + 1) * size + 2 * x + 1] + 2) / 4);
                    current.swap(smaller);
                    size = next;
                }
                Ktx2::WriteDesc desc;
                desc.format = GpuTextureFormat::R16;
                desc.alpha = false;
                desc.width = desc.height = 32;
                for (const std::vector<uint8_t>& level : levels)
                    desc.levels.push_back(std::span<const uint8_t>(level));
                std::string error;
                ASSERT_TRUE(Ktx2::write(desc, bytes, error)) << error;
            }else{
                const CorpusImage image = corpusImage(f.source, 32, 32);
                const fs::path source = writeCorpusImage(image, scratch.path / "src");
                TextureCookRequest request;
                request.source = source;
                request.settings.type = f.type;
                TexturePlatformSettings& p = request.settings.platforms[(size_t)f.family];
                p.override = true;
                p.format = name;
                request.family = f.family;
                const TextureCookResult result = TextureCooker::cook(cache, request, nullptr);
                ASSERT_TRUE(result.ok) << result.error;
                ASSERT_TRUE(cache.readArtifact(result.record.key, bytes));
            }
            std::vector<uint8_t> decoded;
            std::string error;
            ASSERT_TRUE(fixtureDecode(bytes, decoded, error)) << error;
            std::ofstream(ktx2Path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
            std::ofstream(decodedPath, std::ios::binary).write(reinterpret_cast<const char*>(decoded.data()), (std::streamsize)decoded.size());
            continue;
        }
        ASSERT_TRUE(fs::exists(ktx2Path)) << ktx2Path << " is missing: JASPER_TEST_UPDATE_FIXTURES=1 writes it";
        const std::vector<uint8_t> bytes = fixtureRead(ktx2Path);
        Ktx2Info info;
        std::string error;
        ASSERT_TRUE(Ktx2::parseInfo(bytes, bytes.size(), info, error)) << error;
        EXPECT_EQ(info.format, f.format);
        EXPECT_GT(info.levelCount, 1u);
        std::vector<uint8_t> decoded;
        ASSERT_TRUE(fixtureDecode(bytes, decoded, error)) << error;
        EXPECT_TRUE(decoded == fixtureRead(decodedPath)) << "the editor's decode of " << name << " changed";
    }
}
