// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// RmlRenderer's command recording (phase-1 spec 3.2.10): a frame recorded and replayed in endFrame
// is byte-identical to the same frame drawn call by call (layers, filters, masks, box shadows in
// one texture and in tiles, gradients, clip masks under transforms, backdrops, text, paint, filter,
// backdrop and stateful effects); geometry, textures, filters and saved layers released in the
// middle of a recorded frame stay alive until its replay; an empty frame; a context removed between
// its Render and endFrame.

#include "effects/EffectTestCommon.h"

#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>

#include <algorithm>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    // recording on or off for a scope (read by beginFrame)
    struct RecordingMode {
        bool previous;
        explicit RecordingMode(bool on): previous(RmlRenderer::isCommandRecording()){
            RmlRenderer::setCommandRecordingForTesting(on);
        }
        ~RecordingMode(){
            RmlRenderer::setCommandRecordingForTesting(previous);
        }
    };

    // S16: the faithful replay (no batching, no analytic clips) is what drawing at once matches
    // byte for byte; the batched replay's own equivalence is RenderBatch's (within 1)
    struct UnbatchedReplay {
        bool batching = RmlRenderer::isBatching();
        bool primitives = RmlRenderer::isSdfPrimitives();
        UnbatchedReplay(){
            RmlRenderer::setBatchingForTesting(false);
            RmlRenderer::setSdfPrimitivesForTesting(false);
        }
        ~UnbatchedReplay(){
            RmlRenderer::setBatchingForTesting(batching);
            RmlRenderer::setSdfPrimitivesForTesting(primitives);
        }
    };

    struct FixedClock {
        explicit FixedClock(double t){ EffectClock::setFixedTime(t); }
        ~FixedClock(){ EffectClock::setFixedTime(std::nullopt); }
    };

    // the first pixel two images differ at, for the failure message
    std::string firstDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int width){
        if (a.size() != b.size())
            return "sizes " + std::to_string(a.size()) + " and " + std::to_string(b.size());
        for (size_t i = 0; i < a.size(); i++){
            if (a[i] != b[i]){
                const size_t pixel = i / 4;
                return "first difference at (" + std::to_string(pixel % (size_t)width) + ", " + std::to_string(pixel / (size_t)width) +
                       "), channel " + std::to_string(i % 4) + ": " + std::to_string(a[i]) + " recorded, " + std::to_string(b[i]) + " immediate";
            }
        }
        return "identical";
    }

    // Layers, filters, masks, box shadows (one in a texture, one larger than the window in tiles),
    // gradients, a clip mask under transforms, a backdrop and text
    const char* const kLayersDocument = R"(<!DOCTYPE html>
<html><head><style>
html, body { margin: 0; width: 360px; height: 240px; overflow: hidden; background: #101218; font-family: sans-serif; }
.b { position: absolute; width: 60px; height: 40px; }
#big { left: 300px; top: 190px; width: 30px; height: 30px; background: #16a085; box-shadow: 0 0 12px 230px rgba(22, 160, 133, 0.3); }
#shadow { left: 10px; top: 10px; background: #e74c3c; box-shadow: 4px 6px 8px 2px rgba(0, 0, 0, 0.7); border-radius: 8px; }
#blur { left: 90px; top: 10px; background: #3498db; filter: blur(3px); }
#drop { left: 170px; top: 10px; background: #2ecc71; filter: drop-shadow(4px 4px 3px #000) grayscale(0.5); opacity: 0.8; }
#mask { left: 250px; top: 10px; background: #f1c40f; mask-image: linear-gradient(90deg, #000, transparent); }
#grad { left: 10px; top: 70px; background: linear-gradient(45deg, red, blue); }
#radial { left: 90px; top: 70px; background: radial-gradient(circle, #fff, #000); }
#conic { left: 170px; top: 70px; background: conic-gradient(red, yellow, green, red); border-radius: 20px; }
#clip { left: 250px; top: 70px; overflow: hidden; border-radius: 16px; transform: rotate(12deg); background: #8e44ad; }
#clip div { width: 90px; height: 20px; margin: 10px -10px; background: #ecf0f1; transform: rotate(-30deg); }
#glass { left: 40px; top: 130px; width: 200px; height: 50px; backdrop-filter: blur(4px); background: rgba(255, 255, 255, 0.1); border-radius: 12px; }
#text { position: absolute; left: 20px; top: 196px; color: #fff; font-size: 20px; white-space: nowrap; }
</style></head><body>
<div class="b" id="big"></div>
<div class="b" id="shadow"></div><div class="b" id="blur"></div><div class="b" id="drop"></div><div class="b" id="mask"></div>
<div class="b" id="grad"></div><div class="b" id="radial"></div><div class="b" id="conic"></div>
<div class="b" id="clip"><div></div></div>
<div class="b" id="glass"></div>
<div id="text">Recorded 123</div>
</body></html>)";

    // Line-clamp ellipses the blocks draw themselves (after an inline-block: painted by the stacking context with the
    // block's transform, clip and text shadow, between the inline content and the positioned inline-block over it) and
    // one a text run draws
    const char* const kLineClampDocument = R"html(<!DOCTYPE html>
