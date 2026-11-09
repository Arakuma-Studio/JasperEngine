// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The cook end to end on the generated corpus (spec S14): the cache key (AC1), the raw path and
// the cook agreeing on uncompressed levels (AC2), quality floors (AC4), sizes (AC5, R17, R18),
// byte-identical output at any concurrency (AC6), the BC7 foliage repair, cubes and their CPU
// copy, metadata (R32) and cancellation.

#include <gtest/gtest.h>
#include "texture/TextureData.h"

#include "support/EditorTestSupport.h"
#include "support/TextureCorpus.h"

#include "texture/Ktx2.h"
#include "texture/TextureCookQueue.h"
#include "texture/TextureCooker.h"
#include "texture/TextureDecode.h"
#include "texture/TextureEncoders.h"
#include "texture/TexturePrepare.h"

#include <cmath>
#include <fstream>
#include <map>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
namespace fs = std::filesystem;

namespace {

    // the corpus edge the unit tests cook at (the benchmark cooks the spec's full sizes)
    constexpr int kCookTestSize = 512;

    struct CookBench {
        ScratchDir scratch;
        TextureCookCache cache;
        explicit CookBench(const std::string& name): scratch(name){
            cache.open(scratch.path / "cache", "", 1ull << 32);
        }
        ~CookBench(){ cache.close(); }

        fs::path source(const std::string& name, int size = kCookTestSize){
            const fs::path file = scratch.path / "src" / (name + (name == "sky" ? ".hdr" : ".png"));
            std::error_code ec;
            if (!fs::exists(file, ec))
                writeCorpusImage(corpusImage(name, size, size), scratch.path / "src");
            return file;
        }

        TextureCookResult cook(const fs::path& file, const TextureImportSettings& settings, TextureFamily family,
                               const std::function<bool()>& cancelled = nullptr){
            TextureCookRequest request;
            request.source = file;
            request.assetPath = file.filename().generic_string();
            request.settings = settings;
            request.family = family;
            return TextureCooker::cook(cache, request, cancelled);
        }

        std::vector<uint8_t> artifact(const std::string& key){
            std::vector<uint8_t> bytes;
            cache.readArtifact(key, bytes);
            return bytes;
        }
    };

    TextureImportSettings cookSettings(TextureImportType type, TextureCookQuality quality, const std::string& format = std::string(),
                                       TextureFamily family = TextureFamily::Desktop, float rdo = -1.0f){
        TextureImportSettings settings;
        settings.type = type;
        settings.quality = quality;
        if (!format.empty() || rdo >= 0.0f){
            TexturePlatformSettings& p = settings.platforms[(size_t)family];
            p.override = true;
            if (!format.empty()) p.format = format;
            if (rdo >= 0.0f) p.rdo = rdo;
        }
        return settings;
    }

    double cookPsnr(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t texels){
        double squared = 0.0;
        for (size_t i = 0; i < texels; i++)
            for (int c = 0; c < 3; c++){
                const double d = (double)a[i * 4 + c] - b[i * 4 + c];
                squared += d * d;
            }
        const double mse = squared / (double)(texels * 3);
        return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    }

    // the prepared mip 0 a cook compares against (RGBA8 layouts)
    PreparedTexture cookReference(const fs::path& file, const TextureImportSettings& settings, TextureFamily family){
        std::array<TextureData, 6> faces;
        std::string error;
        EXPECT_GT(TextureCooker::decodeSource(file, false, faces, error), 0) << error;
        PreparedTexture prepared;
        const TextureProcessing processing = settings.resolve(family, TextureProjectSettings()).processing();
        EXPECT_TRUE(TexturePrepare::prepare({&faces[0]}, processing, prepared, error)) << error;
        return prepared;
    }

    // the floors of AC4 (dB), calibrated to the measured value - 1 dB, never above the spec's
    struct Floor {
        const char* what;
        TextureFamily family;
        std::string format;     // the explicit format (empty: the table's at `quality`)
        TextureCookQuality quality;
        float rdo;
        double floor;
    };

    const std::vector<std::string>& colourCorpus(){
        static const std::vector<std::string> names = {"photo", "albedo", "gradient"};
        return names;
    }

}

// ---- AC1

