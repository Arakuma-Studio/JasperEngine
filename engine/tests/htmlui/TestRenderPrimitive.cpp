// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SDF primitives (S16 R8-R11, AC8): solid backgrounds, borders, outlines and box shadows drawn as
// HTMLUI_PRIM instances look like the meshes and shadow textures they replace (the default
// golden compare options), under transforms, inside filter, mask and opacity groups, at 1, 4 and 8
// samples; a box that does not qualify keeps the mesh path and says why; the quad-list
// classification and the instance layout.

#include "TestRenderCommon.h"

#include "HtmlUIPrimitive.h"

#include <RmlUi/Core/MeshUtilities.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const int width = 480;
    const int height = 300;

    struct Primitives {
        bool previous;
        explicit Primitives(bool on): previous(RmlRenderer::isSdfPrimitives()){
            RmlRenderer::setSdfPrimitivesForTesting(on);
        }
        ~Primitives(){
            RmlRenderer::setSdfPrimitivesForTesting(previous);
        }
    };

    std::string documentOf(const std::string& css, const std::string& body){
        return "<rml><head><style>body { margin: 0; } .box { position: absolute; width: 90px; height: 60px; background-color: #3060c0; } " +
               css + "</style></head><body>" + body + "</body></rml>";
    }

    struct Frame {
        std::vector<uint8_t> screen;
        HtmlUIRenderStats stats;
    };

    bool drawDocument(const std::string& rml, int samples, bool primitives, Frame& frame){
        Primitives mode(primitives);
        RenderTestContext test(width, height, 0, 0, samples);
        if (!test.isValid() || !test.loadFromMemory(rml, "render/primitives.rml") || !test.renderUntilDrawn())
            return false;
        frame.stats = test.renderer.getStats();
        return test.readScreen(frame.screen);
    }

    // the golden metric (ImageCompare's defaults) between two images
    ::testing::AssertionResult similar(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const std::string& label){
        const CompareOptions options;
        if (a.size() != b.size() || a.empty())
            return ::testing::AssertionFailure() << label << ": sizes differ";
        size_t bad = 0;
        double sum = 0.0;
        int largest = 0;
        const size_t pixels = a.size() / 4;
        for (size_t i = 0; i < pixels; i++){
            int difference = 0;
            for (int c = 0; c < 4; c++)
                difference = std::max(difference, std::abs((int)a[i * 4 + c] - (int)b[i * 4 + c]));
            sum += difference;
            largest = std::max(largest, difference);
            if (difference > options.pixelThreshold)
                bad++;
        }
        const double mean = sum / (double)pixels;
        const double badFraction = (double)bad / (double)pixels;
        if (badFraction <= options.maxBadFraction && mean <= options.maxMeanDiff)
            return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << label << ": " << bad << " bad pixels (" << badFraction * 100.0 << " %), mean " << mean
                                             << ", max " << largest;
    }

    struct Case {
        const char* name;
        std::string css;
        std::string body;
    };

    std::vector<Case> cases(){
        std::vector<Case> list;
        list.push_back({"borders",
                        ".a { left: 10px; top: 10px; border-width: 0px 1px 3px 7px; border-style: solid; border-color: #f04040 #40f040 #f0f040 #f040f0; }"
                        ".b { left: 130px; top: 10px; border: 3px solid #e0e0e0; border-radius: 30px / 15px; }"
                        ".c { left: 250px; top: 10px; border-width: 7px 3px 1px 0px; border-style: solid; border-color: #ffffff; border-radius: 12px 4px 20px 0px; }"
                        ".d { left: 370px; top: 10px; border: 1px solid #a0a0ff; border-radius: 8px; background-color: transparent; }",
                        "<div class=\"box a\"/><div class=\"box b\"/><div class=\"box c\"/><div class=\"box d\"/>"});
        list.push_back({"outlines",
                        ".a { left: 20px; top: 30px; outline: 3px solid #f0a020; outline-offset: 4px; border-radius: 10px; }"
                        ".b { left: 160px; top: 30px; outline: 2px auto #40a0ff; }"
                        ".c { left: 300px; top: 30px; outline: 1px solid #ffffff; outline-offset: -6px; }",
                        "<div class=\"box a\"/><div class=\"box b\"/><div class=\"box c\"/>"});
        list.push_back({"shadows",
                        ".a { left: 30px; top: 30px; border-radius: 8px; box-shadow: 0px 4px 10px rgba(0, 0, 0, 0.8); }"
                        ".b { left: 190px; top: 30px; border-radius: 12px; box-shadow: inset 0px 0px 8px 3px #f04040; }"
                        ".c { left: 340px; top: 30px; box-shadow: 8px 8px 0px 4px #40c040; }"
                        ".d { left: 120px; top: 170px; border-radius: 20px; border: 2px solid #ffffff;"
                        " box-shadow: 0px 0px 0px 6px #f0f040, 6px 6px 16px #000000, inset 4px 4px 6px #40f0f0; }",
                        "<div class=\"box a\"/><div class=\"box b\"/><div class=\"box c\"/><div class=\"box d\"/>"});
        list.push_back({"transforms",
                        ".a { left: 40px; top: 40px; border: 3px solid #ffffff; border-radius: 14px; transform: rotate(12deg); box-shadow: 0px 3px 6px #000000; }"
                        ".b { left: 200px; top: 50px; border: 2px solid #f0c040; border-radius: 10px; transform: scale(1.5); }"
                        ".c { left: 340px; top: 60px; border: 4px solid #40f0c0; border-radius: 16px; transform: perspective(300px) rotateY(30deg); }",
                        "<div class=\"box a\"/><div class=\"box b\"/><div class=\"box c\"/>"});
        list.push_back({"groups",
                        ".a { left: 30px; top: 40px; border: 3px solid #ffffff; border-radius: 12px; filter: blur(2px); }"
                        ".b { left: 180px; top: 40px; border: 3px solid #f0f040; border-radius: 12px; mask-image: linear-gradient(to right, #000000, transparent); }"
                        ".c { left: 330px; top: 40px; border: 3px solid #f04040; border-radius: 12px; opacity: 0.5; box-shadow: 0px 0px 0px 3px #40f040; }",
                        "<div class=\"box a\"/><div class=\"box b\"/><div class=\"box c\"/>"});
        return list;
    }

}

