// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Clip masks without the stencil (S16 R5, R6, AC7): content clear of a rounded container's
// corners is clipped by the scissor alone (the mask is not drawn), a rounded clip whose content
// reaches the curves is clipped in the shaders, antialiased at one sample too, and a clip the
// shaders cannot do (three nested rounded clips) still draws its masks into the stencil.
// S92: clip-path's inset(), circle() and ellipse() clip in the shaders without a layer (R8, AC8);
// an overflow clip under a scale and translation is a scissor whatever its content holds, and
// culls the text lines outside it (R11, AC12); a turned clip keeps the stencil.

#include "TestRenderCommon.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int width = 320;
    const int height = 240;

    struct Batching {
        bool previous;
        explicit Batching(bool on): previous(RmlRenderer::isBatching()){
            RmlRenderer::setBatchingForTesting(on);
        }
        ~Batching(){
            RmlRenderer::setBatchingForTesting(previous);
        }
    };

    std::string documentOf(const std::string& css, const std::string& body){
        return "<rml><head><style>body { margin: 0; } div { display: block; } " + css + "</style></head><body>" + body + "</body></rml>";
    }

    // a rounded overflow container with a child inset by its padding: clear of the curves
    const char* const insetCss =
        "#clip { position: absolute; left: 20px; top: 20px; width: 200px; height: 140px; border-radius: 16px; overflow: hidden;"
        " background-color: #303040; padding: 20px; box-sizing: border-box; }"
        "#child { width: 100%; height: 100%; background-color: #e0a020; }";
    // the child fills the container: its corners are clipped by the curves
    const char* const fullCss =
        "#clip { position: absolute; left: 20px; top: 20px; width: 200px; height: 140px; border-radius: 40px; overflow: hidden; }"
        "#child { width: 100%; height: 100%; background-color: #40c0f0; }";

    struct Frame {
        std::vector<uint8_t> screen;
        HtmlUIRenderStats stats;
    };

    bool drawDocument(const std::string& rml, int samples, Frame& frame){
        RenderTestContext test(width, height, 0, 0, samples);
        if (!test.isValid() || !test.loadFromMemory(rml, "render/clip.rml") || !test.renderUntilDrawn())
            return false;
        frame.stats = test.renderer.getStats();
        return test.readScreen(frame.screen);
    }

    // the golden metric's defaults between two images, up to 0.5 % of the pixels apart (edge pixels:
    // a mask layer's multisampled or feathered mesh against the exact coverage, as RenderPrimitive)
    ::testing::AssertionResult similar(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const std::string& label){
        if (a.size() != b.size() || a.empty())
            return ::testing::AssertionFailure() << label << ": sizes differ";
        size_t bad = 0;
        double sum = 0.0;
        const size_t pixels = a.size() / 4;
        for (size_t i = 0; i < pixels; i++){
            int difference = 0;
            for (int c = 0; c < 4; c++)
                difference = std::max(difference, std::abs((int)a[i * 4 + c] - (int)b[i * 4 + c]));
            sum += difference;
            if (difference > CompareOptions().pixelThreshold)
                bad++;
        }
        const double mean = sum / (double)pixels;
        if ((double)bad / (double)pixels <= 0.005 && mean <= CompareOptions().maxMeanDiff)
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << label << ": " << bad << " bad pixels, mean " << mean;
    }

    // non-background pixels in a rectangle of the screen
    int paintedPixels(const std::vector<uint8_t>& screen, int x0, int y0, int x1, int y1){
        int painted = 0;
        for (int y = y0; y < y1; y++){
            for (int x = x0; x < x1; x++){
                const uint8_t* p = &screen[(size_t(y) * width + x) * 4];
                if (std::abs(p[0] - RenderTestContext::background) > 8 || std::abs(p[1] - RenderTestContext::background) > 8 ||
                    std::abs(p[2] - RenderTestContext::background) > 8)
                    painted++;
            }
        }
        return painted;
    }

    int largestDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        int largest = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); i++)
            largest = std::max(largest, std::abs((int)a[i] - (int)b[i]));
        return a.size() == b.size() ? largest : 255;
    }

}