TEST(TextureCookKey, StableForTheSameInputs){
    ResolvedTextureSettings settings;
    TextureFormatChoice choice;
    choice.format = GpuTextureFormat::BC7;
    choice.rdoLambda = 1.0f;
    choice.zstdLevel = 15;
    const std::string sha(64, 'a');
    const std::string key = TextureCookCache::key(sha, settings, TextureFamily::Desktop, choice);
    EXPECT_EQ(key.size(), 64u);
    EXPECT_EQ(key, TextureCookCache::key(sha, settings, TextureFamily::Desktop, choice));
    // the numbers of a key print without the locale: a decimal comma never reaches it
    std::locale previous = std::locale::global(std::locale::classic());
    EXPECT_EQ(textureCanonicalNumber(0.5f), "0.5");
    EXPECT_EQ(textureCanonicalNumber(-1.0f), "-1");
    EXPECT_EQ(textureCanonicalNumber(128.0f), "128");
    EXPECT_EQ(textureCanonicalNumber(0.123456f), "0.123456");
    std::locale::global(previous);
}

TEST(TextureCookKey, EveryInputChangesIt){
    ResolvedTextureSettings settings;
    TextureFormatChoice choice;
    choice.format = GpuTextureFormat::BC1;
    choice.zstdLevel = 15;
    const std::string sha(64, 'a');
    const std::string reference = TextureCookCache::key(sha, settings, TextureFamily::Desktop, choice);
    std::set<std::string> keys = {reference};
    auto differs = [&](const std::string& key, const char* what){
        EXPECT_TRUE(keys.insert(key).second) << what;
    };
    differs(TextureCookCache::key(std::string(63, 'a') + "b", settings, TextureFamily::Desktop, choice), "source");
    differs(TextureCookCache::key(sha, settings, TextureFamily::Mobile, choice), "family");
    TextureFormatChoice other = choice;
    other.format = GpuTextureFormat::BC7;
    differs(TextureCookCache::key(sha, settings, TextureFamily::Desktop, other), "format");
    other = choice;
    other.zstdLevel = 19;
    differs(TextureCookCache::key(sha, settings, TextureFamily::Desktop, other), "zstd");
    other = choice;
    other.sizeMultiple = 4;
    differs(TextureCookCache::key(sha, settings, TextureFamily::Desktop, other), "multiple of 4");
    other = choice;
    other.codec = TextureCodec::UASTC;
    differs(TextureCookCache::key(sha, settings, TextureFamily::Desktop, other), "codec (and its encoder)");
    other = choice;
    other.format = GpuTextureFormat::BC7;
    other.rdoLambda = 0.5f;
    differs(TextureCookCache::key(sha, settings, TextureFamily::Desktop, other), "rdo");
    ResolvedTextureSettings changed = settings;
    changed.maxSize = 1024;
    differs(TextureCookCache::key(sha, changed, TextureFamily::Desktop, choice), "maxSize");
    changed = settings;
    changed.cpuReadable = true;
    differs(TextureCookCache::key(sha, changed, TextureFamily::Desktop, choice), "cpu readable");
    changed = settings;
    changed.strategy = TextureSizeStrategy::Fit;
    differs(TextureCookCache::key(sha, changed, TextureFamily::Desktop, choice), "project strategy");
    // every settings field is in the canonical text (TextureImportSettings.CanonicalFormChangesWithEveryField)
}

TEST(TextureCookKey, FollowsTheContentNotTheName){
    CookBench bench("cook-key");
    const fs::path a = bench.source("albedo", 64);
    const fs::path b = a.parent_path() / "renamed.png";
    fs::copy_file(a, b);
    const TextureImportSettings settings = cookSettings(TextureImportType::Default, TextureCookQuality::Normal);
    const TextureCookResult first = bench.cook(a, settings, TextureFamily::Desktop);
    ASSERT_TRUE(first.ok) << first.error;
    EXPECT_FALSE(first.cacheHit);
    const TextureCookResult second = bench.cook(b, settings, TextureFamily::Desktop);
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_EQ(second.plan.key, first.plan.key);
    EXPECT_TRUE(second.cacheHit);

    // one byte of the source: another key
    std::fstream file(b, std::ios::in | std::ios::out | std::ios::binary);
    file.seekp(-20, std::ios::end);
    char byte = 0;
    file.seekg(-20, std::ios::end);
    file.read(&byte, 1);
    byte = (char)(byte ^ 0x01);
    file.seekp(-20, std::ios::end);
    file.write(&byte, 1);
    file.close();
    // a new mtime too: the source table hashes again
    fs::last_write_time(b, fs::last_write_time(b) + std::chrono::seconds(2));
    TextureCookPlan plan;
    std::string error;
    TextureCookRequest request;
    request.source = b;
    request.settings = settings;
    ASSERT_TRUE(TextureCooker::plan(bench.cache, request, plan, error)) << error;
    EXPECT_NE(plan.key, first.plan.key);
}

