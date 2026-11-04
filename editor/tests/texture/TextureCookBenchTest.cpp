// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Spec S14 R17, R18, R20 at the spec's sizes: cold cooks of a 2048² photo (desktop BC1 <= 3 s, ASTC
// 6x6 at normal effort <= 6 s, UASTC RDO 1 <= 12 s, all threads), a warm (cache-hit) cook of 200
// textures <= 10 s, and the corpus's bits per texel and PSNR per family at Normal quality (the
// tables of docs/textures.md). Times are printed as [bench] lines; budgets are enforced with
// JASPER_TEST_PERF_SLACK (the benchmark server E: docs/testing.md).

#include <gtest/gtest.h>
#include "util/UiItemLocator.h"

#include "support/EditorTestSupport.h"
#include "support/TextureCorpus.h"

#include "texture/TextureCookQueue.h"
#include "texture/TextureCooker.h"

#include <chrono>
#include <cstdio>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::test;
namespace fs = std::filesystem;

namespace {

    double benchSeconds(std::chrono::steady_clock::time_point start){
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    TextureCookResult benchCook(TextureCookCache& cache, const fs::path& source, TextureImportType type, TextureFamily family,
                                const std::string& format = std::string(), float rdo = -1.0f){
        TextureCookRequest request;
        request.source = source;
        request.settings.type = type;
        request.settings.maxSize = 4096;
        if (!format.empty() || rdo >= 0.0f){
            TexturePlatformSettings& p = request.settings.platforms[(size_t)family];
            p.override = true;
            if (!format.empty()) p.format = format;
            if (rdo >= 0.0f) p.rdo = rdo;
        }
        request.family = family;
        return TextureCooker::cook(cache, request, nullptr);
    }

}

TEST(TextureCookBench, ColdCooksOf2048){
    ScratchDir scratch("cook-bench");
    // the spec's 2048² on the benchmark run; a quick 512² elsewhere (the budgets are not checked there)
    const double slack = perfSlack();
    const int size = slack > 0.0 ? 2048 : 512;
    const fs::path photo = writeCorpusImage(corpusImage("photo", size, size), scratch.path / "src");
    struct Case { const char* what; TextureFamily family; std::string format; float rdo; double budget; };
    const Case cases[] = {
        {"desktop BC1", TextureFamily::Desktop, "", -1.0f, 3.0},
        {"mobile ASTC 6x6", TextureFamily::Mobile, "", -1.0f, 6.0},
        {"web UASTC RDO 1", TextureFamily::Web, "UASTC", 1.0f, 12.0},
    };
    for (const Case& c : cases){
        // a cache of its own: every cook is cold (the source table too)
        TextureCookCache cache;
        cache.open(scratch.path / ("cache-" + std::string(textureFamilyName(c.family))), "", 1ull << 32);
        const auto start = std::chrono::steady_clock::now();
        const TextureCookResult result = benchCook(cache, photo, TextureImportType::Default, c.family, c.format, c.rdo);
        const double seconds = benchSeconds(start);
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_FALSE(result.cacheHit);
        std::printf("[bench] cook %d %-16s %6.2f s (budget %.0f s) %s, %.2f dB\n", size, c.what, seconds, c.budget,
                    result.record.label.c_str(), result.record.psnr);
        if (slack > 0.0){
            EXPECT_LE(seconds, c.budget * slack) << c.what;
        }
        cache.close();
    }
}

TEST(TextureCookBench, WarmCookOf200Textures){
    ScratchDir scratch("cook-bench-warm");
    TextureCookCache cache;
    cache.open(scratch.path / "cache", "", 1ull << 32);
    std::map<std::string, TextureCookInputs> inputs;
    std::vector<TextureBatchItem> items;
    const std::vector<std::string>& names = corpusNames();
    for (int i = 0; i < 200; i++){
        const std::string name = names[(size_t)i % names.size()];
        CorpusImage image = corpusImage(name, 128, 128);
        image.name = name + "_" + std::to_string(i);
        // every file its own content
        if (!image.rgba.empty()) image.rgba[0] = (uint8_t)i;
        if (!image.rgbaFloat.empty()) image.rgbaFloat[0] = (float)i;
        TextureCookInputs in;
        in.assetPath = image.name;
        in.absolute = writeCorpusImage(image, scratch.path / "src");
        in.settings.type = corpusImage(name, 4, 4).type;
        in.hasSettings = true;
        inputs[in.assetPath] = in;
        TextureBatchItem item;
        item.assetPath = in.assetPath;
        items.push_back(item);
    }
    TextureBatchCook::run(cache, TextureProjectSettings(), inputs, items, 0, 2ull << 30, nullptr, nullptr);
    for (const TextureBatchItem& item : items)
        ASSERT_TRUE(item.result.ok) << item.assetPath << ": " << item.result.error;
    // warm: what an export finds
    for (TextureBatchItem& item : items)
        item.result = TextureCookResult();
    const auto start = std::chrono::steady_clock::now();
    TextureBatchCook::run(cache, TextureProjectSettings(), inputs, items, 0, 2ull << 30, nullptr, nullptr);
    const double seconds = benchSeconds(start);
    int hits = 0;
    for (const TextureBatchItem& item : items)
        hits += item.result.ok && item.result.cacheHit ? 1 : 0;
    EXPECT_EQ(hits, 200);
    std::printf("[bench] warm cook of 200 textures %.2f s (budget 10 s)\n", seconds);
    if (perfSlack() > 0.0){
        EXPECT_LE(seconds, 10.0 * perfSlack());
    }
}

TEST(TextureCookBench, CorpusBitsPerTexelAndQuality){
    // the spec's corpus sizes (section 7) at Normal quality, per family
    ScratchDir scratch("cook-bench-corpus");
    TextureCookCache cache;
    cache.open(scratch.path / "cache", "", 1ull << 33);
    struct Item { const char* name; int width, height; };
    const Item corpus[] = {{"photo", 2048, 2048}, {"gradient", 1024, 1024}, {"ui", 1024, 512}, {"foliage", 1024, 1024},
                           {"normal", 2048, 2048}, {"mask", 1024, 1024}, {"albedo", 1024, 1024}, {"sky", 2048, 1024}};
    // the full sizes on the benchmark run, an eighth elsewhere
    const int divisor = perfSlack() > 0.0 ? 1 : 8;
    std::printf("[bench] | texture | family | format | VRAM bpt | disk bpt | quality |\n");
    for (const Item& item : corpus){
        const CorpusImage image = corpusImage(item.name, item.width / divisor, item.height / divisor);
        const fs::path source = writeCorpusImage(image, scratch.path / "src");
        for (TextureFamily family : {TextureFamily::Desktop, TextureFamily::Mobile, TextureFamily::AndroidEtc2, TextureFamily::Web}){
            const TextureCookResult result = benchCook(cache, source, image.type, family);
            ASSERT_TRUE(result.ok) << item.name << " " << textureFamilyName(family) << ": " << result.error;
            const double texels = (double)result.record.width * result.record.height;
            char quality[48];
            if (result.record.normalMeanError > 0.0)
                snprintf(quality, sizeof(quality), "%.2f deg mean", result.record.normalMeanError);
            else if (result.record.hdrRelativeRmse > 0.0)
                snprintf(quality, sizeof(quality), "%.4f rel. RMSE", result.record.hdrRelativeRmse);
            else
                snprintf(quality, sizeof(quality), "%.2f dB", result.record.psnr);
            std::printf("[bench] | %s %dx%d | %s | %s | %.2f | %.2f | %s |\n", item.name, image.width, image.height, textureFamilyName(family),
                        result.record.label.c_str(), result.record.gpuBytes * 8.0 / texels, result.record.diskBytes * 8.0 / texels, quality);
        }
    }
}