TEST(RenderClip, ContentClearOfCornersDrawsNoMask){
    JASPER_REQUIRE_GPU();
    const std::string rml = documentOf(insetCss, "<div id=\"clip\"><div id=\"child\"/></div>");
    Frame batched, unbatched;
    {
        Batching on(true);
        ASSERT_TRUE(drawDocument(rml, 4, batched));
    }
    {
        Batching off(false);
        ASSERT_TRUE(drawDocument(rml, 4, unbatched));
    }
    // the stencil is not touched; the image is the masked one
    EXPECT_EQ(batched.stats.stencilDraws, 0u);
    EXPECT_GE(batched.stats.stencilClipsElided, 1u);
    EXPECT_LE(largestDifference(batched.screen, unbatched.screen), 1);
}

TEST(RenderClip, RoundedClipIsAnalyticAndAntialiased){
    JASPER_REQUIRE_GPU();
    // (S16's analytic clip and its goldens: on whatever the run's switches, AC1)
    S16On s16;
    const std::string rml = documentOf(fullCss, "<div id=\"clip\"><div id=\"child\"/></div>");
    for (int samples : {1, 4}){
        Frame frame;
        ASSERT_TRUE(drawDocument(rml, samples, frame));
        EXPECT_EQ(frame.stats.stencilDraws, 0u) << samples << " samples";
        EXPECT_GE(frame.stats.analyticClips, 1u) << samples << " samples";

        // along the top-left curve (centre (60, 60), radius 40): pixels strictly between the
        // background and the child's colour, even at one sample
        PixelProbe probe{frame.screen, width};
        int between = 0;
        for (int i = 0; i < 90; i++){
            const float angle = 3.14159265f * (1.0f + 0.5f * (float)i / 90.0f);
            const int x = (int)(60.0f + 40.0f * std::cos(angle));
            const int y = (int)(60.0f + 40.0f * std::sin(angle));
            const uint8_t* p = &frame.screen[(size_t(y) * width + x) * 4];
            // the blue channel goes from the background's 0x1E to the child's 0xF0
            if (p[2] > RenderTestContext::background + 8 && p[2] < 0xF0 - 8)
                between++;
        }
        EXPECT_GT(between, 20) << samples << " samples";
        // inside and outside
        EXPECT_TRUE(probe.is(120, 90, 0x40, 0xC0, 0xF0, 255, 1));
        EXPECT_TRUE(probe.isBackground(22, 22));

        CompareResult result = compareWithGolden(samples == 1 ? "p1/rounded_clip_1x" : "p1/rounded_clip_4x", frame.screen, width, height);
        EXPECT_TRUE(result.passed) << result.message;
    }
}

TEST(RenderClip, NestedRoundedClipsUseTheStencil){
    JASPER_REQUIRE_GPU();
    // three rounded clips, each one's content reaching the next one's curves: more than the two
    // shapes the shaders take
    const std::string rml = documentOf(
        ".clip { overflow: hidden; border-radius: 30px; padding: 0; }"
        "#a { position: absolute; left: 20px; top: 20px; width: 260px; height: 190px; background-color: #503030; }"
        "#b { margin-left: 30px; width: 200px; height: 170px; background-color: #305030; }"
        "#c { margin-top: 30px; width: 190px; height: 120px; background-color: #303050; }"
        "#fill { width: 100%; height: 100%; background-color: #f0f0a0; }",
        "<div id=\"a\" class=\"clip\"><div id=\"b\" class=\"clip\"><div id=\"c\" class=\"clip\"><div id=\"fill\"/></div></div></div>");
    Frame frame;
    ASSERT_TRUE(drawDocument(rml, 4, frame));
    EXPECT_GT(frame.stats.stencilDraws, 0u);
    CompareResult result = compareWithGolden("p1/rounded_clip_nested", frame.screen, width, height);
    EXPECT_TRUE(result.passed) << result.message;

    // and the same image unbatched (the stencil either way)
    Frame unbatched;
    {
        Batching off(false);
        ASSERT_TRUE(drawDocument(rml, 4, unbatched));
    }
    EXPECT_LE(largestDifference(frame.screen, unbatched.screen), 1);
}

namespace {

    // clip-path basic shapes (as compat writes them: mask-image: html-shape(...)) on elements with
    // a child crossing the shape's edge. `extra` is added to the child's style
    std::string clipPathDocument(const std::string& extra){
        return documentOf(".s { position: absolute; top: 20px; width: 90px; height: 70px; background-color: #e0a020; }"
                          "#inset { left: 10px; mask-image: html-shape(\"inset(8px 6px round 14px)\"); }"
                          "#circle { left: 115px; mask-image: html-shape(\"circle(32px at 50% 50%)\"); }"
                          "#ellipse { left: 220px; mask-image: html-shape(\"ellipse(40px 26px at 45% 55%)\"); }"
                          ".s div { margin-top: 25px; margin-left: -10px; width: 110px; height: 20px; background-color: #3060c0; " + extra + " }",
                          "<div class=\"s\" id=\"inset\"><div/></div><div class=\"s\" id=\"circle\"><div/></div><div class=\"s\" id=\"ellipse\"><div/></div>");
    }

}

