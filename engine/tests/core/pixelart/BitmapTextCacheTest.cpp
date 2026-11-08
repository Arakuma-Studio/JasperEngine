// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// S51 AC10 (the styled glyphs' pixels) and AC11 (recycling within a budget): the styled glyph
// cache without a scene. A Text's colour change keeping the cache is
// editor/tests/pixelart/BitmapTextComponentTest.cpp (BitmapTextCache.ColourChangeNoRebuild).

#include "pixelart/BitmapFont.h"
#include "pixelart/BitmapText.h"
#include "pixelart/BitmapTextCache.h"
#include "buffer/InterleavedBuffer.h"
#include "LogCapture.h"
#include "TestPaths.h"

#include <gtest/gtest.h>

#include <map>

using namespace jasperengine;
using namespace jasperengine::pixelart;

namespace {

    std::shared_ptr<BitmapFont> btcFont(){
        const std::string json = "{\"format\": \"jasperengine.bitmapfont\", \"source\": {\"type\": \"grid\", \"image\": \"grid@6x9.png\"}}";
        return BitmapFont::loadSettings(test::testPaths().assetDir + "/fonts/cache.bitmapfont", json, nullptr);
    }

    // the page's RGBA texel
    const uint8_t* btcTexel(int page, int x, int y){
        TextureData* data = BitmapTextCache::get().getTexture(page);
        const int width = BitmapTextCache::get().getPageWidth(page);
        return static_cast<const uint8_t*>(data->getData()) + ((size_t)y * (size_t)width + (size_t)x) * 4;
    }

    struct BtcCacheReset {
        BtcCacheReset(){ BitmapTextCache::get().clear(); BitmapTextCache::get().setFrameOverride(1); }
        ~BtcCacheReset(){ BitmapTextCache::get().clear(); BitmapTextCache::get().setFrameOverride(0); BitmapTextCache::get().setBudgetBytes(0); }
    };

}

TEST(BitmapTextCache, OutlineAndShadowArePixels){
    BtcCacheReset reset;
    std::shared_ptr<BitmapFont> font = btcFont();
    ASSERT_TRUE(font);
    BitmapTextCache& cache = BitmapTextCache::get();
    // 'A' (i 33): ink columns 1..2 of the cell, rows 1..6 (a 2 x 6 block)
    for (int neighbors : {4, 8}){
        BitmapTextStyle style;
        style.outlineWidth = 1;
        style.outlineNeighbors = neighbors;
        style.outlineColor = 0xFF0000FFu;   // red, opaque
        const int page = cache.acquire(*font, style, {'A'});
        ASSERT_GE(page, 0);
        const CachedGlyph* g = cache.find(page, *font, style, 'A');
        ASSERT_TRUE(g);
        EXPECT_EQ(g->faceW, 2);
        EXPECT_EQ(g->faceH, 6);
        EXPECT_EQ(g->backW, 4);
        EXPECT_EQ(g->backH, 8);
        EXPECT_EQ(g->backOffsetX, -1);
        EXPECT_EQ(g->backOffsetY, -1);
        // the corner texel of the back: on with 8 neighbours, off with 4
        const uint8_t* corner = btcTexel(page, g->backX, g->backY);
        EXPECT_EQ(corner[3], neighbors == 8 ? 255 : 0) << neighbors;
        const uint8_t* edge = btcTexel(page, g->backX + 1, g->backY);
        EXPECT_EQ(edge[0], 255);
        EXPECT_EQ(edge[3], 255);
        // the face: white with its alpha
        const uint8_t* face = btcTexel(page, g->faceX, g->faceY);
        EXPECT_EQ(face[0], 255);
        EXPECT_EQ(face[3], 255);
    }
    // a shadow (-2, 0) without an outline: the face's silhouette two pixels left
    BitmapTextStyle shadow;
    shadow.shadow = true;
    shadow.shadowX = -2;
    shadow.shadowColor = 0x80000000u;   // black at alpha 128
    const int page = cache.acquire(*font, shadow, {'A'});
    const CachedGlyph* g = cache.find(page, *font, shadow, 'A');
    ASSERT_TRUE(g);
    EXPECT_EQ(g->backW, 4);
    EXPECT_EQ(g->backH, 6);
    EXPECT_EQ(g->backOffsetX, -2);
    EXPECT_EQ(g->backOffsetY, 0);
    EXPECT_EQ(btcTexel(page, g->backX, g->backY)[3], 128);
    EXPECT_EQ(btcTexel(page, g->backX + 3, g->backY)[3], 0);
    // outline 2 grows two pixels
    BitmapTextStyle wide;
    wide.outlineWidth = 2;
    wide.outlineColor = 0xFF000000u;
    const CachedGlyph* w = cache.find(cache.acquire(*font, wide, {'A'}), *font, wide, 'A');
    ASSERT_TRUE(w);
    EXPECT_EQ(w->backW, 6);
    EXPECT_EQ(w->backH, 10);
    // the same style again is a hit
    const uint64_t misses = cache.getStats().misses;
    cache.acquire(*font, wide, {'A'});
    EXPECT_EQ(cache.getStats().misses, misses);
}