// ---- AC2 (the cook side): the raw path and an uncompressed cook give the same bytes

TEST(TextureCook, RawPathAndUncompressedCookAgree){
    CookBench bench("cook-raw");
    for (const std::string& name : {std::string("photo"), std::string("foliage"), std::string("normal"), std::string("mask")}){
        SCOPED_TRACE(name);
        const fs::path file = bench.source(name, 128);
        const CorpusImage image = corpusImage(name, 8, 8);
        TextureImportSettings settings = cookSettings(image.type, TextureCookQuality::None);
        if (name == "foliage") settings.preserveCoverage = true;
        const TextureCookResult result = bench.cook(file, settings, TextureFamily::Desktop);
        ASSERT_TRUE(result.ok) << result.error;
        const std::vector<uint8_t> bytes = bench.artifact(result.record.key);
        Ktx2MemorySource source(bytes);
        Ktx2Image cooked;
        std::string error;
        ASSERT_TRUE(Ktx2::read(source, 0, cooked, error)) << error;

        // the raw path: TexturePrepare with the import processing, then the uncompressed levels
        const PreparedTexture prepared = cookReference(file, settings, TextureFamily::Desktop);
        std::vector<std::vector<uint8_t>> levels;
        ASSERT_TRUE(TexturePrepare::toUncompressed(prepared, TexturePrepare::uncompressedFormat(prepared.layout), levels, error)) << error;
        ASSERT_EQ(cooked.levels.size(), levels.size());
        for (size_t level = 0; level < levels.size(); level++)
            EXPECT_EQ(cooked.levels[level], levels[level]) << "level " << level;
    }
}

// S42 AC3 (R4): a 256 x 128 sprite with pixelArt: true cooks to one RGBA8 level on every family
// (uncompressed, zstd: lossless), and each decodes to the source's bytes exactly
TEST(TextureCook, PixelArtLossless){
    CookBench bench("cook-pixel-art");
    CorpusImage image = corpusImage("photo", 256, 128);
    image.name = "pixel_sprite";
    for (size_t i = 3; i < image.rgba.size(); i += 4) image.rgba[i] = 255;   // opaque: Alpha Clip and bleeding keep every byte
    const fs::path file = writeCorpusImage(image, bench.scratch.path / "src");
    TextureImportSettings settings;
    settings.pixelArt = true;
    for (TextureFamily family : {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::AndroidEtc2, TextureFamily::Web}){
        SCOPED_TRACE(textureFamilyName(family));
        const TextureCookResult result = bench.cook(file, settings, family);
        ASSERT_TRUE(result.ok) << result.error;
        const std::vector<uint8_t> bytes = bench.artifact(result.record.key);
        Ktx2MemorySource source(bytes);
        Ktx2Image cooked;
        std::string error;
        ASSERT_TRUE(Ktx2::read(source, 0, cooked, error)) << error;
        EXPECT_EQ(cooked.info.format, GpuTextureFormat::RGBA8);
        ASSERT_EQ(cooked.levels.size(), 1u) << "no mipmaps";
        ASSERT_EQ(cooked.levels[0].size(), image.rgba.size());
        EXPECT_TRUE(cooked.levels[0] == image.rgba) << "the cooked level is the source, byte for byte";
    }
    // red control: the same texture without Pixel Art is block-compressed with a mip chain
    const TextureCookResult lossy = bench.cook(file, TextureImportSettings(), TextureFamily::Desktop);
    ASSERT_TRUE(lossy.ok) << lossy.error;
    const std::vector<uint8_t> lossyBytes = bench.artifact(lossy.record.key);
    Ktx2MemorySource lossySource(lossyBytes);
    Ktx2Image lossyImage;
    std::string error;
    ASSERT_TRUE(Ktx2::read(lossySource, 0, lossyImage, error)) << error;
    EXPECT_GT(lossyImage.levels.size(), 1u);
}

