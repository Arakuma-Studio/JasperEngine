// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S51 AC24 (R19, R20): load, rebuild and cache budgets, printed as [bench] lines and enforced with
// perfSlack() (JASPER_TEST_PERF_SLACK, the benchmark run; docs/testing.md), and the atlas memory,
// always checked.

#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapText.h"
#include "pixelart/BitmapTextCache.h"
#include "buffer/InterleavedBuffer.h"
#include "support/AllocationCounter.h"
#include "TestPaths.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    double bfbSlack(){
#if defined(NDEBUG)
        return testsupport::perfSlack();
#else
        return 4.0 * testsupport::perfSlack();
#endif
    }

    // the best of `runs` runs, ms
    template<typename F>
    double bfbBest(int runs, F&& f){
        double best = 1e9;
        for (int i = 0; i < runs; i++){
            const auto start = std::chrono::steady_clock::now();
            f();
            best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
        return best;
    }

    void bfbBudget(const char* name, double ms, double budget){
        const double slack = bfbSlack();
        std::printf("[bench] %s: %.4f ms (budget %.3f ms)\n", name, ms, budget * (slack > 0.0 ? slack : 1.0));
        if (slack > 0.0)
            EXPECT_LE(ms, budget * slack) << name;
    }

    std::string bfbFixture(const std::string& name){
        return test::testPaths().assetDir + "/fonts/" + name;
    }

    const char* bfbGrid = "{\"format\": \"jasperengine.bitmapfont\", \"source\": {\"type\": \"grid\", \"image\": \"grid@6x9.png\"}}";

}

TEST(BitmapFontBench, LoadBudgets){
    // R19: a 95-glyph grid font (parse, measure, pack) <= 2 ms
    const double grid = bfbBest(5, [](){ EXPECT_TRUE(BitmapFont::loadSettings(bfbFixture("bench.bitmapfont"), bfbGrid, nullptr)); });
    bfbBudget("grid font load (95 glyphs)", grid, 2.0);

    // a 256-glyph TrueType pixel font <= 15 ms (the fixture: ASCII, Latin-1, U+0100-U+0141)
    const std::string ttf = "{\"format\": \"jasperengine.bitmapfont\", \"source\": {\"type\": \"truetype\", \"file\": \"pixel8.ttf\", \"chars\": \"latin_ext\"}}";
    const double raster = bfbBest(5, [&ttf](){ EXPECT_TRUE(BitmapFont::loadSettings(bfbFixture("bench.bitmapfont"), ttf, nullptr)); });
    bfbBudget("TrueType pixel font load", raster, 15.0);

    // a cooked .jfont <= 1 ms
    std::shared_ptr<BitmapFont> font = BitmapFont::loadSettings(bfbFixture("bench.bitmapfont"), bfbGrid, nullptr);
    ASSERT_TRUE(font);
    const std::vector<uint8_t> cooked = font->toCooked();
    const double jfont = bfbBest(10, [&cooked](){ EXPECT_TRUE(BitmapFont::loadCooked(cooked, nullptr)); });
    bfbBudget(".jfont load", jfont, 1.0);
}

TEST(BitmapFontBench, AtlasMemory){
    // R20: a 95-glyph 6x9 monochrome atlas <= 64 KiB
    std::shared_ptr<BitmapFont> font = BitmapFont::loadSettings(bfbFixture("bench.bitmapfont"), bfbGrid, nullptr);
    ASSERT_TRUE(font);
    size_t bytes = 0;
    for (const BitmapFontPage& page : font->getPages()) bytes += page.pixels.size();
    std::printf("[bench] 95-glyph 6x9 atlas: %zu bytes (%zu pages)\n", bytes, font->getPages().size());
    EXPECT_LE(bytes, (size_t)64 * 1024);
    EXPECT_EQ(font->getPages()[0].channels, 1);
    std::shared_ptr<BitmapFont> builtin = BitmapFont::builtinPixel();
    size_t builtinBytes = 0;
    for (const BitmapFontPage& page : builtin->getPages()) builtinBytes += page.pixels.size();
    EXPECT_LE(builtinBytes, (size_t)64 * 1024);
}

TEST(BitmapFontBench, RebuildAndCacheBudgets){
    std::shared_ptr<BitmapFont> font = BitmapFont::loadSettings(bfbFixture("bench.bitmapfont"), bfbGrid, nullptr);
    ASSERT_TRUE(font);
    BitmapText text(font, "bench", 0);
    std::string line;
    for (int i = 0; i < 200; i++) line += (char)(33 + (i * 7) % 94);
    auto rebuild = [&](const BitmapTextOptions& options){
        InterleavedBuffer buffer;
        buffer.addAttribute(AttributeType::POSITION, 3);
        buffer.addAttribute(AttributeType::TEXCOORD1, 2);
        buffer.addAttribute(AttributeType::COLOR, 4);
        std::vector<uint16_t> indices;
        std::vector<Vector2> positions;
        std::vector<CharExtent> extents;
        std::vector<TextGlyphInfo> info;
        unsigned int w = 0, h = 0;
        text.createText(line, &buffer, indices, positions, extents, w, h, options, &info);
    };
    // R20: a 200-character Text rebuilds its mesh in <= 0.05 ms
    const double plain = bfbBest(20, [&](){ rebuild(BitmapTextOptions()); });
    bfbBudget("200-character rebuild", plain, 0.05);

    // a styled rebuild with a cache hit costs the same
    BitmapTextCache::get().clear();
    BitmapTextOptions styled;
    styled.style.outlineWidth = 1;
    styled.style.outlineColor = 0xFF000000u;
    rebuild(styled);
    const double hit = bfbBest(20, [&](){ rebuild(styled); });
    bfbBudget("200-character styled rebuild (cache hit)", hit, 0.05);

    // a cache miss rasterises <= 0.01 ms per glyph
    std::vector<uint32_t> glyphs;
    for (uint32_t c = 33; c < 127; c++) glyphs.push_back(c);
    double miss = 1e9;
    for (int run = 0; run < 5; run++){
        BitmapTextCache::get().clear();
        const auto start = std::chrono::steady_clock::now();
        EXPECT_GE(BitmapTextCache::get().acquire(*font, styled.style, glyphs), 0);
        miss = std::min(miss, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    bfbBudget("styled glyph miss (per glyph)", miss / (double)glyphs.size(), 0.01);
    BitmapTextCache::get().clear();
}