TEST(RenderPrimitive, MatchesTheMeshPath){
    JASPER_REQUIRE_GPU();
    for (const Case& test : cases()){
        const std::string rml = documentOf(test.css, test.body);
        for (int samples : {1, 4, 8}){
            Frame meshes, primitives;
            ASSERT_TRUE(drawDocument(rml, samples, false, meshes)) << test.name;
            ASSERT_TRUE(drawDocument(rml, samples, true, primitives)) << test.name;
            const std::string label = std::string(test.name) + " at " + std::to_string(samples) + " samples";
            EXPECT_TRUE(similar(primitives.screen, meshes.screen, label));
            EXPECT_GT(primitives.stats.instancesDrawn, 0u) << label;
        }
    }
}

TEST(RenderPrimitive, ShadowsMakeNoTextures){
    JASPER_REQUIRE_GPU();
    // blur, spread, inset, several per box: no saved layer, no blur pass
    const Case shadows = cases()[2];
    Frame meshes, primitives;
    ASSERT_TRUE(drawDocument(documentOf(shadows.css, shadows.body), 4, false, meshes));
    ASSERT_TRUE(drawDocument(documentOf(shadows.css, shadows.body), 4, true, primitives));
    EXPECT_GT(meshes.stats.boxShadowTextures, 0u);
    EXPECT_EQ(primitives.stats.boxShadowTextures, 0u);
    EXPECT_EQ(primitives.stats.layersPushed, 0u);
    EXPECT_EQ(primitives.stats.filtersApplied, 0u);
}