// ---- R32 metadata, cubes and the CPU copy, cancellation

TEST(TextureCook, CookedFilesCarryTheirMetadata){
    CookBench bench("cook-meta");
    const fs::path file = bench.source("ui", 100);
    TextureImportSettings settings = cookSettings(TextureImportType::UI, TextureCookQuality::Normal);
    settings.maxSize = 64;
    const TextureCookResult result = bench.cook(file, settings, TextureFamily::Mobile);
    ASSERT_TRUE(result.ok) << result.error;
    const std::vector<uint8_t> bytes = bench.artifact(result.record.key);
    Ktx2Info info;
    std::string error;
    ASSERT_TRUE(Ktx2::parseInfo(bytes, bytes.size(), info, error)) << error;
    EXPECT_EQ(info.keyValues.at("JE.logical"), "100,100");
    EXPECT_EQ(info.keyValues.at("JE.type"), "ui");
    EXPECT_NE(info.keyValues.at("JE.flags").find("alpha"), std::string::npos);
    EXPECT_EQ(info.format, GpuTextureFormat::ASTC_6x6);
    EXPECT_TRUE(info.srgb);
    EXPECT_EQ(info.levelCount, 1u);   // UI: no mips
    EXPECT_EQ(result.record.width, 64);
    EXPECT_EQ(result.record.logicalWidth, 100);
    EXPECT_EQ(result.record.flags & TEXTURE_FLAG_STREAMABLE, 0u);
    // the record survives the cache
    TextureCookRecord record;
    ASSERT_TRUE(bench.cache.load(result.record.key, record));
    EXPECT_EQ(record.format, "ASTC_6x6");
    EXPECT_EQ(record.diskBytes, bytes.size());
}

TEST(TextureCook, CubeCrossesCookSixFacesAndACpuCopy){
    CookBench bench("cook-cube");
    // a 4:3 horizontal cross of a 32 px face, each face its own colour
    const int face = 32;
    CorpusImage cross;
    cross.name = "cross";
    cross.width = face * 4;
    cross.height = face * 3;
    cross.rgba.assign((size_t)cross.width * cross.height * 4, 0);
    const int cells[6][2] = {{2, 1}, {0, 1}, {1, 0}, {1, 2}, {1, 1}, {3, 1}};   // +X -X +Y -Y +Z -Z
    for (int f = 0; f < 6; f++)
        for (int y = 0; y < face; y++)
            for (int x = 0; x < face; x++){
                uint8_t* p = &cross.rgba[(((size_t)cells[f][1] * face + y) * cross.width + cells[f][0] * face + x) * 4];
                p[0] = (uint8_t)(40 * f);
                p[1] = (uint8_t)(255 - 40 * f);
                p[2] = (uint8_t)(x * 8);
                p[3] = 255;
            }
    const fs::path file = writeCorpusImage(cross, bench.scratch.path / "src");
    TextureImportSettings settings = cookSettings(TextureImportType::Default, TextureCookQuality::Normal);
    settings.shape = TextureShape::Cube;
    const TextureCookResult result = bench.cook(file, settings, TextureFamily::Desktop);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.record.faces, 6);
    EXPECT_EQ(result.record.width, face);
    EXPECT_GT(result.record.cpuBytes, 0u);
    EXPECT_NE(result.record.flags & TEXTURE_FLAG_CUBE, 0u);
    std::vector<uint8_t> cpu;
    ASSERT_TRUE(bench.cache.readArtifact(result.record.key, cpu, ".cpu.ktx2"));
    Ktx2Info info;
    std::string error;
    ASSERT_TRUE(Ktx2::parseInfo(cpu, cpu.size(), info, error)) << error;
    EXPECT_EQ(info.format, GpuTextureFormat::RGBA8);
    EXPECT_EQ(info.faces, 6u);
}