TEST(RenderClip, ClipPathBasicShapesAreAnalytic){
    JASPER_REQUIRE_GPU();
    // the layers of the mesh path in the frame compared, not drawn from the layer cache (S92 R9)
    LayerCacheOff cacheOff;
    S16On s16;
    // the same shapes with a text shadow on the child (nothing to draw, but a possible layer
    // inside): the mask layer they took before (S92 R8)
    const std::string analytic = clipPathDocument("");
    const std::string masked = clipPathDocument("text-shadow: 1px 1px 0px #000000;");
    for (int samples : {1, 4}){
        const std::string label = std::to_string(samples) + " samples";
        Frame frame, layered;
        ASSERT_TRUE(drawDocument(analytic, samples, frame)) << label;
        ASSERT_TRUE(drawDocument(masked, samples, layered)) << label;
        EXPECT_EQ(frame.stats.layersPushed, 0u) << label;
        EXPECT_EQ(frame.stats.stencilDraws, 0u) << label;
        EXPECT_GE(frame.stats.analyticClips, 3u) << label;
        EXPECT_EQ(frame.stats.passes, 1u) << label;
        EXPECT_GT(layered.stats.layersPushed, 0u) << label;
        EXPECT_TRUE(similar(frame.screen, layered.screen, label));

        PixelProbe probe{frame.screen, width};
        // inside each shape, the box's colour and the child's; outside, the background
        EXPECT_TRUE(probe.is(55, 32, 0xe0, 0xa0, 0x20)) << label;
        EXPECT_TRUE(probe.is(55, 55, 0x30, 0x60, 0xc0)) << label;
        EXPECT_TRUE(probe.isBackground(12, 22)) << label;          // the inset's corner
        EXPECT_TRUE(probe.is(160, 55, 0x30, 0x60, 0xc0)) << label;
        EXPECT_TRUE(probe.isBackground(117, 22)) << label;         // beyond the circle
        EXPECT_TRUE(probe.isBackground(117, 55)) << label;         // the child beyond the circle
        EXPECT_TRUE(probe.isBackground(222, 22)) << label;         // beyond the ellipse
        // the circle's edge (centre (160, 55), radius 32) above the child antialiased, at one
        // sample too
        int between = 0;
        for (int i = 0; i < 90; i++){
            const float angle = 3.14159265f * (1.0f + 0.5f * (float)i / 90.0f);
            const int x = (int)std::lround(160.0f + 32.0f * std::cos(angle));
            const int y = (int)std::lround(55.0f + 32.0f * std::sin(angle));
            if (y >= 44)
                continue;
            const uint8_t* p = probe.at(x, y);
            if (p[0] > RenderTestContext::background + 8 && p[0] < 0xe0 - 8)
                between++;
        }
        EXPECT_GT(between, 10) << label;
        CompareResult result = compareWithGolden(samples == 1 ? "s92/clip_path_shapes_1x" : "s92/clip_path_shapes_4x", frame.screen, width, height);
        EXPECT_TRUE(result.passed) << result.message;
    }

    // a concave polygon() keeps the mask layer (a convex one is analytic: ConvexPolygonClipPathIsAnalytic)
    Frame polygon;
    ASSERT_TRUE(drawDocument(documentOf("#p { position: absolute; left: 20px; top: 20px; width: 90px; height: 70px; background-color: #e0a020;"
                                        " mask-image: html-shape(\"polygon(0% 0%, 100% 0%, 50% 40%, 100% 100%, 0% 100%)\"); }",
                                        "<div id=\"p\"/>"),
                             4, polygon));
    EXPECT_GT(polygon.stats.layersPushed, 0u);
}