TEST(BitmapTextCache, Recycle){
    BtcCacheReset reset;
    std::shared_ptr<BitmapFont> font = btcFont();
    ASSERT_TRUE(font);
    BitmapTextCache& cache = BitmapTextCache::get();
    cache.setBudgetBytes(256 * 1024);
    test::LogCapture capture;

    std::vector<uint32_t> ascii;
    for (uint32_t c = 33; c < 127; c++) ascii.push_back(c);
    std::vector<BitmapTextStyle> styles(12);
    for (int s = 0; s < 12; s++){
        styles[(size_t)s].outlineWidth = 1;
        styles[(size_t)s].outlineColor = 0xFF000000u | (uint32_t)(s * 20);
    }

    // a Text per style: its page and the page's generation when it was built
    struct Built { int page = -1; uint32_t generation = 0; };
    std::map<int, Built> texts;
    std::map<int, uint64_t> lastDrawn;          // page -> frame a Text drew from it
    std::map<int, uint32_t> generations;
    for (uint64_t frame = 2; frame < 200; frame++){
        cache.setFrameOverride(frame);
        const std::map<int, uint64_t> drawnBefore = lastDrawn;
        // two styles live at a time, each for three frames: all twelve cycle
        const int live[2] = {(int)((frame / 3) % 12), (int)((frame / 3 + 1) % 12)};
        for (int s : live){
            Built& built = texts[s];
            if (built.page < 0 || cache.getGeneration(built.page) != built.generation){
                built.page = cache.acquire(*font, styles[(size_t)s], ascii);
                ASSERT_GE(built.page, 0);
                built.generation = cache.getGeneration(built.page);
            }else{
                cache.touch(built.page);
            }
            // no glyph missing in a drawn frame
            for (uint32_t c : ascii)
                ASSERT_TRUE(cache.find(built.page, *font, styles[(size_t)s], c)) << frame << " " << s << " " << c;
            lastDrawn[built.page] = frame;
        }
        for (int s : live)
            EXPECT_EQ(cache.getGeneration(texts[s].page), texts[s].generation) << "a live text's page was recycled";
        // a page recycled in this frame was not drawn from in this frame or the last
        for (int p = 0; p < (int)cache.getPageCount(); p++){
            const uint32_t now = cache.getGeneration(p);
            if (generations.count(p) && generations[p] != now){
                auto drawn = drawnBefore.find(p);
                EXPECT_TRUE(drawn == drawnBefore.end() || drawn->second + 1 < frame) << "page " << p << " at frame " << frame;
            }
            generations[p] = now;
        }
    }
    EXPECT_GT(cache.getStats().recycles, 0u);
    EXPECT_EQ(cache.getStats().overBudget, 0u);
    EXPECT_LE(cache.getUsedBytes(), (size_t)256 * 1024);
    EXPECT_EQ(capture.count(D_LOG_WARN, "Bitmap text cache budget too small"), 1u);
    EXPECT_EQ(cache.getStats().diagnostics, 1u);
}

TEST(BitmapTextCache, StyledTextDrawsBackLayersFirst){
    BtcCacheReset reset;
    std::shared_ptr<BitmapFont> font = btcFont();
    ASSERT_TRUE(font);
    BitmapText text(font, "cache", 0);
    InterleavedBuffer buffer;
    buffer.addAttribute(AttributeType::POSITION, 3);
    buffer.addAttribute(AttributeType::TEXCOORD1, 2);
    buffer.addAttribute(AttributeType::COLOR, 4);
    std::vector<uint16_t> indices;
    std::vector<Vector2> positions;
    std::vector<CharExtent> extents;
    std::vector<TextGlyphInfo> info;
    unsigned int width = 0, height = 0;
    BitmapTextOptions options;
    options.style.outlineWidth = 1;
    options.style.outlineColor = 0xFF000000u;
    options.faceColor = Vector3(0.25f, 0.5f, 0.75f);
    text.createText("AB C", &buffer, indices, positions, extents, width, height, options, &info);
    ASSERT_TRUE(text.usesCache());
    ASSERT_EQ(info.size(), 8u);
    EXPECT_EQ(text.getBackQuads(), 4);
    Attribute* colors = buffer.getAttribute(AttributeType::COLOR);
    for (size_t g = 0; g < 8; g++){
        EXPECT_EQ(info[g].back, g < 4) << g;
        EXPECT_EQ(info[g].cluster, info[g % 4].cluster);
        const Vector4 c = buffer.getVector4(colors, (unsigned int)(g * 4));
        if (g < 4) {
            EXPECT_EQ(c, Vector4(1, 1, 1, 1));
        } else {
            EXPECT_EQ(c, Vector4(0.25f, 0.5f, 0.75f, 1.0f));
        }
    }
    EXPECT_TRUE(text.isColoredTexture());
}