TEST(TextureCook, CancelStopsTheEncode){
    CookBench bench("cook-cancel");
    const fs::path file = bench.source("photo", 256);
    const TextureCookResult result = bench.cook(file, cookSettings(TextureImportType::Default, TextureCookQuality::High),
                                                TextureFamily::Mobile, [](){ return true; });
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(result.cancelled);
    EXPECT_FALSE(bench.cache.has(result.plan.key));
}

// ---- AC4

TEST(TextureEncodeQuality, ColourFloors){
    CookBench bench("cook-quality");
    const std::vector<Floor> floors = {
        {"BC7 H", TextureFamily::Desktop, "BC7", TextureCookQuality::High, 0.0f, 42.0},
        {"BC7 N", TextureFamily::Desktop, "BC7", TextureCookQuality::Normal, 1.0f, 40.0},
        {"BC1", TextureFamily::Desktop, "BC1", TextureCookQuality::Normal, -1.0f, 34.0},
        {"ASTC 4x4", TextureFamily::Mobile, "ASTC_4x4", TextureCookQuality::Normal, -1.0f, 42.0},
        {"ASTC 5x5", TextureFamily::Mobile, "ASTC_5x5", TextureCookQuality::Normal, -1.0f, 39.0},
        {"ASTC 6x6", TextureFamily::Mobile, "ASTC_6x6", TextureCookQuality::Normal, -1.0f, 36.0},
        {"ASTC 8x8", TextureFamily::Mobile, "ASTC_8x8", TextureCookQuality::Normal, -1.0f, 32.0},
        {"ASTC 10x10", TextureFamily::Mobile, "ASTC_10x10", TextureCookQuality::Normal, -1.0f, 29.0},
        {"ETC2 RGB", TextureFamily::AndroidEtc2, "ETC2_RGB8", TextureCookQuality::Normal, -1.0f, 34.0},
        {"ETC1S q128", TextureFamily::Web, "ETC1S", TextureCookQuality::Normal, 128.0f, 31.0},
        {"UASTC λ1", TextureFamily::Web, "UASTC", TextureCookQuality::Normal, 1.0f, 39.0},
    };
    for (const Floor& f : floors){
        double sum = 0.0;
        for (const std::string& name : colourCorpus()){
            const TextureCookResult result = bench.cook(bench.source(name), cookSettings(TextureImportType::Default, f.quality, f.format, f.family, f.rdo), f.family);
            ASSERT_TRUE(result.ok) << f.what << " " << name << ": " << result.error;
            sum += result.record.psnr;
        }
        const double average = sum / (double)colourCorpus().size();
        std::printf("[ quality ] %-10s %6.2f dB (floor %.1f)\n", f.what, average, f.floor);
        EXPECT_GE(average, f.floor) << f.what;
    }
}

TEST(TextureEncodeQuality, UastcTranscodesKeepTheirQuality){
    CookBench bench("cook-uastc");
    double sumBc7 = 0.0, sumAstc = 0.0;
    for (const std::string& name : colourCorpus()){
        const fs::path file = bench.source(name);
        const TextureImportSettings settings = cookSettings(TextureImportType::Default, TextureCookQuality::High, "UASTC", TextureFamily::Web, 0.0f);
        const TextureCookResult result = bench.cook(file, settings, TextureFamily::Web);
        ASSERT_TRUE(result.ok) << result.error;
        const PreparedTexture reference = cookReference(file, settings, TextureFamily::Web);
        const std::vector<uint8_t> bytes = bench.artifact(result.record.key);
        for (GpuTextureFormat target : {GpuTextureFormat::BC7, GpuTextureFormat::ASTC_4x4}){
            std::vector<std::vector<uint8_t>> levels;
            int width = 0, height = 0;
            std::string error;
            ASSERT_TRUE(transcodeKtx2(bytes, target, levels, width, height, error)) << error;
            std::vector<uint8_t> rgba;
            ASSERT_TRUE(TextureDecode::decode(target, width, height, levels[0].data(), levels[0].size(), rgba));
            const double psnr = cookPsnr(rgba, reference.faces[0][0].pixels, (size_t)width * height);
            (target == GpuTextureFormat::BC7 ? sumBc7 : sumAstc) += psnr;
        }
    }
    const double count = (double)colourCorpus().size();
    std::printf("[ quality ] UASTC λ0 -> BC7 %.2f dB, -> ASTC 4x4 %.2f dB (floor 41)\n", sumBc7 / count, sumAstc / count);
    EXPECT_GE(sumBc7 / count, 41.0);
    EXPECT_GE(sumAstc / count, 41.0);
}