// S92: a convex clip-path polygon() (the port's octagon cards, a diamond) clips in the shaders, without a layer or
// the stencil, antialiased at one sample too; its pixels match the stencil's (the classic replay: its triangle fan)
// but at the edges
TEST(RenderClip, ConvexPolygonClipPathIsAnalytic){
    JASPER_REQUIRE_GPU();
    S16On s16;
    const char* const shapes[] = {
        "polygon(21px 0px, 69px 0px, 90px 21px, 90px 49px, 69px 70px, 21px 70px, 0px 49px, 0px 21px)",
        "polygon(50% 0%, 100% 50%, 50% 100%, 0% 50%)",
        "polygon(evenodd, 0% 100%, 50% 0%, 100% 100%)",
    };
    for (const char* shape : shapes){
        for (int samples : {4, 1}){
            const std::string label = std::string(shape) + ", " + std::to_string(samples) + " samples";
            const std::string css = std::string("#p { position: absolute; left: 20px; top: 20px; width: 90px; height: 70px; background-color: #e0a020;"
                                                " mask-image: html-shape(\"") + shape + "\"); } #c { position: absolute; left: 10px; top: 30px; width: 60px;"
                                                " height: 30px; background-color: #3060c0; }";
            Frame frame;
            ASSERT_TRUE(drawDocument(documentOf(css, "<div id=\"p\"><div id=\"c\"/></div>"), samples, frame)) << label;
            EXPECT_EQ(frame.stats.layersPushed, 0u) << label;
            EXPECT_EQ(frame.stats.stencilDraws, 0u) << label;
            EXPECT_GT(frame.stats.analyticClips, 0u) << label;
            Frame stencil;
            {
                ClassicReplay classic;
                ASSERT_TRUE(drawDocument(documentOf(css, "<div id=\"p\"><div id=\"c\"/></div>"), samples, stencil)) << label;
            }
            EXPECT_GT(stencil.stats.stencilDraws, 0u) << label;
            EXPECT_TRUE(similar(frame.screen, stencil.screen, label));
        }
    }
}

namespace {

    // U27 (the port's lists): a panel scaled and moved (scale and translation only) holds an
    // overflow: hidden scroll viewport of rows with filtered tiles (a colour filter, as the port's
    // ScrollView draws its items: a layer each, which reads nothing beyond its element) and text,
    // scrolled so that rows are cut at both edges and others lie wholly beyond them. `transform` is
    // the panel's
    std::string scaledListDocument(const std::string& transform, bool tiles, const std::string& tileFilter = "brightness(1.2)"){
        std::string rows;
        for (int i = 0; i < 40; i++){
            rows += "<div class=\"row\">";
            if (tiles)
                rows += "<div class=\"tile\"/>";
            rows += "Item " + std::to_string(i) + "</div>";
        }
        return documentOf("#panel { position: absolute; left: 30px; top: 20px; width: 160px; height: 120px; background-color: #283040;"
                          " transform-origin: 0 0; transform: " + transform + "; }"
                          "#view { position: absolute; left: 10px; top: 20px; width: 140px; height: 70px; overflow: hidden; }"
                          ".row { height: 16px; font-size: 12px; color: #f0f0f0; font-family: sans-serif; }"
                          ".tile { float: left; width: 12px; height: 12px; margin-right: 4px; background-color: #e04040;"
                          " filter: " + tileFilter + "; }",
                          "<div id=\"panel\"><div id=\"view\">" + rows + "</div></div>");
    }

    bool drawScrolled(const std::string& rml, int samples, Frame& frame){
        RenderTestContext test(width, height, 0, 0, samples);
        if (!test.isValid())
            return false;
        Rml::ElementDocument* document = test.loadFromMemory(rml, "render/clip.rml");
        if (!document || !test.renderUntilDrawn())
            return false;
        // rows 2 to 6 visible, rows 2 and 6 cut by the viewport's edges
        document->GetElementById("view")->SetScrollTop(40.0f);
        if (!test.renderUntilDrawn())
            return false;
        frame.stats = test.renderer.getStats();
        return test.readScreen(frame.screen);
    }

}

