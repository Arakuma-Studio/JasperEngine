// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// docs/specs/S82-title-designer.md R55, R56: the picker's preview strips. A strip is S80's shaper
// and outlines rasterized by S80's ThorVG rasterizer: it has coverage in the glyph colour, follows
// the variation axes, and draws a code point no face covers as a red box with a cross (none once a
// Fallback Font covers it). The strip queue uploads at most 2 strips a frame, keeps an LRU,
// destroys evicted textures a frame later and drops work for rows no longer requested.

#include <gtest/gtest.h>

#include "titleart/fonts/FontTestSupport.h"

#include "titleart/fonts/FontPreviewRaster.h"

#include <atomic>
#include <initializer_list>

using namespace jasperengine::editor::titlefonts;
using namespace jasperengine::editor::titlefonts::test;

namespace {

    StripRequest stripOf(const fs::path& font, const std::string& text) {
        StripRequest request;
        request.font = {font.string(), 0};
        request.text = text;
        request.pixelSize = 32.0f;
        request.color = 0x2080FFFFu;
        return request;
    }

    struct Coverage {
        size_t glyph = 0;       // pixels in the glyph colour with alpha
        size_t missingBox = 0;  // the red box's pixels
        uint64_t alpha = 0;
    };

    Coverage coverageOf(const StripImage& image) {
        Coverage c;
        for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
            const uint8_t* p = &image.rgba[i];
            if (p[3] == 0) continue;
            c.alpha += p[3];
            if (p[0] == 0x20 && p[1] == 0x80 && p[2] == 0xFF) c.glyph++;
            else if (p[0] == 235 && p[1] == 52 && p[2] == 52 && p[3] == 255) c.missingBox++;
        }
        return c;
    }

    // a texture stand-in that counts its destruction
    struct FakeUploaded : FontPreviewRaster::Uploaded {
        std::atomic<int>* destroyed;
        ImTextureID id;
        FakeUploaded(std::atomic<int>* destroyed, ImTextureID id) : destroyed(destroyed), id(id) {}
        ~FakeUploaded() override { (*destroyed)++; }
        ImTextureID texture() const override { return id; }
    };

}

// Red control: rasterizing the em boxes instead of the outlines (or an empty element) fails the
// coverage bounds; a colour taken from the wrong byte order fails the glyph-colour count
TEST(FontPreviewRasterTest, StripHasCoverageInTheGlyphColour) {
    const StripImage image = rasterizeStrip(stripOf(oswaldPath(), "Title"));
    ASSERT_TRUE(image.ok) << image.error;
    EXPECT_GT(image.width, 40);
    EXPECT_GT(image.height, 20);
    EXPECT_LT(image.height, 80);
    ASSERT_EQ(image.rgba.size(), (size_t)image.width * image.height * 4);
    const Coverage c = coverageOf(image);
    EXPECT_GT(c.glyph, 100u);
    EXPECT_EQ(c.missingBox, 0u);
    EXPECT_TRUE(image.missing.empty());
    // ink stays inside the strip, not all of it covered
    EXPECT_LT(c.glyph, (size_t)image.width * image.height / 2);
    EXPECT_GT(image.baseline, 0.0f);
    EXPECT_LT(image.baseline, (float)image.height);

    // a heavier instance has more ink (the variation axes reach the outlines)
    StripRequest light = stripOf(oswaldPath(), "Title");
    light.variations["wght"] = 200;
    StripRequest bold = light;
    bold.variations["wght"] = 700;
    EXPECT_GT(coverageOf(rasterizeStrip(bold)).alpha, coverageOf(rasterizeStrip(light)).alpha * 5 / 4);
    EXPECT_NE(light.key(), bold.key());

    // vertical strips are taller than wide (Dela Gothic One sets kanji upright)
    StripRequest vertical = stripOf(delaGothicPath(), "\xE6\xBF\x80\xE7\x86\xB1\xE6\xBF\x80\xE7\x86\xB1");
    vertical.vertical = true;
    const StripImage column = rasterizeStrip(vertical);
    ASSERT_TRUE(column.ok) << column.error;
    EXPECT_GT(column.height, column.width);
}