TEST(TextureEncodeQuality, SingleChannelFloor){
    CookBench bench("cook-bc4");
    const fs::path file = bench.source("mask");
    const TextureCookResult result = bench.cook(file, cookSettings(TextureImportType::SingleChannel, TextureCookQuality::Normal), TextureFamily::Desktop);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.record.format, "BC4");
    std::printf("[ quality ] BC4 %.2f dB (floor 42)\n", result.record.psnr);
    EXPECT_GE(result.record.psnr, 42.0);
}

TEST(TextureEncodeQuality, NormalMapsKeepTheirAngles){
    CookBench bench("cook-normals");
    const fs::path file = bench.source("normal");
    const struct { const char* what; TextureFamily family; std::string format; } cases[] = {
        {"BC5", TextureFamily::Desktop, ""}, {"EAC RG11", TextureFamily::AndroidEtc2, ""},
        {"ASTC 6x6", TextureFamily::Mobile, ""}, {"UASTC λ1", TextureFamily::Web, ""}};
    for (const auto& c : cases){
        const TextureCookResult result = bench.cook(file, cookSettings(TextureImportType::Normal, TextureCookQuality::Normal), c.family);
        ASSERT_TRUE(result.ok) << c.what << ": " << result.error;
        EXPECT_NE(result.record.flags & TEXTURE_FLAG_NORMAL_RG, 0u);
        std::printf("[ quality ] %-9s normals mean %.3f deg, p99 %.3f deg\n", c.what, result.record.normalMeanError, result.record.normalP99Error);
        EXPECT_GT(result.record.normalMeanError, 0.0) << c.what;
        EXPECT_LE(result.record.normalMeanError, 1.5) << c.what;
        EXPECT_LE(result.record.normalP99Error, 6.0) << c.what;
    }
}

TEST(TextureEncodeQuality, HdrStaysWithinThreePercent){
    CookBench bench("cook-hdr");
    const fs::path file = bench.source("sky", 256);
    const struct { const char* what; TextureFamily family; TextureCookQuality quality; } cases[] = {
        {"BC6H", TextureFamily::Desktop, TextureCookQuality::Normal}, {"RGB9E5", TextureFamily::Mobile, TextureCookQuality::Normal}};
    for (const auto& c : cases){
        const TextureCookResult result = bench.cook(file, cookSettings(TextureImportType::HDR, c.quality), c.family);
        ASSERT_TRUE(result.ok) << c.what << ": " << result.error;
        EXPECT_EQ(result.record.format, c.what);
        std::printf("[ quality ] %-6s relative RMSE %.4f\n", c.what, result.record.hdrRelativeRmse);
        EXPECT_GT(result.record.hdrRelativeRmse, 0.0);
        EXPECT_LE(result.record.hdrRelativeRmse, 0.03) << c.what;
    }
}

TEST(TextureEncodeQuality, FoliageTransparentTexelsStayTransparentAfterBc7){
    CookBench bench("cook-foliage");
    const fs::path file = bench.source("foliage");
    for (TextureCookQuality quality : {TextureCookQuality::Low, TextureCookQuality::Normal, TextureCookQuality::High}){
        const TextureImportSettings settings = cookSettings(TextureImportType::Default, quality);
        const TextureCookResult result = bench.cook(file, settings, TextureFamily::Desktop);
        ASSERT_TRUE(result.ok) << result.error;
        ASSERT_EQ(result.record.format, "BC7");
        const std::vector<uint8_t> bytes = bench.artifact(result.record.key);
        GpuTextureFormat format;
        int width = 0, height = 0, faces = 1;
        std::vector<uint8_t> decoded;
        std::string error;
        ASSERT_TRUE(decodeKtx2ForPreview(bytes, 0, format, width, height, faces, decoded, error)) << error;
        const PreparedTexture reference = cookReference(file, settings, TextureFamily::Desktop);
        size_t leaks = 0, transparent = 0;
        for (size_t i = 0; i < (size_t)width * height; i++){
            if (reference.faces[0][0].pixels[i * 4 + 3] == 0){
                transparent++;
                leaks += decoded[i * 4 + 3] != 0 ? 1 : 0;
            }
        }
        EXPECT_GT(transparent, 0u);
        EXPECT_EQ(leaks, 0u) << textureQualityName(quality);
        EXPECT_GE(result.record.psnrAlpha, 40.0) << textureQualityName(quality);
    }
}

