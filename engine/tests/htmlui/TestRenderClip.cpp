// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Clip masks without the stencil (S16 R5, R6, AC7): content clear of a rounded container's
// corners is clipped by the scissor alone (the mask is not drawn), a rounded clip whose content
// reaches the curves is clipped in the shaders, antialiased at one sample too, and a clip the
// shaders cannot do (three nested rounded clips) still draws its masks into the stencil.

#include "TestRenderCommon.h"

#include <gtest/gtest.h>

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
