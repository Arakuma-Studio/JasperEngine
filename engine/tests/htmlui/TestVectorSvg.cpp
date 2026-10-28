// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SVG on LunaSVG (p1-vector, benchmark decision): the rasteriser shared by TextureData and the
// tests, RmlUi's SVG plugin (<svg src>, inline <svg>, crop-to-content, decorator: svg()) at
// several sizes, and the TextureData regression against the nanosvg-era sizes and colours.
// CPU only: the plugin draws through a RecordingRenderInterface.

#include "harness/RecordingRenderInterface.h"
#include "harness/TestSystem.h"

#include "HtmlUI.h"
#include "util/LunaSVGRuntime.h"
#include "texture/TextureData.h"

#include <RmlUi/Core.h>

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    std::string readFixture(const std::string& name){
        std::ifstream file(std::string(JASPER_HTMLUI_TEST_SOURCE_DIR) + "/fixtures/" + name, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    }

    struct MeanColour {
        double r = 0, g = 0, b = 0, a = 0;
    };

    // alpha-weighted mean colour of straight-alpha RGBA8, and the mean alpha
    MeanColour meanColour(const uint8_t* rgba, int width, int height){
        MeanColour mean;
        double alphaSum = 0;
        for (size_t i = 0; i < (size_t)width * height; i++){
            const double a = rgba[i * 4 + 3];
            alphaSum += a;
            mean.r += rgba[i * 4 + 0] * a;
            mean.g += rgba[i * 4 + 1] * a;
            mean.b += rgba[i * 4 + 2] * a;
        }
        if (alphaSum > 0){
            mean.r /= alphaSum;
            mean.g /= alphaSum;
            mean.b /= alphaSum;
        }
        mean.a = alphaSum / ((double)width * height);
        return mean;
    }

    ::testing::AssertionResult isPremultiplied(const std::vector<uint8_t>& rgba){
        for (size_t i = 0; i < rgba.size(); i += 4){
            if (rgba[i] > rgba[i + 3] || rgba[i + 1] > rgba[i + 3] || rgba[i + 2] > rgba[i + 3])
                return ::testing::AssertionFailure() << "pixel " << i / 4 << " has a channel above its alpha";
        }
        return ::testing::AssertionSuccess();
    }

    size_t partialPixels(const std::vector<uint8_t>& rgba){
        size_t count = 0;
        for (size_t i = 3; i < rgba.size(); i += 4)
            count += (rgba[i] > 0 && rgba[i] < 255) ? 1 : 0;
        return count;
    }

}

TEST(LunaSvgTest, RasterizeAtThreeSizesIsPremultiplied){
    const std::string svg = readFixture("common/icon.svg");
    ASSERT_FALSE(svg.empty());
    for (float scale : {1.0f, 2.5f, 10.0f}){
        std::vector<uint8_t> pixels;
        int width = 0, height = 0;
        ASSERT_TRUE(LunaSVGRuntime::rasterize(svg.data(), svg.size(), scale, true, pixels, width, height));
        const int expected = (int)std::ceil(24.0f * scale);
        EXPECT_EQ(width, expected);
        EXPECT_EQ(height, expected);
        ASSERT_EQ(pixels.size(), (size_t)width * height * 4);
        EXPECT_TRUE(isPremultiplied(pixels)) << "scale " << scale;
        EXPECT_GT(partialPixels(pixels), 0u) << "scale " << scale;
        // the centre is the white circle, a corner of the rounded square is transparent
        const uint8_t* centre = &pixels[((size_t)(height / 2) * width + width / 2) * 4];
        EXPECT_EQ(centre[0], 255);
        EXPECT_EQ(centre[3], 255);
        EXPECT_EQ(pixels[3], 0);
    }
}

TEST(LunaSvgTest, StraightAlphaOutput){
    const std::string svg = readFixture("common/icon.svg");
    std::vector<uint8_t> pixels;
    int width = 0, height = 0;
    ASSERT_TRUE(LunaSVGRuntime::rasterize(svg.data(), svg.size(), 3.0f, false, pixels, width, height));
    // the antialiased edge of the white circle on blue keeps full white over partial blue...
    // and the rounded square's outer edge keeps its colour at partial alpha
    bool straightEdge = false;
    for (size_t i = 0; i < pixels.size(); i += 4){
        if (pixels[i + 3] > 0 && pixels[i + 3] < 200 && pixels[i + 2] > pixels[i + 3])
            straightEdge = true;
    }
    EXPECT_TRUE(straightEdge);
}