// The colour under fully transparent texels is kept by every family's alpha format: bilinear
// sampling at a cutout's edge blends it in (non-premultiplied alpha), so an encoder that treats it
// as free (astcenc's alpha weighting) draws fringes, and the report's RGB PSNR drops to ~13 dB.
TEST(TextureEncodeQuality, ColourUnderTransparentTexelsIsKept){
    CookBench bench("cook-under-alpha");
    const fs::path file = bench.source("foliage");
    for (TextureFamily family : {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::AndroidEtc2, TextureFamily::Web}){
        SCOPED_TRACE(textureFamilyName(family));
        const TextureImportSettings settings = cookSettings(TextureImportType::Default, TextureCookQuality::Normal);
        const TextureCookResult result = bench.cook(file, settings, family);
        ASSERT_TRUE(result.ok) << result.error;
        const std::vector<uint8_t> bytes = bench.artifact(result.record.key);
        GpuTextureFormat format;
        int width = 0, height = 0, faces = 1;
        std::vector<uint8_t> decoded;
        std::string error;
        ASSERT_TRUE(decodeKtx2ForPreview(bytes, 0, format, width, height, faces, decoded, error)) << error;
        ASSERT_EQ(format, GpuTextureFormat::RGBA8);
        const PreparedTexture reference = cookReference(file, settings, family);
        ASSERT_EQ(reference.faces[0][0].width, width);
        double squared = 0.0;
        size_t samples = 0;
        for (size_t i = 0; i < (size_t)width * height; i++){
            const uint8_t* source = &reference.faces[0][0].pixels[i * 4];
            if (source[3] != 0)
                continue;
            for (int c = 0; c < 3; c++){
                const double d = (double)decoded[i * 4 + c] - source[c];
                squared += d * d;
            }
            samples += 3;
        }
        ASSERT_GT(samples, 0u);
        const double mse = squared / (double)samples;
        const double psnr = mse <= 0.0 ? 100.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
        std::printf("[ quality ] %-12s %-10s RGB under alpha 0: %.2f dB, whole texture %.2f dB\n", textureFamilyName(family),
                    result.record.format.c_str(), psnr, result.record.psnr);
        EXPECT_GE(psnr, 30.0) << result.record.format;
        EXPECT_GE(result.record.psnr, 34.0) << result.record.format;
    }
}

// ---- AC5: R18 (disk) and R17 (VRAM), bits per mip-0 texel with the mips