TEST(RenderClip, TextInScaledScrollWithFilterLayers){
    JASPER_REQUIRE_GPU();
    S16On s16;
    // the viewport at scale 1.25 from (34, 20) + (10, 20) * 1.25: (46.5, 45) to (221.5, 132.5)
    Frame frame;
    ASSERT_TRUE(drawScrolled(scaledListDocument("translate(4px, 0px) scale(1.25)", true), 4, frame));
    // a scissor, no stencil mask, whatever the content holds (filter layers, text)
    EXPECT_EQ(frame.stats.stencilDraws, 0u);
    EXPECT_GT(frame.stats.layersPushed, 0u);
    // nothing of the rows beyond the viewport's edges: the panel's colour above and below it
    PixelProbe probe{frame.screen, width};
    EXPECT_TRUE(probe.is(100, 40, 0x28, 0x30, 0x40));
    EXPECT_TRUE(probe.is(100, 137, 0x28, 0x30, 0x40));
    int strays = 0;
    for (int y : {36, 38, 40, 42, 136, 138, 140, 150, 160}){
        for (int x = 50; x < 215; x++){
            const uint8_t* p = probe.at(x, y);
            if (std::abs(p[0] - 0x28) > 4 || std::abs(p[1] - 0x30) > 4 || std::abs(p[2] - 0x40) > 4)
                strays++;
        }
    }
    EXPECT_EQ(strays, 0);
    // the visible rows are drawn
    EXPECT_GT(paintedPixels(frame.screen, 48, 50, 215, 128), 500);
    CompareResult result = compareWithGolden("s92/u27_scaled_scroll", frame.screen, width, height);
    EXPECT_TRUE(result.passed) << result.message;

    // the text lines beyond the viewport are culled as without the transform: as few draws
    // submitted as the unscaled list, far fewer than a turned one (no scissor, nothing culled)
    Frame scaled, plain, turned;
    ASSERT_TRUE(drawScrolled(scaledListDocument("scale(1.25)", false), 4, scaled));
    ASSERT_TRUE(drawScrolled(scaledListDocument("none", false), 4, plain));
    ASSERT_TRUE(drawScrolled(scaledListDocument("rotate(0.5deg)", false), 4, turned));
    EXPECT_LE(scaled.stats.geometrySubmitted, plain.stats.geometrySubmitted + 2);
    EXPECT_GT(turned.stats.geometrySubmitted, scaled.stats.geometrySubmitted + 20);
}

// U27 with tiles whose filter reaches beyond them (a drop shadow): a cut row's filter window crosses
// the viewport's edge, so the composite is clipped by the viewport's rectangle, in the composite's
// shader (S92 B2) instead of a stencil mask of it; nothing of a tile or its shadow beyond the
// edges (a scissor alone let half the samples of the row above it through on llvmpipe at 4
// samples), with the layer cache on (its results drawn clipped) and off
TEST(RenderClip, DropShadowTilesAcrossScaledClip){
    JASPER_REQUIRE_GPU();
    S16On s16;
    for (bool cache : {true, false}){
        for (int samples : {4, 1}){
            const std::string label = std::string(cache ? "cache" : "no cache") + ", " + std::to_string(samples) + " samples";
            const bool before = RmlRenderer::isLayerCache();
            RmlRenderer::setLayerCacheForTesting(cache);
            Frame frame;
            const bool drawn = drawScrolled(scaledListDocument("translate(4px, 0px) scale(1.25)", true, "drop-shadow(#102080 0px 3px 0px)"), samples,
                                            frame);
            RmlRenderer::setLayerCacheForTesting(before);
            ASSERT_TRUE(drawn) << label;
            EXPECT_EQ(frame.stats.stencilDraws, 0u) << label;
            EXPECT_GT(frame.stats.analyticClips, 0u) << label;
            PixelProbe probe{frame.screen, width};
            // the rows TextInScaledScrollWithFilterLayers probes (the viewport's own edges are a
            // rounded scissor's), and below the bottom edge where a cut row's shadow falls
            int strays = 0;
            std::string rows;
            for (int y : {36, 38, 40, 42, 136, 137, 138, 140, 150}){
                int row = 0;
                for (int x = 50; x < 215; x++){
                    const uint8_t* p = probe.at(x, y);
                    if (std::abs(p[0] - 0x28) > 4 || std::abs(p[1] - 0x30) > 4 || std::abs(p[2] - 0x40) > 4)
                        row++;
                }
                if (row > 0)
                    rows += " " + std::to_string(y) + ":" + std::to_string(row);
                strays += row;
            }
            EXPECT_EQ(strays, 0) << label << ", rows" << rows;
            // the tiles and their shadows inside
            EXPECT_GT(paintedPixels(frame.screen, 48, 50, 215, 128), 500) << label;
        }
    }
}

TEST(RenderClip, RotatedClipKeepsStencil){
    JASPER_REQUIRE_GPU();
    S16On s16;
    // the same list turned: its clip cannot be a scissor, and with filter layers inside it is
    // not analytic either
    Frame frame;
    ASSERT_TRUE(drawScrolled(scaledListDocument("rotate(6deg) scale(1.25)", true), 4, frame));
    EXPECT_GT(frame.stats.stencilDraws, 0u);
    CompareResult result = compareWithGolden("s92/u27_rotated_scroll", frame.screen, width, height);
    EXPECT_TRUE(result.passed) << result.message;
}