TEST(LunaSvgTest, InvalidInput){
    TestSystem::get().clearLogs();
    std::vector<uint8_t> pixels;
    int width = 0, height = 0;
    EXPECT_FALSE(LunaSVGRuntime::rasterize(nullptr, 0, 1.0f, true, pixels, width, height));
    const std::string broken = "<svg xmlns='http://www.w3.org/2000/svg' width='0' height='0'/>";
    EXPECT_FALSE(LunaSVGRuntime::rasterize(broken.data(), broken.size(), 1.0f, true, pixels, width, height));
    const std::string notSvg = "hello";
    EXPECT_FALSE(LunaSVGRuntime::rasterize(notSvg.data(), notSvg.size(), 1.0f, true, pixels, width, height));
    // over the cap
    const std::string svg = readFixture("common/icon.svg");
    EXPECT_FALSE(LunaSVGRuntime::rasterize(svg.data(), svg.size(), 1000.0f, true, pixels, width, height));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "SVG rasterization size too large"), 1u);
    // a non-positive or non-finite scale is the intrinsic size
    ASSERT_TRUE(LunaSVGRuntime::rasterize(svg.data(), svg.size(), -2.0f, true, pixels, width, height));
    EXPECT_EQ(width, 24);
    ASSERT_TRUE(LunaSVGRuntime::rasterize(svg.data(), svg.size(), NAN, true, pixels, width, height));
    EXPECT_EQ(width, 24);
}

TEST(LunaSvgTest, TextUsesTheBuiltInFonts){
    // LunaSVG never scans system fonts: <text> works because the engine registered its own
    const std::string svg =
        "<svg xmlns='http://www.w3.org/2000/svg' width='120' height='40'>"
        "<text x='4' y='30' font-family='sans-serif' font-size='28' fill='#000'>Abc</text></svg>";
    const std::string unknownFamily =
        "<svg xmlns='http://www.w3.org/2000/svg' width='120' height='40'>"
        "<text x='4' y='30' font-size='28' fill='#000'>Abc</text></svg>";
    for (const std::string& source : {svg, unknownFamily}){
        std::vector<uint8_t> pixels;
        int width = 0, height = 0;
        ASSERT_TRUE(LunaSVGRuntime::rasterize(source.data(), source.size(), 1.0f, true, pixels, width, height));
        size_t covered = 0;
        for (size_t i = 3; i < pixels.size(); i += 4)
            covered += pixels[i] > 128 ? 1 : 0;
        EXPECT_GT(covered, 100u);
    }
}

// The nanosvg-era references: size and alpha-weighted mean colour (straight alpha) of each
// fixture at svgScale 1 and 3, measured with the engine's nanosvg copy (TextureData before
// phase 1) on the same files. LunaSVG must keep the sizes and stay within 2/255.
TEST(TextureDataSvgTest, SameSizesAndColoursAsNanosvg){
    struct Reference {
        const char* file;
        float scale;
        int width, height;
        double r, g, b, a;
        double tolerance;
    };
    const Reference references[] = {
        {"common/icon.svg", 1, 24, 24, 95.65, 141.36, 209.91, 170.38, 2},
        {"common/icon.svg", 3, 72, 72, 96.87, 142.24, 210.29, 170.85, 2},
        // nanosvg's gradient ramp is off by ~3/255 at 64 px (LunaSVG gives the exact 128/127)
        {"p1/texturedata/gradient.svg", 1, 64, 32, 129.97, 0.00, 124.01, 250.77, 3.5},
        {"p1/texturedata/gradient.svg", 3, 192, 96, 128.66, 0.00, 125.34, 251.06, 2},
        {"p1/texturedata/opacity.svg", 1, 48, 48, 49.24, 138.08, 49.24, 112.99, 2},
        {"p1/texturedata/opacity.svg", 3, 144, 144, 49.73, 138.18, 49.73, 113.20, 2},
        {"p1/texturedata/stroke.svg", 1, 24, 24, 31.78, 31.78, 31.78, 76.69, 2},
        {"p1/texturedata/stroke.svg", 3, 72, 72, 31.93, 31.93, 31.93, 77.28, 2},
        // 30pt x 15pt = 40 x 20 CSS px
        {"p1/texturedata/units.svg", 1, 40, 20, 209.29, 146.85, 46.15, 255.00, 2},
        {"p1/texturedata/units.svg", 3, 120, 60, 209.09, 146.09, 46.19, 255.00, 2},
    };

    for (const Reference& reference : references){
        TextureData texture;
        const std::string path = TextureData::buildSvgScalePath(reference.file, reference.scale);
        ASSERT_TRUE(texture.loadTextureFromFile(path.c_str())) << path;
        EXPECT_EQ(texture.getWidth(), reference.width) << path;
        EXPECT_EQ(texture.getHeight(), reference.height) << path;
        EXPECT_EQ(texture.getOriginalWidth(), reference.width) << path;
        EXPECT_EQ(texture.getChannels(), 4) << path;
        EXPECT_EQ(texture.getColorFormat(), ColorFormat::RGBA) << path;
        EXPECT_EQ(texture.getSize(), (unsigned int)(reference.width * reference.height * 4)) << path;
        const MeanColour mean = meanColour(static_cast<const uint8_t*>(texture.getData()), texture.getWidth(), texture.getHeight());
        EXPECT_NEAR(mean.r, reference.r, reference.tolerance) << path;
        EXPECT_NEAR(mean.g, reference.g, reference.tolerance) << path;
        EXPECT_NEAR(mean.b, reference.b, reference.tolerance) << path;
        EXPECT_NEAR(mean.a, reference.a, reference.tolerance) << path;
    }
}

