// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// SDF primitives (S16 R8-R11, AC8): solid backgrounds, borders, outlines and box shadows drawn as
// HTMLUI_PRIM instances look like the meshes and shadow textures they replace (the default
// golden compare options), under transforms, inside filter, mask and opacity groups, at 1, 4 and 8
// samples; a box that does not qualify keeps the mesh path and says why; the quad-list
// classification and the instance layout.

#include "TestRenderCommon.h"

#include "HtmlUIPrimitive.h"
#include "render/FramebufferRender.h"
#include "render/SystemRender.h"

#include <RmlUi/Core/MeshUtilities.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
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
        return "<rml><head><style>body { margin: 0; } div { display: block; } .box { position: absolute; width: 90px; height: 60px; background-color: #3060c0; } " +
               css + "</style></head><body>" + body + "</body></rml>";
    }

    struct Frame {
        std::vector<uint8_t> screen;
        HtmlUIRenderStats stats;
        int samples = 0;
    };

    bool drawDocument(const std::string& rml, int samples, bool primitives, Frame& frame){
        Primitives mode(primitives);
        RenderTestContext test(width, height, 0, 0, samples);
        if (!test.isValid() || !test.loadFromMemory(rml, "render/primitives.rml") || !test.renderUntilDrawn())
            return false;
        frame.stats = test.renderer.getStats();
        frame.samples = test.target.getSampleCount();
        return test.readScreen(frame.screen);
    }

    // the golden metric (ImageCompare's defaults) between two images
    ::testing::AssertionResult similar(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const std::string& label){
        // the default threshold and mean; up to 0.5 % of the pixels apart (default 0.05 %): the
        // edge pixels, where the meshes' feathering, polygonal curves and aliased shadow textures
        // differ from the exact coverage (spec Deviations, AC8)
        CompareOptions options;
        options.maxBadFraction = 0.005;
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
        // CSS background gradients (S16-F5): a rounded one tile, a bordered rounded grid of repeated
        // tiles, small repeated tiles of a square box, an RCSS decorator, one under a transform
        list.push_back({"gradients",
                        ".a { left: 10px; top: 10px; background-image: linear-gradient(to right, #ff4040, #4040ff); border-radius: 12px; }"
                        ".b { left: 130px; top: 10px; background-image: radial-gradient(circle at 30% 40%, #ffffff, #208020 70%);"
                        " border: 4px solid #e0e0e080; border-radius: 16px; }"
                        ".c { left: 250px; top: 10px; background-image: linear-gradient(45deg, #f0c040, transparent); background-size: 30px 20px; }"
                        ".d { left: 370px; top: 10px; decorator: linear-gradient(180deg, #40f0c0, #102040); }"
                        ".e { left: 40px; top: 130px; background-image: linear-gradient(90deg, #ff0000 50%, #00ff00 50%); border-radius: 10px;"
                        " transform: rotate(10deg); }"
                        ".f { left: 200px; top: 130px; background-image: linear-gradient(to bottom, #ffffff, #000000); background-size: 40px 40px;"
                        " border-radius: 14px; }",
                        "<div class=\"box a\"/><div class=\"box b\"/><div class=\"box c\"/><div class=\"box d\"/><div class=\"box e\"/>"
                        "<div class=\"box f\"/>"});
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
    // the statistics of a replayed frame: S92 R1 keeps an unchanged one (no pass, no draw)
    FrameReuseMode replayed(false);
    // the layers of the mesh path in the frame compared, not drawn from the layer cache (S92 R9)
    LayerCacheOff cacheOff;
    for (const Case& test : cases()){
        const std::string rml = documentOf(test.css, test.body);
        for (int samples : {1, 4, 8}){
            const std::string label = std::string(test.name) + " at " + std::to_string(samples) + " samples";
            // a device without targets of this sample count (llvmpipe and lavapipe have 1 and 4)
            // gets the count below it: that was compared already
            if (FramebufferRender::getMaxMsaaSamples(samples) != samples){
                std::printf("[ skipped  ] %s: the device has no such targets\n", label.c_str());
                continue;
            }
            Frame meshes, primitives;
            ASSERT_TRUE(drawDocument(rml, samples, false, meshes)) << test.name;
            ASSERT_TRUE(drawDocument(rml, samples, true, primitives)) << test.name;
            EXPECT_EQ(primitives.samples, samples) << label;
            const ::testing::AssertionResult result = similar(primitives.screen, meshes.screen, label);
            EXPECT_TRUE(result);
            if (!result){
                // (the render state an earlier test may have left: GPU timing, pipelines)
                const GpuTimingState timing = SystemRender::getGpuTimingState();
                std::printf("[ diagnose ] %s: gpu timing available %d enabled %d (%s), pipeline cache %zu, instances %u\n", label.c_str(),
                            timing.available ? 1 : 0, timing.enabled ? 1 : 0, timing.reason.c_str(), (size_t)SystemRender::getPipelineCacheSize(),
                            primitives.stats.instancesDrawn);
                const std::string base = getOutputDirectory() + "/primitives-" + test.name + "-" + std::to_string(samples);
                writePng(base + ".sdf.png", primitives.screen, width, height);
                writePng(base + ".mesh.png", meshes.screen, width, height);
            }
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

TEST(RenderPrimitive, GradientBackgroundsNeedNoMaskLayer){
    JASPER_REQUIRE_GPU();
    // the statistics of a replayed frame: S92 R1 keeps an unchanged one (no pass, no draw)
    FrameReuseMode replayed(false);
    // the layers of the mesh path in the frame compared, not drawn from the layer cache (S92 R9)
    LayerCacheOff cacheOff;
    // a rounded box's CSS gradient: the mesh path masks it in a layer, the primitive clips itself
    // (S16-F5); the gradients are instances, no shader draw is left
    const std::string rml = documentOf(".g { left: 20px; top: 20px; border-radius: 12px; border: 2px solid #ffffff;"
                                       " background-image: linear-gradient(to right, #ff4040, #4040ff); }",
                                       "<div class=\"box g\"/>");
    Frame meshes, primitives;
    ASSERT_TRUE(drawDocument(rml, 4, false, meshes));
    ASSERT_TRUE(drawDocument(rml, 4, true, primitives));
    EXPECT_GT(meshes.stats.layersPushed, 0u);
    EXPECT_EQ(primitives.stats.layersPushed, 0u);
    EXPECT_GT(primitives.stats.instancesDrawn, 0u);
    EXPECT_TRUE(similar(primitives.screen, meshes.screen, "rounded gradient"));
}

// a gradient alone (no background colour under it), as RCSS decorator and as CSS image: its ends
// in their colours where they are (the MCP gallery's probes)
TEST(RenderPrimitive, GradientsDrawWhereTheyAre){
    JASPER_REQUIRE_GPU();
    for (const char* paint : {"decorator: linear-gradient(90deg, #ff0000, #0000ff);", "background-image: linear-gradient(90deg, #ff0000, #0000ff);"}){
        const std::string rml = documentOf(std::string("#g { position: absolute; left: 40px; top: 40px; width: 300px; height: 100px; ") + paint + " }",
                                           "<div id=\"g\"/>");
        for (bool on : {false, true}){
            Frame frame;
            ASSERT_TRUE(drawDocument(rml, 4, on, frame)) << paint;
            auto at = [&](int x, int y){ return frame.screen.data() + ((size_t)y * width + (size_t)x) * 4; };
            const uint8_t* start = at(44, 90);
            const uint8_t* end = at(336, 90);
            EXPECT_GT(start[0], 230) << paint << (on ? " primitives" : " mesh") << ": " << int(start[0]) << " " << int(start[1]) << " " << int(start[2]);
            EXPECT_LT(start[2], 30) << paint << (on ? " primitives" : " mesh");
            EXPECT_GT(end[2], 230) << paint << (on ? " primitives" : " mesh") << ": " << int(end[0]) << " " << int(end[1]) << " " << int(end[2]);
            EXPECT_LT(end[0], 30) << paint << (on ? " primitives" : " mesh");
        }
    }
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

// S92 R7 (AC7): gradients beyond S16-F5's four linear or radial stops are primitives too: up to 16
// stops (px, %, hard and transparent ones) through the renderer's stop table, conic and repeating
// gradients, and every background layer of a rounded box whose tiles are one tile or a grid along
// each axis. They push no layer and no mask, the frame is one pass, and they look like the mesh
// path they replace (the default golden metric of MatchesTheMeshPath)
namespace {

    void expectLayerFreeGradients(const char* label, const std::string& css, const std::string& body){
        const std::string rml = documentOf(css, body);
        for (int samples : {1, 4}){
            const std::string at = std::string(label) + " at " + std::to_string(samples) + " samples";
            Frame meshes, primitives;
            ASSERT_TRUE(drawDocument(rml, samples, false, meshes)) << at;
            ASSERT_TRUE(drawDocument(rml, samples, true, primitives)) << at;
            EXPECT_EQ(primitives.stats.layersPushed, 0u) << at;
            EXPECT_EQ(primitives.stats.passes, 1u) << at;
            EXPECT_GT(primitives.stats.instancesDrawn, 0u) << at;
            const ::testing::AssertionResult result = similar(primitives.screen, meshes.screen, at);
            EXPECT_TRUE(result);
            if (!result){
                const std::string base = getOutputDirectory() + "/primitives-" + label + "-" + std::to_string(samples);
                writePng(base + ".sdf.png", primitives.screen, width, height);
                writePng(base + ".mesh.png", meshes.screen, width, height);
            }
        }
    }

}

TEST(RenderPrimitive, GradientsWithManyStops){
    JASPER_REQUIRE_GPU();
    // the layers of the mesh path in the frame compared, not drawn from the layer cache (S92 R9)
    LayerCacheOff cacheOff;
    // the port's button body (7 stops, hard px stops), 16 stops, hard and transparent stops of a
    // radial gradient, a decorator of 9 stops
    expectLayerFreeGradients("many-stops",
                             ".a { left: 10px; top: 10px; border-radius: 14px; border: 2px solid #ffffff;"
                             " background-image: linear-gradient(180deg, #fff3c0 0px, #ffd060 8px, #ffb000 8px, #ff8a00 30px, #e06000 45px, #c83c0080 52px, transparent 60px); }"
                             ".b { left: 130px; top: 10px; border-radius: 20px; background-image: linear-gradient(90deg, #ff0000 0%, #ff6000 7%, #ffc000 13%,"
                             " #c0ff00 20%, #60ff00 27%, #00ff00 33%, #00ff60 40%, #00ffc0 47%, #00c0ff 53%, #0060ff 60%, #0000ff 67%, #6000ff 73%,"
                             " #c000ff 80%, #ff00c0 87%, #ff0060 93%, #ffffff 100%); }"
                             ".c { left: 250px; top: 10px; border-radius: 30px; background-image: radial-gradient(circle at 40% 40%, #ffffff 0px, #ffffff 6px,"
                             " #f04040 6px, #f04040 14px, transparent 14px, transparent 20px, #4040f0 20px, #102040 40px); }"
                             ".d { left: 370px; top: 10px; decorator: linear-gradient(0deg, #101010, #303030 10%, #505050 20%, #707070 30%, #909090 40%,"
                             " #b0b0b0 50%, #d0d0d0 60%, #f0f0f0 70%, #ffffff); }"
                             ".e { left: 40px; top: 130px; border-radius: 10px; transform: rotate(10deg); background-image: linear-gradient(45deg, #ff0000,"
                             " #00ff00 20%, #0000ff 40%, #ffff00 60%, #00ffff 80%, #ff00ff); }",
                             "<div class=\"box a\"/><div class=\"box b\"/><div class=\"box c\"/><div class=\"box d\"/><div class=\"box e\"/>");

    // more than 16 stops keep the mesh path and its mask layer
    std::string seventeen = "linear-gradient(90deg";
    for (int i = 0; i < 17; i++)
        seventeen += std::string(", ") + ((i % 2) ? "#ff0000 " : "#0000ff ") + std::to_string(i * 6) + "%";
    seventeen += ")";
    Frame frame;
    ASSERT_TRUE(drawDocument(documentOf(".f { left: 10px; top: 10px; border-radius: 14px; background-image: " + seventeen + "; }", "<div class=\"box f\"/>"),
                             4, true, frame));
    EXPECT_GT(frame.stats.layersPushed, 0u);
}

TEST(RenderPrimitive, ConicAndRepeating){
    JASPER_REQUIRE_GPU();
    expectLayerFreeGradients("conic-repeating",
                             ".a { left: 10px; top: 10px; border-radius: 16px; background-image: conic-gradient(from 45deg at 40% 60%, #ff0000, #ffff00,"
                             " #00ff00, #00ffff, #0000ff, #ff00ff, #ff0000); }"
                             ".b { left: 130px; top: 10px; border-radius: 12px; background-image: repeating-linear-gradient(45deg, #cc3333 0px, #cc3333 6px,"
                             " #ffffff 6px, #ffffff 12px); }"
                             ".c { left: 250px; top: 10px; border-radius: 20px; background-image: repeating-radial-gradient(circle at 30% 40%, #3366cc 0px,"
                             " #3366cc 5px, #f4c430 5px, #f4c430 10px); }"
                             ".d { left: 370px; top: 10px; border-radius: 45px / 30px; background-image: repeating-conic-gradient(#222222 0deg, #222222 15deg,"
                             " #eeeeee 15deg, #eeeeee 30deg); }"
                             ".e { left: 40px; top: 130px; decorator: conic-gradient(from 90deg, #cc3333, #3366cc, #cc3333); }"
                             ".f { left: 160px; top: 130px; border-radius: 8px; decorator: repeating-linear-gradient(90deg, #40c040 0px, #40c040 4px,"
                             " #202020 4px, #202020 9px); }",
                             "<div class=\"box a\"/><div class=\"box b\"/><div class=\"box c\"/><div class=\"box d\"/><div class=\"box e\"/>"
                             "<div class=\"box f\"/>");
}

TEST(RenderPrimitive, EveryLayerOfARoundedBox){
    JASPER_REQUIRE_GPU();
    // four layers on a rounded, bordered box: a tile drawn once in the middle, a strip repeated
    // along x only, a grid of dots, one over the whole box; and a tile alone in a corner
    expectLayerFreeGradients("rounded-layers",
                             ".a { left: 20px; top: 20px; width: 160px; height: 110px; border-radius: 22px; border: 3px solid #e0e0e0;"
                             " background-image: linear-gradient(#ff000080, #ff000080), linear-gradient(90deg, #00ff00, #0000ff),"
                             " radial-gradient(#ffffff 30%, transparent 32%), linear-gradient(180deg, #303030, #808080);"
                             " background-size: 40px 20px, 30px 12px, 16px 16px, auto;"
                             " background-position: center, 0px 10px, 0px 0px, 0px 0px;"
                             " background-repeat: no-repeat, repeat-x, repeat, repeat; }"
                             ".b { left: 240px; top: 20px; width: 160px; height: 110px; border-radius: 40px; background-color: #203040;"
                             " background-image: radial-gradient(circle, #f0c040, #f0c04000); background-size: 60px 60px;"
                             " background-position: 0px 0px; background-repeat: no-repeat; }",
                             "<div class=\"box a\"/><div class=\"box b\"/>");
}

TEST(RenderPrimitive, GradientInstanceLayout){
    // up to four stops in the instance, more in the shader's row of the stop table; the function
    // and the single tile (a negative period) as given
    RmlCompiledShader four;
    four.function = RmlCompiledShader::Function::REPEATING_CONIC;
    four.p = Rml::Vector2f(3, 4);
    four.v = Rml::Vector2f(1, 0);
    four.stopPositions = {0.0f, 0.25f, 0.5f, 1.0f};
    four.stopColors = {Rml::Colourf(1, 0, 0, 1), Rml::Colourf(0, 1, 0, 1), Rml::Colourf(0, 0, 1, 1), Rml::Colourf(1, 1, 1, 1)};
    Rml::BoxPrimitive gradient;
    gradient.kind = Rml::BoxPrimitive::Kind::Gradient;
    gradient.border_box = Rml::Rectanglef::FromPositionSize(Rml::Vector2f(0, 0), Rml::Vector2f(80, 40));
    gradient.shader = reinterpret_cast<Rml::CompiledShaderHandle>(&four);
    gradient.gradient_repeat = Rml::Vector2f(-20, 16);
    HtmlUIPrimitive instance = RmlRenderer::toInstance(gradient, Rml::Vector2f(0, 0));
    EXPECT_EQ(instance.params[0], (float)HtmlUIPrimitive::GRADIENT);
    EXPECT_FLOAT_EQ(instance.f[0], (float)RmlCompiledShader::Function::REPEATING_CONIC);
    EXPECT_FLOAT_EQ(instance.f[1], 4.0f);
    EXPECT_FLOAT_EQ(instance.f[2], -20.0f);
    EXPECT_FLOAT_EQ(instance.f[3], 16.0f);
    EXPECT_FLOAT_EQ(instance.e[1], 0.25f);
    EXPECT_EQ(instance.border[1][1], 255);

    RmlCompiledShader seven = four;
    seven.stopPositions = {0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 1.0f};
    seven.stopColors.resize(7, Rml::Colourf(1, 1, 1, 1));
    seven.stopRow = 5;
    gradient.shader = reinterpret_cast<Rml::CompiledShaderHandle>(&seven);
    instance = RmlRenderer::toInstance(gradient, Rml::Vector2f(0, 0));
    EXPECT_FLOAT_EQ(instance.f[1], 7.0f);
    EXPECT_FLOAT_EQ(instance.e[0], 5.0f);
    EXPECT_EQ(instance.border[1][1], 0);
}
