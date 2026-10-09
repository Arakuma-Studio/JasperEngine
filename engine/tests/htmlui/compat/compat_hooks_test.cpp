// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// p2-runtime's hooks into phase-0/1 code: H6 (RmlFontEngine::hasFamily, the no-text test
// switch, font-effect geometry for text-shadow) and H7 (RmlRenderer::LoadTexture serving
// "@htmlui/" virtual files and ?htmlui-filter=nearest). H8 is compat_svg_test.cpp, H9
// TestVectorLottie.cpp (LottieElementTest.PlayOnHover).

#include "RuntimeTestCommon.h"
#include "TestRenderCommon.h"

#include "RmlFontEngine.h"
#include "RmlRenderer.h"
#include "htmlui/compat/VirtualFiles.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::compat;
using namespace jasperengine::htmlui::compat;

namespace {

    using CompatFontHookTest = RuntimeContextTest;

    // vertices drawn with a texture (glyph quads) in the last frame
    size_t texturedVertices(const RecordingRenderInterface& recording){
        size_t count = 0;
        for (const RecordingRenderInterface::Draw& draw : recording.getDraws()){
            if (draw.texture)
                count += draw.vertexCount;
        }
        return count;
    }

    std::string readFixtureBytes(const std::string& name){
        std::ifstream file(std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/fixtures/" + name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

}

// ---------------------------------------------------------------- H6

TEST_F(CompatFontHookTest, HasFamily){
    EXPECT_TRUE(RmlFontEngine::hasFamily("sans-serif"));
    EXPECT_TRUE(RmlFontEngine::hasFamily("monospace"));
    EXPECT_TRUE(RmlFontEngine::hasFamily("Roboto"));
    EXPECT_TRUE(RmlFontEngine::hasFamily("\"Roboto\""));
    EXPECT_FALSE(RmlFontEngine::hasFamily("No Such Family"));
    EXPECT_FALSE(RmlFontEngine::hasFamily(""));
}

TEST_F(CompatFontHookTest, FontEffectsGenerateGeometry){
    // text-shadow as compat writes it (shadow / glow), and the plain text
    ASSERT_NE(show("<p id='plain'>Shadow</p>"), nullptr);
    const size_t plain = texturedVertices(recording);
    ASSERT_GT(plain, 0u);

    ASSERT_NE(show("<p id='shadow' style='font-effect: shadow(2px 2px #ff0000ff);'>Shadow</p>"), nullptr);
    const size_t shadow = texturedVertices(recording);
    EXPECT_EQ(shadow, plain * 2);   // the shadow layer redraws every glyph

    ASSERT_NE(show("<p id='both' style='font-effect: glow(0px 2px 1px 1px #00ff00ff), shadow(2px 2px #ff0000ff);'>Shadow</p>"), nullptr);
    EXPECT_EQ(texturedVertices(recording), plain * 3);
    // the glow has glyphs of its own (RGBA8 effect pages)
    EXPECT_GT(HtmlUI::getFontEngine().getEffectPageCount(), 0u);
}

TEST_F(CompatFontHookTest, RenderTextForTestingKeepsLayout){
    RmlFontEngine& fonts = HtmlUI::getFontEngine();
    ASSERT_TRUE(fonts.getRenderTextForTesting());
    ASSERT_NE(show("<p><span id='text' style='font-effect: shadow(1px 1px #000000ff);'>Some text here</span></p>"), nullptr);
    const float width = byId("text")->GetBox().GetSize().x;
    ASSERT_GT(texturedVertices(recording), 0u);

    fonts.setRenderTextForTesting(false);
    frame();
    EXPECT_EQ(texturedVertices(recording), 0u);
    EXPECT_FLOAT_EQ(byId("text")->GetBox().GetSize().x, width);

    fonts.setRenderTextForTesting(true);
    frame();
    EXPECT_GT(texturedVertices(recording), 0u);
}

// ---------------------------------------------------------------- H7

TEST(CompatTextureHook, StripTextureFilter){
    bool nearest = false;
    EXPECT_EQ(RmlRenderer::stripTextureFilter("ui/a.png", nearest), "ui/a.png");
    EXPECT_FALSE(nearest);
    EXPECT_EQ(RmlRenderer::stripTextureFilter("ui/a.png?htmlui-filter=nearest", nearest), "ui/a.png");
    EXPECT_TRUE(nearest);
    EXPECT_EQ(RmlRenderer::stripTextureFilter("ui/a.svg?svgScale=2&htmlui-filter=nearest", nearest), "ui/a.svg?svgScale=2");
    EXPECT_TRUE(nearest);
    EXPECT_EQ(RmlRenderer::stripTextureFilter("ui/a.svg?htmlui-filter=nearest&svgScale=2", nearest), "ui/a.svg?svgScale=2");
    EXPECT_TRUE(nearest);
    EXPECT_EQ(RmlRenderer::stripTextureFilter("ui/a.png?v=3", nearest), "ui/a.png?v=3");
    EXPECT_FALSE(nearest);
}

TEST(CompatTextureHook, VirtualFilesAndNearestSampling){
    JASPER_REQUIRE_GPU();
    const std::string png = readFixtureBytes("compat/runtime/pixels2x2.png");
    ASSERT_FALSE(png.empty());
    VirtualFiles::add("@htmlui/test/pixels.png", png);
    VirtualFiles::add("@htmlui/test/square.svg", std::string("<svg xmlns='http://www.w3.org/2000/svg' width='8' height='8'>"
                                                             "<rect width='8' height='8' fill='#00ff00'/></svg>"));

    TestSystem::get().clearLogs();
    {
        RenderTestContext test(256, 128);
        ASSERT_TRUE(test.isValid());
        const std::string rml =
            "<rml><head><style>body { display: block; width: 256px; height: 128px; margin: 0; } "
            "img { display: block; position: absolute; top: 0; width: 64px; height: 64px; }</style></head><body>"
            "<img style='left: 0;' src='/@htmlui/test/pixels.png?htmlui-filter=nearest'/>"
            "<img style='left: 64px;' src='/@htmlui/test/pixels.png'/>"
            "<img style='left: 128px; width: 16px; height: 16px;' src='/@htmlui/test/square.svg'/>"
            "</body></rml>";
        ASSERT_NE(test.loadFromMemory(rml, "compat/runtime/hooks.rml"), nullptr);
        ASSERT_TRUE(test.renderUntilDrawn());

        std::vector<uint8_t> rgba;
        ASSERT_TRUE(test.readScreen(rgba));
        PixelProbe probe{rgba, 256};
        // nearest: hard cell edges (red | green on the top row, blue | white below)
        EXPECT_TRUE(probe.is(31, 10, 255, 0, 0));
        EXPECT_TRUE(probe.is(32, 10, 0, 255, 0));
        EXPECT_TRUE(probe.is(31, 50, 0, 0, 255));
        // linear: the same spot blends the two texels
        const uint8_t* blended = probe.at(64 + 31, 10);
        EXPECT_GT(blended[0], 60);
        EXPECT_GT(blended[1], 60);
        // the virtual SVG
        EXPECT_TRUE(probe.is(136, 8, 0, 255, 0));
    }
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u);

    // a missing virtual file is an ordinary load error
    TestSystem::get().clearLogs();
    {
        RenderTestContext test(64, 64);
        ASSERT_NE(test.loadFromMemory("<rml><head><style>img { display: block; width: 8px; height: 8px; }</style></head>"
                                      "<body><img src='/@htmlui/test/missing.png'/></body></rml>", "compat/runtime/hooks.rml"), nullptr);
        test.renderUntilDrawn();
    }
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "could not load image '/@htmlui/test/missing.png'"), 1u);

    VirtualFiles::remove("@htmlui/test/pixels.png");
    VirtualFiles::remove("@htmlui/test/square.svg");
}