TEST(TextureEncodeSize, CorpusAveragesMeetTheBudgets){
    CookBench bench("cook-size");
    const double texels = (double)kCookTestSize * kCookTestSize;
    struct Budget { const char* what; TextureFamily family; TextureImportType type; std::string format; float rdo; double disk; double vram; };
    const std::vector<Budget> budgets = {
        {"desktop BC1", TextureFamily::Desktop, TextureImportType::Default, "BC1", -1.0f, 4.8, 5.34},
        {"desktop BC7 RDO λ1 + zstd", TextureFamily::Desktop, TextureImportType::Default, "BC7", 1.0f, 7.5, 10.67},
        {"mobile 6x6 + zstd", TextureFamily::Mobile, TextureImportType::Default, "ASTC_6x6", -1.0f, 4.6, 4.74},
        {"web ETC1S q128", TextureFamily::Web, TextureImportType::Default, "ETC1S", 128.0f, 2.0, 10.67},
        {"web UASTC λ1 + zstd", TextureFamily::Web, TextureImportType::Default, "UASTC", 1.0f, 6.0, 10.67},
    };
    for (const Budget& b : budgets){
        double disk = 0.0, vram = 0.0;
        for (const std::string& name : colourCorpus()){
            const TextureCookResult result = bench.cook(bench.source(name), cookSettings(b.type, TextureCookQuality::Normal, b.format, b.family, b.rdo), b.family);
            ASSERT_TRUE(result.ok) << b.what << ": " << result.error;
            disk += result.record.diskBytes * 8.0 / texels;
            vram += result.record.gpuBytes * 8.0 / texels;
        }
        disk /= (double)colourCorpus().size();
        vram /= (double)colourCorpus().size();
        std::printf("[ size ] %-26s disk %.2f bpt (<= %.2f), VRAM %.2f bpt (<= %.2f)\n", b.what, disk, b.disk, vram, b.vram);
        EXPECT_LE(disk, b.disk) << b.what;
        // R17's figures are the block rates; 512 px is no multiple of 6, so the 6x6 levels round
        // up a column and a row of blocks (2048 px: within 0.5%, TextureCookBench)
        EXPECT_LE(vram, b.vram * 1.025) << b.what;
    }
    // normal maps on the web: UASTC λ1
    const TextureCookResult normal = bench.cook(bench.source("normal"), cookSettings(TextureImportType::Normal, TextureCookQuality::Normal), TextureFamily::Web);
    ASSERT_TRUE(normal.ok) << normal.error;
    const double normalDisk = normal.record.diskBytes * 8.0 / texels;
    std::printf("[ size ] web normal UASTC λ1       disk %.2f bpt (<= 6.00)\n", normalDisk);
    EXPECT_LE(normalDisk, 6.0);
    // R17's mobile mask: 8x8 with mips, 2.67 bits per texel
    const TextureCookResult mask = bench.cook(bench.source("mask"), cookSettings(TextureImportType::SingleChannel, TextureCookQuality::Normal), TextureFamily::Mobile);
    ASSERT_TRUE(mask.ok) << mask.error;
    EXPECT_EQ(mask.record.format, "ASTC_8x8");
    EXPECT_LE(mask.record.gpuBytes * 8.0 / texels, 2.68);
}

// ---- AC6

TEST(TextureCookDeterminism, SameBytesAtAnyConcurrencyAndTwice){
    std::map<std::string, std::vector<uint8_t>> first;
    for (int run = 0; run < 3; run++){
        CookBench bench("cook-determinism-" + std::to_string(run));
        std::map<std::string, TextureCookInputs> inputs;
        std::vector<TextureBatchItem> items;
        for (const std::string& name : corpusNames()){
            const CorpusImage image = corpusImage(name, 8, 8);
            TextureCookInputs in;
            in.assetPath = name;
            in.absolute = bench.source(name, 256);
            in.settings.type = image.type;
            in.hasSettings = true;
            inputs[name] = in;
            for (TextureFamily family : {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::AndroidEtc2, TextureFamily::Web}){
                TextureBatchItem item;
                item.assetPath = name;
                item.family = family;
                items.push_back(item);
            }
        }
        // one texture at a time, then eight at once (and again)
        TextureBatchCook::run(bench.cache, TextureProjectSettings(), inputs, items, run == 0 ? 1 : 8, 2ull << 30, nullptr, nullptr);
        for (const TextureBatchItem& item : items){
            const std::string id = item.assetPath + "/" + textureFamilyName(item.family);
            ASSERT_TRUE(item.result.ok) << id << ": " << item.result.error;
            const std::vector<uint8_t> bytes = bench.artifact(item.result.record.key);
            ASSERT_FALSE(bytes.empty()) << id;
            if (run == 0)
                first[id] = bytes;
            else
                EXPECT_TRUE(first[id] == bytes) << id << " differs on run " << run;
        }
    }
}

// a file name that is not UTF-8 is never cooked (it could not be a key of the cache's tables or
// of an export's index.json): it loads from its source; check --textures aborted on one
TEST(TextureCook, NamesThatAreNotUtf8AreNotCooked){
    EXPECT_TRUE(isCookableTexture("textures/photo.png"));
    EXPECT_TRUE(isCookableTexture("textures/caf\xc3\xa9.png"));
    EXPECT_FALSE(isCookableTexture("textures/stal\xe9.png"));
    EXPECT_FALSE(isCookableTexture("textures/notes.txt"));
}