TEST(TextureDataSvgTest, StraightAlphaAndTransparency){
    TextureData texture;
    ASSERT_TRUE(texture.loadTextureFromFile("p1/texturedata/opacity.svg"));
    EXPECT_TRUE(texture.hasAlpha());
    const uint8_t* rgba = static_cast<const uint8_t*>(texture.getData());
    // the half-transparent green square: straight (0, 160, 0) at alpha 128
    const uint8_t* green = &rgba[(6 * 48 + 6) * 4];
    EXPECT_NEAR(green[1], 160, 1);
    EXPECT_NEAR(green[3], 128, 1);

    TextureData opaque;
    ASSERT_TRUE(opaque.loadTextureFromFile("p1/texturedata/units.svg"));
    EXPECT_FALSE(opaque.hasAlpha());
}

TEST(TextureDataSvgTest, Errors){
    TestSystem::get().clearLogs();
    TextureData tooLarge;
    EXPECT_FALSE(tooLarge.loadTextureFromFile("common/icon.svg?svgScale=1000"));
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "SVG rasterization size too large"), 1u);
    TextureData missing;
    EXPECT_FALSE(missing.loadTextureFromFile("p1/texturedata/missing.svg"));
}

// ---------------------------------------------------------------- RmlUi's SVG plugin

namespace {

    class SvgPluginTest : public ::testing::Test {
    protected:
        void SetUp() override{
            TestSystem::get().clearLogs();
            ASSERT_TRUE(HtmlUI::acquire());
            context = Rml::CreateContext("svg-plugin-test", Rml::Vector2i(512, 384), &recording);
            ASSERT_NE(context, nullptr);
        }

        void TearDown() override{
            context->UnloadAllDocuments();
            context->Update();
            Rml::RemoveContext("svg-plugin-test");
            Rml::ReleaseTextures(&recording);
            Rml::ReleaseRenderManagers();
            HtmlUI::release();
            EXPECT_EQ(recording.getLiveTextureCount(), 0u);
            EXPECT_EQ(recording.getLiveGeometryCount(), 0u);
        }

        Rml::ElementDocument* show(const std::string& body){
            const std::string rml = "<rml><head><style>body { display: block; width: 512px; height: 384px; } "
                                    "svg, div { display: block; }</style></head><body>" + body + "</body></rml>";
            Rml::ElementDocument* document = context->LoadDocumentFromMemory(rml, "p1/vector/plugin.rml");
            if (document)
                document->Show();
            context->Update();
            recording.beginFrame();
            context->Render();
            return document;
        }

        RecordingRenderInterface recording;
        Rml::Context* context = nullptr;
    };

}

TEST_F(SvgPluginTest, MissingFileWarns){
    Rml::ElementDocument* document = show("<svg src=\"nothing.svg\" style=\"width: 10px; height: 10px;\"/>");
    ASSERT_NE(document, nullptr);
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_WARN, "Could not load SVG file"), 1u);
}