// Red control: drawing missing clusters with the .notdef outline (no box) fails the red count;
// leaving the Fallback Fonts out of the strip keeps the box in the second half
TEST(FontPreviewRasterTest, MissingGlyphIsARedBoxWithACross) {
    const StripImage image = rasterizeStrip(stripOf(oswaldPath(), "A\xE6\xBF\x80"));   // A激
    ASSERT_TRUE(image.ok) << image.error;
    EXPECT_EQ(image.missing, (std::vector<uint32_t>{0x6FC0}));
    const Coverage c = coverageOf(image);
    EXPECT_GT(c.missingBox, 60u) << "the box and its cross";
    EXPECT_GT(c.glyph, 20u) << "the A is drawn";

    StripRequest withFallback = stripOf(oswaldPath(), "A\xE6\xBF\x80");
    withFallback.fallbacks.push_back({delaGothicPath().string(), 0});
    const StripImage covered = rasterizeStrip(withFallback);
    ASSERT_TRUE(covered.ok) << covered.error;
    EXPECT_TRUE(covered.missing.empty());
    EXPECT_EQ(coverageOf(covered).missingBox, 0u);

    const StripImage unreadable = rasterizeStrip(stripOf(fs::path("/nonexistent/font.ttf"), "A"));
    EXPECT_FALSE(unreadable.ok);
    EXPECT_FALSE(unreadable.error.empty());
}

// Red control: uploading every finished strip at once uploads 5 in one frame; destroying an
// evicted texture at once (not at the next beginFrame) counts it destroyed before the frame ends
TEST(FontPreviewRasterTest, QueueUploadsTwoPerFrameKeepsAnLruAndDropsUnrequestedRows) {
    std::atomic<int> rasterized{0};
    std::atomic<int> destroyed{0};
    int uploaded = 0;
    FontPreviewRaster raster(3, 2);
    raster.setRasterizer([&rasterized](const StripRequest&) {
        rasterized++;
        StripImage image;
        image.ok = true;
        image.width = image.height = 1;
        image.rgba = {255, 255, 255, 255};
        return image;
    });
    raster.setUploader([&uploaded, &destroyed](const StripImage&) {
        uploaded++;
        return std::unique_ptr<FontPreviewRaster::Uploaded>(new FakeUploaded(&destroyed, (ImTextureID)(intptr_t)uploaded));
    });
    std::vector<StripRequest> rows;
    for (int i = 0; i < 5; i++) rows.push_back(stripOf(oswaldPath(), "row " + std::to_string(i)));

    // frame 1: nothing ready, five queued
    raster.beginFrame();
    for (const StripRequest& row : rows) EXPECT_EQ(raster.request(row), nullptr);
    raster.endFrame();
    raster.waitIdle();
    EXPECT_EQ(rasterized.load(), 5);

    const auto requestRows = [&raster, &rows](std::initializer_list<int> which) {
        int ready = 0;
        for (int i : which) {
            const FontPreviewRaster::Strip* strip = raster.request(rows[(size_t)i]);
            ready += strip && strip->texture ? 1 : 0;
        }
        return ready;
    };

    // frame 2: all five finished, two uploaded
    raster.beginFrame();
    EXPECT_EQ(requestRows({0, 1, 2, 3, 4}), 2);
    EXPECT_EQ(raster.uploadsThisFrame(), 2);
    raster.endFrame();

    // frame 3, scrolled down to rows 2-4: two more uploads; the LRU (3) evicts row 0, whose
    // texture lives until the next frame starts
    raster.beginFrame();
    EXPECT_EQ(requestRows({2, 3, 4}), 2);
    EXPECT_EQ(raster.uploadsThisFrame(), 2);
    EXPECT_EQ(raster.cachedCount(), 3u);
    EXPECT_EQ(destroyed.load(), 0) << "not while the frame may still draw it";
    raster.endFrame();

    // frame 4: row 0's texture destroyed; row 4 uploaded, evicting row 1
    raster.beginFrame();
    EXPECT_EQ(destroyed.load(), 1);
    EXPECT_EQ(requestRows({2, 3, 4}), 3);
    EXPECT_EQ(raster.cachedCount(), 3u);
    raster.endFrame();
    raster.beginFrame();
    EXPECT_EQ(destroyed.load(), 2);
    raster.endFrame();
    EXPECT_EQ(uploaded, 5);
    EXPECT_EQ(rasterized.load(), 5) << "each strip rasterized once";

    // a row requested once and then scrolled away: its work is dropped at the end of the next frame
    const StripRequest gone = stripOf(oswaldPath(), "scrolled away");
    raster.beginFrame();
    EXPECT_EQ(raster.request(gone), nullptr);
    raster.endFrame();
    EXPECT_EQ(raster.pendingCount(), 1u);
    raster.beginFrame();
    raster.endFrame();
    EXPECT_EQ(raster.pendingCount(), 0u);

    raster.clear();
    EXPECT_EQ(raster.cachedCount(), 0u);
    raster.waitIdle();
}