TEST(RenderPrimitive, NonQualifyingBoxesKeepTheMeshPath){
    JASPER_REQUIRE_GPU();
    Primitives on(true);
    RenderTestContext test(width, height);
    ASSERT_TRUE(test.isValid());
    Rml::ElementDocument* document = test.loadFromMemory(
        documentOf(".solid { left: 10px; top: 10px; border: 2px solid #ffffff; border-radius: 6px; }"
                   ".dashed { left: 130px; top: 10px; border: 2px dashed #ffffff; }"
                   ".gradient { left: 250px; top: 10px; decorator: linear-gradient(to right, #ff0000, #0000ff); }"
                   ".corner { left: 370px; top: 10px; border-width: 3px; border-style: solid; border-color: #ff0000 #00ff00 #0000ff #ffff00; border-radius: 10px; }",
                   "<div id=\"solid\" class=\"box solid\"/><div id=\"dashed\" class=\"box dashed\"/>"
                   "<div id=\"gradient\" class=\"box gradient\"/><div id=\"corner\" class=\"box corner\"/>"),
        "render/primitives.rml");
    ASSERT_NE(document, nullptr);
    ASSERT_TRUE(test.renderUntilDrawn());
    const char* reason = nullptr;
    EXPECT_TRUE(document->GetElementById("solid")->IsPaintedAsPrimitives(&reason));
    EXPECT_STREQ(reason, "");
    EXPECT_FALSE(document->GetElementById("dashed")->IsPaintedAsPrimitives(&reason));
    EXPECT_STREQ(reason, "border-style");
    EXPECT_FALSE(document->GetElementById("corner")->IsPaintedAsPrimitives(&reason));
    EXPECT_STREQ(reason, "border colours on a curve");
    // the background colour is a primitive, the decorator its mesh above it
    EXPECT_TRUE(document->GetElementById("gradient")->IsPaintedAsPrimitives(&reason));
}

TEST(RenderPrimitive, QuadListClassification){
    Rml::Mesh quads;
    Rml::MeshUtilities::GenerateQuad(quads, Rml::Vector2f(10, 20), Rml::Vector2f(30, 40), Rml::ColourbPremultiplied(200, 100, 50, 255),
                                     Rml::Vector2f(0.25f, 0.5f), Rml::Vector2f(0.75f, 1.0f));
    Rml::MeshUtilities::GenerateQuad(quads, Rml::Vector2f(50, 20), Rml::Vector2f(10, 10), Rml::ColourbPremultiplied(0, 0, 0, 128));
    std::vector<HtmlUIPrimitive> instances;
    ASSERT_TRUE(RmlRenderer::classifyQuadList(quads.vertices, quads.indices, &instances));
    ASSERT_EQ(instances.size(), 2u);
    const HtmlUIPrimitive& first = instances[0];
    EXPECT_EQ(first.params[0], (float)HtmlUIPrimitive::QUAD);
    EXPECT_FLOAT_EQ(first.a[0], 10.0f);
    EXPECT_FLOAT_EQ(first.a[1], 20.0f);
    EXPECT_FLOAT_EQ(first.b[0], 40.0f);
    EXPECT_FLOAT_EQ(first.b[1], 60.0f);
    EXPECT_FLOAT_EQ(first.d[0], 0.25f);
    EXPECT_FLOAT_EQ(first.d[3], 1.0f);
    EXPECT_EQ(first.color[0], 200);
    EXPECT_EQ(instances[1].color[3], 128);

    // a sheared quad (synthetic italic) is still a parallelogram
    Rml::Mesh italic = quads;
    for (int i = 0; i < 4; i++)
        italic.vertices[(size_t)i].position.x += (60.0f - italic.vertices[(size_t)i].position.y) * 0.21f;
    EXPECT_TRUE(RmlRenderer::classifyQuadList(italic.vertices, italic.indices, nullptr));

    // not quads: another index order, a trapezoid, two colours, uvs not affine
    Rml::Mesh reordered = quads;
    std::swap(reordered.indices[1], reordered.indices[2]);
    EXPECT_FALSE(RmlRenderer::classifyQuadList(reordered.vertices, reordered.indices, nullptr));
    Rml::Mesh trapezoid = quads;
    trapezoid.vertices[1].position.x += 3.0f;
    EXPECT_FALSE(RmlRenderer::classifyQuadList(trapezoid.vertices, trapezoid.indices, nullptr));
    Rml::Mesh gradient = quads;
    gradient.vertices[2].colour = Rml::ColourbPremultiplied(0, 255, 0, 255);
    EXPECT_FALSE(RmlRenderer::classifyQuadList(gradient.vertices, gradient.indices, nullptr));
    Rml::Mesh rotatedUv = quads;
    std::swap(rotatedUv.vertices[1].tex_coord, rotatedUv.vertices[3].tex_coord);
    EXPECT_FALSE(RmlRenderer::classifyQuadList(rotatedUv.vertices, rotatedUv.indices, nullptr));
    Rml::Mesh triangle;
    triangle.vertices = {quads.vertices[0], quads.vertices[1], quads.vertices[2]};
    triangle.indices = {0, 1, 2};
    EXPECT_FALSE(RmlRenderer::classifyQuadList(triangle.vertices, triangle.indices, nullptr));
}