<html><head><style>
html, body { margin: 0; width: 360px; height: 240px; overflow: hidden; background: #101218; font-family: sans-serif; color: #fff; }
.c { position: absolute; display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 2; overflow: hidden; width: 140px;
     font-size: 16px; line-height: 20px; background: #2c3e50; color: #f39c12; text-shadow: 2px 2px 0 #c0392b; }
.t { color: #ecf0f1; }
.ib { display: inline-block; width: 24px; height: 14px; background: #27ae60; }
</style></head><body>
<div class="c" style="left: 10px; top: 10px"><span class="t">Two lines of text then</span> <span class="ib"></span> <span class="t">more text after</span></div>
<div class="c" style="left: 190px; top: 10px; transform: rotate(-6deg)"><span class="t">A turned block with</span> <span class="ib" style="position: relative; top: 4px"><span style="display: block; width: 40px; height: 8px; background: #8e44ad"></span></span> <span class="t">hidden words</span></div>
<div class="c" style="left: 10px; top: 120px; float: left"><span class="t">A text run draws this one at the end of the line</span></div>
<div class="c" style="left: 190px; top: 120px; opacity: 0.8"><span class="t">Translucent</span><br><br><span class="t">past the clamp</span></div>
</body></html>)html";

    // paint, filter, backdrop and stateful effects, one under a transform
    std::string effectsDocument(){
        std::string html = R"(<!DOCTYPE html>
<html><head><style>
html, body { margin: 0; width: 360px; height: 240px; overflow: hidden; background: #101218; font-family: sans-serif; }
.e { position: absolute; width: 100px; height: 70px; background-color: #34495e; }
#card { position: absolute; left: 0; top: 0; width: 360px; height: 240px; }
)";
        struct Placed {
            const char* name;
            int x, y;
            const char* extra;
        };
        const Placed placed[] = {
            {"stripes", 10, 10, ""}, {"spotlight", 130, 10, "transform: rotate(8deg);"}, {"pixelate", 250, 10, ""},
            {"frosted-glass", 10, 90, "background-color: transparent; border-radius: 12px;"}, {"particles", 130, 90, ""}, {"fog", 250, 90, ""},
        };
        std::string body;
        int index = 0;
        for (const Placed& p : placed){
            const EffectManifest* m = EffectRegistry::get().find(p.name);
            if (!m)
                continue;
            const std::string id = "e" + std::to_string(index++);
            html += "#" + id + " { left: " + std::to_string(p.x) + "px; top: " + std::to_string(p.y) + "px; " + p.extra + " " + declarationOf(*m) + "; }\n";
            body += "<div class=\"e\" id=\"" + id + "\"></div>";
        }
        html += "</style></head><body><img id=\"card\" src=\"/effects/testcard.png\">" + body +
                "<div style=\"position: absolute; left: 20px; top: 190px; color: #fff; font-size: 20px;\">Effects</div></body></html>";
        return html;
    }

    // the document captured with recording on or off, in a scene of its own
    TestImage captureWith(bool recording, const std::string& html, size_t* commands){
        RecordingMode mode(recording);
        TestImage image;
        EffectScene scene;
        if (!scene.valid())
            return image;
        std::string error;
        EXPECT_TRUE(scene.loadHtml(html, &error)) << error;
        EffectRuntime::get().clearTestInput();
        EXPECT_TRUE(scene.capture(image));
        if (commands && scene.fixture().renderer())
            *commands = scene.fixture().renderer()->getLastFrameCommandCount();
        return image;
    }

    void expectRecordingMatches(const std::string& html, const char* label){
        UnbatchedReplay unbatched;
        size_t recordedCommands = 0, immediateCommands = 0;
        const TestImage recorded = captureWith(true, html, &recordedCommands);
        const TestImage immediate = captureWith(false, html, &immediateCommands);
        ASSERT_FALSE(recorded.empty()) << label;
        ASSERT_FALSE(immediate.empty()) << label;
        EXPECT_GT(recordedCommands, 0u) << label << ": the frame was recorded";
        EXPECT_EQ(immediateCommands, 0u) << label << ": the frame was drawn at once";
        ASSERT_EQ(recorded.width, immediate.width);
        ASSERT_EQ(recorded.height, immediate.height);
        EXPECT_TRUE(recorded.rgba == immediate.rgba) << label << ": " << firstDifference(recorded.rgba, immediate.rgba, recorded.width);
    }

    // RenderTestContext frames drawn by hand
    struct Quad {
        Rml::Mesh mesh;
        Rml::CompiledGeometryHandle handle = 0;
        Quad(RmlRenderer& renderer, float x, float y, float w, float h, Rml::ColourbPremultiplied colour){
            Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(x, y), Rml::Vector2f(w, h), colour, Rml::Vector2f(0, 0), Rml::Vector2f(1, 1));
            handle = renderer.CompileGeometry(mesh.vertices, mesh.indices);
        }
        // as RmlUi: the handle goes, then the mesh, overwritten so a read of it shows
        void release(RmlRenderer& renderer){
            renderer.ReleaseGeometry(handle);
            handle = 0;
            for (Rml::Vertex& vertex : mesh.vertices){
                vertex.position = Rml::Vector2f(-12345.f, 98765.f);
                vertex.colour = Rml::ColourbPremultiplied(7, 7, 7, 7);
            }
            for (int& index : mesh.indices)
                index = 0x7fffffff;
            mesh = Rml::Mesh();
        }
    };

    // frames until the renderer's programs exist (an empty context)
    bool warmUp(RenderTestContext& test){
        return test.renderUntilDrawn();
    }

    std::vector<uint8_t> drawFrame(RenderTestContext& test, const std::function<void(RmlRenderer&)>& draw){
        beginTestFrame();
        EXPECT_TRUE(test.renderer.beginFrame(test.target, test.contextSize));
        draw(test.renderer);
        EXPECT_TRUE(test.renderer.endFrame());
        SystemRender::commit();
        std::vector<uint8_t> layer;
        EXPECT_TRUE(test.readLayer(layer));
        return layer;
    }

}

TEST(RenderRecordTest, RecordedFramesMatchImmediateOnes){
    TestSystem::get().clearLogs();
    {
        EffectScene probe;
        if (!probe.valid())
            GTEST_SKIP() << "no GPU";
    }
    for (const char* name : {"stripes", "spotlight", "pixelate", "frosted-glass", "particles", "fog"}){
        const EffectManifest* m = EffectRegistry::get().find(name);
        ASSERT_NE(m, nullptr) << name;
        ASSERT_TRUE(warmUpEffect(*m)) << name;
    }
    FixedClock clock(1.0);
    expectRecordingMatches(kLayersDocument, "layers");
    expectRecordingMatches(effectsDocument(), "effects");
    expectRecordingMatches(kLineClampDocument, "line-clamp");
    EffectRuntime::get().clearTestInput();
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST(RenderRecordTest, ResourcesReleasedMidFrameLiveUntilTheReplay){
    std::vector<uint8_t> layers[2];
    for (int recording = 1; recording >= 0; recording--){
        RecordingMode mode(recording != 0);
        RenderTestContext test(360, 240);
        if (!test.isValid())
            GTEST_SKIP() << "no GPU";
        ASSERT_TRUE(warmUp(test));
        const size_t geometryBefore = test.renderer.getLiveGeometryCount();
        const size_t texturesBefore = test.renderer.getLiveTextureCount();

        layers[recording] = drawFrame(test, [&](RmlRenderer& r){
            // a geometry drawn, then released with its mesh; one compiled after it is another
            Quad red(r, 10, 10, 40, 30, Rml::ColourbPremultiplied(255, 0, 0, 255));
            r.RenderGeometry(red.handle, Rml::Vector2f(0, 0), 0);
            const Rml::CompiledGeometryHandle redHandle = red.handle;
            red.release(r);
            Quad green(r, 60, 10, 40, 30, Rml::ColourbPremultiplied(0, 255, 0, 255));
            if (recording){
                EXPECT_NE(green.handle, redHandle) << "a geometry released while recording is not reused before the replay";
            }
            r.RenderGeometry(green.handle, Rml::Vector2f(0, 0), 0);
            green.release(r);

            // a texture released after its draw
            std::vector<Rml::byte> blue(8 * 8 * 4);
            for (size_t i = 0; i < blue.size(); i += 4){
                blue[i + 2] = 255;
                blue[i + 3] = 255;
            }
            const Rml::TextureHandle texture = r.GenerateTexture(blue, Rml::Vector2i(8, 8));
            ASSERT_NE(texture, 0u);
            Quad textured(r, 110, 10, 40, 30, Rml::ColourbPremultiplied(255, 255, 255, 255));
            r.RenderGeometry(textured.handle, Rml::Vector2f(0, 0), texture);
            r.ReleaseTexture(texture);
            textured.release(r);

            // a filter released after its composite (a box shadow's blur)
            Rml::Dictionary parameters;
            parameters["sigma"] = Rml::Variant(2.0f);
            const Rml::CompiledFilterHandle blur = r.CompileFilter("blur", parameters);
            ASSERT_NE(blur, 0u);
            const Rml::LayerHandle layer = r.PushLayer();
            EXPECT_EQ(layer, (Rml::LayerHandle)1);
            Quad blurred(r, 160, 10, 40, 30, Rml::ColourbPremultiplied(255, 255, 0, 255));
            r.RenderGeometry(blurred.handle, Rml::Vector2f(0, 0), 0);
            blurred.release(r);
            const Rml::CompiledFilterHandle filters[] = {blur};
            r.CompositeLayers(layer, 0, Rml::BlendMode::Blend, Rml::Span<const Rml::CompiledFilterHandle>(filters, 1));
            r.PopLayer();
            r.ReleaseFilter(blur);

            // a saved layer drawn elsewhere, then released
            r.PushLayer();
            r.EnableScissorRegion(true);
            r.SetScissorRegion(Rml::Rectanglei::FromPositionSize(Rml::Vector2i(220, 10), Rml::Vector2i(40, 30)));
            Quad saved(r, 220, 10, 40, 30, Rml::ColourbPremultiplied(0, 128, 255, 255));
            r.RenderGeometry(saved.handle, Rml::Vector2f(0, 0), 0);
            saved.release(r);
            const Rml::TextureHandle savedTexture = r.SaveLayerAsTexture();
            ASSERT_NE(savedTexture, 0u);
            r.PopLayer();
            r.EnableScissorRegion(false);
            Quad copy(r, 270, 10, 40, 30, Rml::ColourbPremultiplied(255, 255, 255, 255));
            r.RenderGeometry(copy.handle, Rml::Vector2f(0, 0), savedTexture);
            copy.release(r);
            r.ReleaseTexture(savedTexture);

            // a mask image released after its composite
            const Rml::LayerHandle masked = r.PushLayer();
            Quad content(r, 10, 60, 80, 40, Rml::ColourbPremultiplied(255, 0, 255, 255));
            r.RenderGeometry(content.handle, Rml::Vector2f(0, 0), 0);
            content.release(r);
            r.PushLayer();
            Quad maskShape(r, 10, 60, 40, 40, Rml::ColourbPremultiplied(255, 255, 255, 255));
            r.RenderGeometry(maskShape.handle, Rml::Vector2f(0, 0), 0);
            maskShape.release(r);
            const Rml::CompiledFilterHandle mask = r.SaveLayerAsMaskImage();
            ASSERT_NE(mask, 0u);
            r.PopLayer();
            const Rml::CompiledFilterHandle maskFilters[] = {mask};
            r.CompositeLayers(masked, 0, Rml::BlendMode::Blend, Rml::Span<const Rml::CompiledFilterHandle>(maskFilters, 1));
            r.PopLayer();
            r.ReleaseFilter(mask);
        });

        EXPECT_EQ(test.renderer.getLastFrameCommandCount() > 0, recording != 0);
        EXPECT_EQ(test.renderer.getLiveGeometryCount(), geometryBefore);
        EXPECT_EQ(test.renderer.getLiveTextureCount(), texturesBefore);
        ASSERT_FALSE(layers[recording].empty());
        const PixelProbe probe{layers[recording], 360};
        EXPECT_TRUE(probe.is(30, 25, 255, 0, 0)) << "the released geometry was drawn with its data";
        EXPECT_TRUE(probe.is(80, 25, 0, 255, 0));
        EXPECT_TRUE(probe.is(130, 25, 0, 0, 255)) << "the released texture was drawn";
        EXPECT_TRUE(probe.is(290, 25, 0, 128, 255)) << "the saved layer was filled before its draw";
        EXPECT_TRUE(probe.is(20, 80, 255, 0, 255)) << "masked in";
        EXPECT_TRUE(probe.is(70, 80, 0, 0, 0, 0)) << "masked out";
    }
    EXPECT_TRUE(layers[1] == layers[0]) << firstDifference(layers[1], layers[0], 360);
}

TEST(RenderRecordTest, AnEmptyFrameClearsTheLayer){
    RenderTestContext test(64, 48);
    if (!test.isValid())
        GTEST_SKIP() << "no GPU";
    ASSERT_TRUE(warmUp(test));
    // something in layer 0 first, so the clear shows
    drawFrame(test, [](RmlRenderer& r){
        Quad quad(r, 0, 0, 64, 48, Rml::ColourbPremultiplied(255, 255, 255, 255));
        r.RenderGeometry(quad.handle, Rml::Vector2f(0, 0), 0);
        quad.release(r);
    });
    const std::vector<uint8_t> layer = drawFrame(test, [](RmlRenderer&){});
    EXPECT_EQ(test.renderer.getLastFrameCommandCount(), 0u);
    EXPECT_EQ(test.renderer.getStats().passes, 1u);
    EXPECT_EQ(test.renderer.getStats().drawCalls, 0u);
    ASSERT_EQ(layer.size(), (size_t)64 * 48 * 4);
    EXPECT_TRUE(std::all_of(layer.begin(), layer.end(), [](uint8_t v){ return v == 0; })) << "layer 0 is cleared";
}

TEST(RenderRecordTest, AContextRemovedBeforeTheFrameEnds){
    UnbatchedReplay unbatched;
    // a document whose release mid-frame frees geometry (with its meshes), filters, shaders, box
    // shadow textures and effect instances while the frame is recorded
    const char* const rml = R"(<rml><head><style>
body { width: 100%; height: 100%; margin: 0px; font-family: sans-serif; }
div { position: absolute; width: 60px; height: 40px; }
#a { left: 10px; top: 10px; background-color: #e74c3c; box-shadow: 4px 6px 8px 2px #000000b0; border-radius: 8px; }
#b { left: 90px; top: 10px; background-color: #3498db; filter: blur(3px); }
#c { left: 170px; top: 10px; decorator: linear-gradient(45deg, #ff0000, #0000ff); }
#d { left: 10px; top: 70px; background-color: #2ecc71; transform: rotate(10deg); overflow: hidden; border-radius: 12px; }
</style></head><body><div id="a"/><div id="b"/><div id="c"/><div id="d">text</div></body></rml>)";
    std::vector<uint8_t> layers[2];
    size_t geometryAfter[2] = {0, 0};
    for (int recording = 1; recording >= 0; recording--){
        TestSystem::get().clearLogs();
        RecordingMode mode(recording != 0);
        RenderTestContext test(256, 128);
        if (!test.isValid())
            GTEST_SKIP() << "no GPU";
        ASSERT_NE(test.loadFromMemory(rml, "render/record-removed.rml"), nullptr);
        ASSERT_TRUE(test.renderUntilDrawn());

        beginTestFrame();
        ASSERT_TRUE(test.renderer.beginFrame(test.target, test.contextSize));
        test.context->Update();
        test.context->Render();
        // removed before endFrame: everything it gave the renderer goes back now
        Rml::Context* context = test.context;
        test.context = nullptr;
        RmlRenderer::unbindContext(context);
        Rml::RemoveContext(context->GetName());
        EXPECT_TRUE(test.renderer.endFrame());
        SystemRender::commit();
        EXPECT_EQ(test.renderer.getLastFrameCommandCount() > 0, recording != 0);
        ASSERT_TRUE(test.readLayer(layers[recording]));
        geometryAfter[recording] = test.renderer.getLiveGeometryCount();
        Rml::ReleaseTextures(&test.renderer);
        Rml::ReleaseRenderManagers();
        EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
    }
    EXPECT_EQ(geometryAfter[1], geometryAfter[0]);
    EXPECT_TRUE(layers[1] == layers[0]) << firstDifference(layers[1], layers[0], 256);
}

TEST(RenderRecordTest, AContextWithEffectsRemovedBeforeTheFrameEnds){
    // the effect instances outlive their elements until the replay, which skips their draws
    EffectScene probe;
    if (!probe.valid())
        GTEST_SKIP() << "no GPU";
    for (const char* name : {"stripes", "pixelate", "particles"}){
        const EffectManifest* m = EffectRegistry::get().find(name);
        ASSERT_NE(m, nullptr) << name;
        ASSERT_TRUE(warmUpEffect(*m)) << name;
    }
    TestSystem::get().clearLogs();
    RecordingMode mode(true);
    FixedClock clock(1.0);
    EffectScene scene;
    std::string error;
    ASSERT_TRUE(scene.loadHtml(effectsDocument(), &error)) << error;
    TestImage image;
    ASSERT_TRUE(scene.capture(image));
    const size_t instances = EffectRuntime::get().liveInstanceCount();
    EXPECT_GT(instances, 0u);

    RmlRenderer* renderer = scene.fixture().renderer();
    ASSERT_NE(renderer, nullptr);
    RmlRenderTarget target;
    ASSERT_TRUE(target.ensure(360, 240, 1));
    beginTestFrame();
    ASSERT_TRUE(renderer->beginFrame(target, Rml::Vector2i(360, 240)));
    scene.context()->Update();
    scene.context()->Render();
    const size_t recordedInstances = EffectRuntime::get().liveInstanceCount();
    // the document goes in the middle of the frame (elements destroyed, instances released)
    scene.context()->UnloadAllDocuments();
    scene.context()->Update();
    EXPECT_EQ(EffectRuntime::get().liveInstanceCount(), recordedInstances) << "released after the replay";
    EXPECT_TRUE(renderer->endFrame());
    SystemRender::commit();
    EXPECT_LT(EffectRuntime::get().liveInstanceCount(), recordedInstances) << "released by endFrame";
    target.destroy();
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}