TEST(RenderPrimitive, BoxInstanceLayout){
    Rml::BoxPrimitive box;
    box.border_box = Rml::Rectanglef::FromPositionSize(Rml::Vector2f(5, 6), Rml::Vector2f(100, 50));
    box.radii = {Rml::Vector2f(8, 4), Rml::Vector2f(0, 4), Rml::Vector2f(6, 6), Rml::Vector2f(2, 0)};
    box.border_widths = Rml::Vector4f(1, 2, 3, 4);
    box.fill_insets = Rml::Vector4f(1, 1, 1, 0);
    box.fill = Rml::ColourbPremultiplied(10, 20, 30, 255);
    box.border_colors = {Rml::ColourbPremultiplied(255, 0, 0, 255), Rml::ColourbPremultiplied(0, 255, 0, 255),
                         Rml::ColourbPremultiplied(0, 0, 255, 255), Rml::ColourbPremultiplied(255, 255, 0, 255)};
    const HtmlUIPrimitive instance = RmlRenderer::toInstance(box, Rml::Vector2f(10, 20));
    EXPECT_EQ(instance.params[0], (float)HtmlUIPrimitive::BOX);
    EXPECT_FLOAT_EQ(instance.a[0], 15.0f);
    EXPECT_FLOAT_EQ(instance.a[1], 26.0f);
    EXPECT_FLOAT_EQ(instance.a[2], 100.0f);
    // a corner with one radius 0 is square in both axes
    EXPECT_FLOAT_EQ(instance.b[0], 8.0f);
    EXPECT_FLOAT_EQ(instance.c[0], 4.0f);
    EXPECT_FLOAT_EQ(instance.b[1], 0.0f);
    EXPECT_FLOAT_EQ(instance.c[1], 0.0f);
    EXPECT_FLOAT_EQ(instance.b[3], 0.0f);
    EXPECT_FLOAT_EQ(instance.d[3], 4.0f);
    EXPECT_FLOAT_EQ(instance.e[2], 1.0f);
    EXPECT_EQ(instance.border[2][2], 255);

    Rml::BoxPrimitive shadow;
    shadow.kind = Rml::BoxPrimitive::Kind::ShadowOuter;
    shadow.border_box = Rml::Rectanglef::FromPositionSize(Rml::Vector2f(0, 4), Rml::Vector2f(50, 50));
    shadow.sigma = 3.0f;
    shadow.clip_box = Rml::Rectanglef::FromPositionSize(Rml::Vector2f(0, 0), Rml::Vector2f(50, 50));
    const HtmlUIPrimitive shadowInstance = RmlRenderer::toInstance(shadow, Rml::Vector2f(1, 1));
    EXPECT_EQ(shadowInstance.params[0], (float)HtmlUIPrimitive::SHADOW_OUTER);
    EXPECT_FLOAT_EQ(shadowInstance.params[1], 3.0f);
    EXPECT_FLOAT_EQ(shadowInstance.a[1], 5.0f);
    EXPECT_FLOAT_EQ(shadowInstance.d[1], 1.0f);
}
