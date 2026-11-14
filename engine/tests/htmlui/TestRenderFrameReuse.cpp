// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S92 frame reuse (docs/specs/S92-htmlui-render-cost.md R1-R6, AC1-AC5): the signature of what a
// frame recorded follows every input of the draw (geometry and textures by serial and content
// version, everything else by value) and nothing else; a frame that matches the last drawn one is
// not replayed and its image stays; every change draws again, with the image a fresh replay gives;
// the composite covers only the tiles a frame wrote; a depth-stencil nothing reads again is not
// stored; a kept frame that differs from a fresh replay is reported (the verification mode).

#include "FixtureRunner.h"
#include "TestRenderCommon.h"

#include "HtmlUIClock.h"
#include "HtmlUITexture.h"
#include "htmlui/compat/VirtualFiles.h"
#include "htmlui/effects/EffectRuntime.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>

#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct ReuseQuad {
        Rml::Mesh mesh;
        Rml::CompiledGeometryHandle handle = 0;

        ReuseQuad(RmlRenderer& renderer, float x, float y, float w, float h, Rml::ColourbPremultiplied colour){
            Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(x, y), Rml::Vector2f(w, h), colour);
            handle = renderer.CompileGeometry(mesh.vertices, mesh.indices);
        }
    };

    // frames until one is drawn (the shaders compile on the first ones); draw() records the calls,
    // the signature of everything it recorded (origin: target px) is taken before the frame ends
    uint64_t recordedSignature(RenderTestContext& test, const std::function<void()>& draw, Rml::Vector2i origin, uint32_t& live){
        uint64_t signature = 0;
        for (int i = 0; i < 30; i++){
            beginTestFrame();
            test.renderer.beginFrame(test.target, test.contextSize);
            draw();
            live = 0;
            signature = test.renderer.signatureOf(0, test.renderer.getRecordedCommandCount(), origin, live);
            const bool drawn = test.renderer.endFrame();
            SystemRender::commit();
            if (drawn)
                break;
        }
        return signature;
    }

    uint64_t recordedSignature(RenderTestContext& test, const std::function<void()>& draw){
        uint32_t live = 0;
        return recordedSignature(test, draw, Rml::Vector2i(0, 0), live);
    }

}

TEST(RenderFrameReuse, SignatureFollowsWhatWasRecorded){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(160, 90);
    ASSERT_TRUE(test.isValid());
    const Rml::ColourbPremultiplied red(255, 0, 0, 255);
    ReuseQuad quad(test.renderer, 0, 0, 20, 10, red);
    std::unique_ptr<HtmlUITexture> owned = HtmlUITexture::createDynamic("frame-reuse-dynamic", 4, 4);
    ASSERT_TRUE(owned);
    HtmlUITexture* texture = owned.release();   // the renderer's from its first draw, released below
    const Rml::TextureHandle textureHandle = reinterpret_cast<Rml::TextureHandle>(texture);
    const Rml::Matrix4f scaled = Rml::Matrix4f::Scale(2.0f, 2.0f, 1.0f);

    auto base = [&]{
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 10), 0);
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(40, 10), textureHandle);
    };
    const uint64_t first = recordedSignature(test, base);
    EXPECT_EQ(recordedSignature(test, base), first) << "the same calls, the same signature";

    EXPECT_NE(recordedSignature(test, [&]{
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 11), 0);
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(40, 10), textureHandle);
    }), first) << "a translation";
    EXPECT_NE(recordedSignature(test, [&]{
        test.renderer.EnableScissorRegion(true);
        test.renderer.SetScissorRegion(Rml::Rectanglei::FromSize(Rml::Vector2i(100, 50)));
        base();
    }), first) << "a scissor";
    EXPECT_NE(recordedSignature(test, [&]{
        test.renderer.SetTransform(&scaled);
        base();
        test.renderer.SetTransform(nullptr);
    }), first) << "a transform";

    // the same mesh compiled again is another geometry: a released address never matches (D2)
    {
        ReuseQuad again(test.renderer, 0, 0, 20, 10, red);
        EXPECT_NE(recordedSignature(test, [&]{
            test.renderer.RenderGeometry(again.handle, Rml::Vector2f(10, 10), 0);
            test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(40, 10), textureHandle);
        }), first) << "a geometry compiled again";
        test.renderer.ReleaseGeometry(again.handle);
    }

    // texels uploaded again: the content version
    const std::vector<uint8_t> texels(4 * 4 * 4, 128);
    ASSERT_TRUE(texture->update(texels.data(), texels.size()));
    SystemRender::commit();
    const uint64_t uploaded = recordedSignature(test, base);
    EXPECT_NE(uploaded, first) << "an upload into a drawn texture";
    EXPECT_EQ(recordedSignature(test, base), uploaded);

    test.renderer.ReleaseTexture(textureHandle);
    test.renderer.ReleaseGeometry(quad.handle);
}

TEST(RenderFrameReuse, SignatureRelativeToAnOriginAndLiveSources){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(160, 90);
    ASSERT_TRUE(test.isValid());
    ReuseQuad quad(test.renderer, 0, 0, 20, 10, Rml::ColourbPremultiplied(0, 0, 255, 255));
    uint32_t live = 0;

    // the same content 30 px further, relative to an origin 30 px further: one key (an element that
    // moves keeps its layer scope's key)
    const uint64_t here = recordedSignature(test, [&]{ test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 10), 0); },
                                            Rml::Vector2i(0, 0), live);
    const uint64_t there = recordedSignature(test, [&]{ test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(40, 10), 0); },
                                             Rml::Vector2i(30, 0), live);
    EXPECT_EQ(here, there);
    EXPECT_EQ(live, 0u);
    // not under a transform: the matrix holds where the element is
    const Rml::Matrix4f rotated = Rml::Matrix4f::RotateZ(0.5f);
    const uint64_t rotatedHere = recordedSignature(test, [&]{
        test.renderer.SetTransform(&rotated);
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 10), 0);
    }, Rml::Vector2i(0, 0), live);
    const uint64_t rotatedThere = recordedSignature(test, [&]{
        test.renderer.SetTransform(&rotated);
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(40, 10), 0);
    }, Rml::Vector2i(30, 0), live);
    EXPECT_NE(rotatedHere, rotatedThere);

    // a texture whose texels change without an upload the renderer sees
    std::unique_ptr<HtmlUITexture> owned = HtmlUITexture::createDynamic("frame-reuse-video", 4, 4);
    ASSERT_TRUE(owned);
    owned->liveContent = true;
    const Rml::TextureHandle video = reinterpret_cast<Rml::TextureHandle>(owned.release());
    recordedSignature(test, [&]{ test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 10), video); }, Rml::Vector2i(0, 0), live);
    EXPECT_EQ(live, (uint32_t)RmlRenderer::LIVE_VIDEO);

    test.renderer.ReleaseTexture(video);
    test.renderer.ReleaseGeometry(quad.handle);
}

namespace {

    // an HTML document of its own for a runner (a virtual file of the compat layer)
    bool loadHtml(FixtureRunner& runner, const std::string& html){
        static int counter = 0;
        const std::string path = "@htmlui/test/s92/frame-reuse" + std::to_string(++counter) + ".html";
        htmlui::compat::VirtualFiles::add(path, html);
        std::string error;
        const bool loaded = runner.load(path, &error);
        htmlui::compat::VirtualFiles::remove(path);
        EXPECT_TRUE(loaded) << error;
        return loaded;
    }

    std::string page(const std::string& css, const std::string& body){
        return "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>html, body { margin: 0; width: 640px; height: 360px; "
               "font-family: sans-serif; color: #ffffff; }\n" + css + "</style></head><body>" + body + "</body></html>";
    }

    // The runner's frames on its clock: kept() is the last frame's framesKept
    struct Frames {
        FixtureRunner& runner;
        double clock = 0.0;

        explicit Frames(FixtureRunner& r): runner(r), clock(r.time()){}

        const HtmlUIRenderStats& next(double step = 1.0 / 60.0){
            clock += step;
            runner.frameAt(clock, true);
            return runner.renderer()->getStats();
        }
        // the same moment again (a fresh replay of the frame just drawn, with frame reuse off)
        const HtmlUIRenderStats& again(){
            runner.frameAt(clock, true);
            return runner.renderer()->getStats();
        }
        bool kept() const{ return runner.renderer()->getStats().framesKept == 1; }
        // frames until one is kept (nothing changed since the last drawn one)
        bool settle(int maxFrames = 60){
            for (int i = 0; i < maxFrames; i++){
                if (next().framesKept == 1)
                    return true;
            }
            return false;
        }
        std::vector<uint8_t> layer() const{
            std::vector<uint8_t> rgba;
            EXPECT_TRUE(runner.renderContext()->readLayer(rgba));
            return rgba;
        }
    };

    // layer 0 over the background into the screen, committed (a readback reads committed frames)
    void composite(RenderTestContext& render){
        render.compositeToScreen();
        SystemRender::commit();
    }

    // the first pixel where two images differ, or -1
    long firstDifference(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b){
        if (a.size() != b.size())
            return 0;
        for (size_t i = 0; i < a.size(); i++){
            if (a[i] != b[i])
                return (long)(i / 4);
        }
        return -1;
    }

    // the frame after a change: drawn, with the image a fresh replay of that frame gives
    void expectRedrawn(Frames& frames, const char* what){
        const HtmlUIRenderStats stats = frames.next();
        EXPECT_EQ(stats.framesKept, 0u) << what;
        EXPECT_GT(stats.passes, 0u) << what;
        const std::vector<uint8_t> drawn = frames.layer();
        {
            FrameReuseMode off(false);
            frames.again();
        }
        const std::vector<uint8_t> fresh = frames.layer();
        EXPECT_EQ(firstDifference(drawn, fresh), -1) << what << ": the frame after the change is not what a fresh replay draws";
    }

}

TEST(RenderFrameReuse, StaticDocumentDrawsNothing){
    JASPER_REQUIRE_GPU();
    FixtureRunner runner(1280, 720, 4);
    ASSERT_TRUE(runner.valid());
    std::string error;
    ASSERT_TRUE(runner.load("fixtures/s92/static.html", &error)) << error;
    RenderTestContext& render = *runner.renderContext();
    Frames frames(runner);

    // the last drawn frame's screen, until a frame is kept
    std::vector<uint8_t> drawnScreen;
    bool kept = false;
    uint32_t lastPasses = 0;
    for (int i = 0; i < 60 && !kept; i++){
        const HtmlUIRenderStats& stats = frames.next();
        lastPasses = stats.passes;
        composite(render);
        if (stats.framesKept == 1){
            kept = true;
            break;
        }
        ASSERT_TRUE(render.readScreen(drawnScreen));
    }
    ASSERT_TRUE(kept) << "no frame of a static document was kept: the last one had " << lastPasses << " passes";
    ASSERT_FALSE(drawnScreen.empty());
    for (int frame = 0; frame < 3; frame++){
        if (frame > 0){
            frames.next();
            composite(render);
        }
        const HtmlUIRenderStats& stats = runner.renderer()->getStats();
        EXPECT_EQ(stats.passes, 0u) << "frame " << frame;
        EXPECT_EQ(stats.drawCalls, 0u) << "frame " << frame;
        EXPECT_EQ(stats.framesKept, 1u) << "frame " << frame;
        std::vector<uint8_t> screen;
        ASSERT_TRUE(render.readScreen(screen));
        EXPECT_EQ(firstDifference(screen, drawnScreen), -1) << "frame " << frame << ": the composite of the kept frame";
    }
}

TEST(RenderFrameReuse, EveryChangeRedraws){
    JASPER_REQUIRE_GPU();
    FixtureRunner runner(640, 360, 4);
    ASSERT_TRUE(runner.valid());
    // a data model the document binds (created before the document loads)
    int modelValue = 1;
    Rml::DataModelConstructor constructor = runner.context()->CreateDataModel("s92");
    const bool made = static_cast<bool>(constructor);
    ASSERT_TRUE(made);
    constructor.Bind("value", &modelValue);
    Rml::DataModelHandle model = constructor.GetModelHandle();
    std::string error;
    ASSERT_TRUE(runner.load("fixtures/s92/changes.html", &error)) << error;
    Rml::ElementDocument* document = runner.document();
    ASSERT_NE(document, nullptr);
    Frames frames(runner);
    ASSERT_TRUE(frames.settle()) << "the loaded document never settled";

    struct Change {
        const char* what;
        std::function<void()> apply;
    };
    const Change changes[] = {
        {"text", [&]{ document->GetElementById("text")->SetInnerRML("two"); }},
        {"class", [&]{ document->GetElementById("classy")->SetClass("on", true); }},
        {"style property", [&]{ document->GetElementById("styled")->SetProperty("background-color", "#e0e020"); }},
        {"scroll", [&]{ document->GetElementById("scroller")->SetScrollTop(40.0f); }},
        {"image load", [&]{ document->GetElementById("pic")->SetAttribute("src", "../common/checker.png"); }},
        {"data-model value", [&]{ modelValue = 2; model.DirtyVariable("value"); }},
        {"hover", [&]{ runner.context()->ProcessMouseMove(260, 80, 0); }},
    };
    for (const Change& change : changes){
        change.apply();
        expectRedrawn(frames, change.what);
        EXPECT_TRUE(frames.settle()) << change.what << ": never settled";
    }

    // a transform animation draws every frame it runs, then settles
    document->GetElementById("spin")->SetClass("go", true);
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(frames.next().framesKept, 0u) << "transform animation, frame " << i;
    EXPECT_TRUE(frames.settle(60)) << "transform animation: never settled";

    // the focus caret blinks on the wall clock: drawn, then kept, then drawn again at the blink
    document->GetElementById("field")->Focus();
    expectRedrawn(frames, "focus");
    bool blinked = false;
    const auto start = std::chrono::steady_clock::now();
    while (!blinked && std::chrono::steady_clock::now() - start < std::chrono::seconds(5)){
        const bool wasKept = frames.kept();
        blinked = wasKept && frames.next().framesKept == 0;
        if (!wasKept)
            frames.next();
    }
    EXPECT_TRUE(blinked) << "the caret's blink drew no frame";
    document->GetElementById("field")->Blur();
    EXPECT_TRUE(frames.settle()) << "blur: never settled";

    // a new size and a new sample count draw again
    RenderTestContext& render = *runner.renderContext();
    render.target.ensure(render.targetSize.x, render.targetSize.y, render.sampleCount() == 1 ? 4 : 1);
    expectRedrawn(frames, "sample count");
    EXPECT_TRUE(frames.settle()) << "sample count: never settled";
    render.target.ensure(render.targetSize.x / 2, render.targetSize.y / 2, render.sampleCount());
    render.targetSize = Rml::Vector2i(render.target.getWidth(), render.target.getHeight());
    expectRedrawn(frames, "resize");
    EXPECT_TRUE(frames.settle()) << "resize: never settled";
}

TEST(RenderFrameReuse, LiveSourcesDrawEveryFrame){
    JASPER_REQUIRE_GPU();
    // a phase-5 effect on the running clock draws every frame; on a fixed clock it settles
    {
        FixtureRunner runner(320, 200, 1);
        ASSERT_TRUE(runner.valid());
        ASSERT_TRUE(loadHtml(runner, page("#fx { position: absolute; left: 10px; top: 10px; width: 160px; height: 80px; "
                                          "background-image: effect(stripes, density 4); }\n", "<div id=\"fx\"></div>")));
        Frames frames(runner);
        for (int i = 0; i < 40; i++)
            frames.next();
        for (int i = 0; i < 4; i++){
            const HtmlUIRenderStats& stats = frames.next();
            EXPECT_EQ(stats.framesKept, 0u) << "running effect, frame " << i;
            EXPECT_NE(stats.liveSources & RmlRenderer::LIVE_EFFECT, 0u) << "frame " << i;
        }
        htmlui::EffectClock::setFixedTime(1.0);
        EXPECT_TRUE(frames.settle()) << "an effect on a fixed clock never settled";
        htmlui::EffectClock::setFixedTime(std::nullopt);
    }
    // a playing <video>
    {
        FixtureRunner runner(640, 400, 1);
        ASSERT_TRUE(runner.valid());
        std::string error;
        ASSERT_TRUE(runner.load("fixtures/video/pages/autoplay.html", &error)) << error;
        Frames frames(runner);
        bool live = false;
        for (int i = 0; i < 240 && !live; i++)
            live = (frames.next().liveSources & RmlRenderer::LIVE_VIDEO) != 0;
        ASSERT_TRUE(live) << "no frame drew the video";
        for (int i = 0; i < 4; i++)
            EXPECT_EQ(frames.next().framesKept, 0u) << "video, frame " << i;
    }
}

TEST(RenderFrameReuse, GlyphPageGrowthChangesTheSignature){
    JASPER_REQUIRE_GPU();
    RenderTestContext test(160, 90);
    ASSERT_TRUE(test.isValid());
    ReuseQuad quad(test.renderer, 0, 0, 20, 10, Rml::ColourbPremultiplied(255, 255, 255, 255));
    // a glyph page: the font engine bumps its generation when it adds glyphs
    auto generation = std::make_shared<uint64_t>(1);
    RmlRenderer::AtlasTexture desc;
    desc.sample = HtmlUITexture::Sample::COVERAGE;
    desc.generation = generation;
    const Rml::TextureHandle page = test.renderer.createAtlasTexture(8, 8, desc);
    ASSERT_NE(page, (Rml::TextureHandle)0);
    auto draw = [&]{ test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 10), page); };
    const uint64_t before = recordedSignature(test, draw);
    EXPECT_EQ(recordedSignature(test, draw), before);
    (*generation)++;
    EXPECT_NE(recordedSignature(test, draw), before);
    test.renderer.ReleaseTexture(page);
    test.renderer.ReleaseGeometry(quad.handle);
}

TEST(RenderFrameReuse, CompositesOnlyDrawnTiles){
    JASPER_REQUIRE_GPU();
    // the HUD: content on about a tenth of the view
    FixtureRunner runner(1280, 720, 4);
    ASSERT_TRUE(runner.valid());
    std::string error;
    ASSERT_TRUE(runner.load("fixtures/s92/hud.html", &error)) << error;
    RenderTestContext& render = *runner.renderContext();
    Frames frames(runner);
    ASSERT_TRUE(frames.settle());
    const HtmlUIRenderStats& stats = runner.renderer()->getStats();
    const uint32_t view = 1280u * 720u;
    std::printf("[ s92 ] hud: %u composite tiles, %u composite pixels (%.1f %% of the view)\n", stats.compositeTiles, stats.compositePixels,
                100.0 * stats.compositePixels / view);
    EXPECT_GT(stats.compositePixels, 0u);
    EXPECT_LE(stats.compositePixels, view / 5);
    EXPECT_LE(stats.compositePixels, stats.compositeTiles * 64u * 64u);

    // the tiles' composite is the whole quad's image: the background where the layer is
    // transparent, the layer over it elsewhere
    composite(render);
    std::vector<uint8_t> layer, screen;
    ASSERT_TRUE(render.readLayer(layer));
    ASSERT_TRUE(render.readScreen(screen));
    ASSERT_EQ(layer.size(), screen.size());
    size_t content = 0;
    for (size_t p = 0; p < layer.size() / 4; p++){
        const uint8_t* l = &layer[p * 4];
        const uint8_t* s = &screen[p * 4];
        const int a = l[3];
        if (a == 0 && l[0] == 0 && l[1] == 0 && l[2] == 0){
            ASSERT_EQ(s[0], RenderTestContext::background) << "pixel " << p % 1280 << ", " << p / 1280;
            continue;
        }
        content++;
        for (int c = 0; c < 3; c++){
            const int expected = l[c] + (RenderTestContext::background * (255 - a) + 127) / 255;
            ASSERT_NEAR(s[c], expected, 1) << "pixel " << p % 1280 << ", " << p / 1280 << " channel " << c;
        }
    }
    EXPECT_GT(content, 0u);

    // an empty document draws no composite at all
    ASSERT_TRUE(loadHtml(runner, page("", "<div></div>")));
    Frames empty(runner);
    ASSERT_TRUE(empty.settle());
    const uint64_t draws = runner.renderer()->getCompositeDrawCount();
    composite(render);
    EXPECT_EQ(runner.renderer()->getCompositeDrawCount(), draws);
    EXPECT_EQ(runner.renderer()->getStats().compositePixels, 0u);
}

TEST(RenderFrameReuse, DepthStencilDiscarded){
    JASPER_REQUIRE_GPU();
    FixtureRunner runner(640, 360, 4);
    ASSERT_TRUE(runner.valid());
    FrameReuseMode off(false);
    // no stencil use: no depth-stencil is stored
    std::string error;
    ASSERT_TRUE(runner.load("fixtures/s92/btn.html", &error)) << error;
    Frames frames(runner);
    for (int i = 0; i < 30; i++)
        frames.next();
    const HtmlUIRenderStats plain = frames.next();
    EXPECT_GT(plain.passes, 0u);
    EXPECT_EQ(plain.stencilDraws, 0u);
    EXPECT_EQ(plain.depthStencilStores, 0u) << plain.passes << " passes";

    // a rotated rounded clip around filtered content: the passes after the last one testing the
    // mask store none
    ASSERT_TRUE(loadHtml(runner, page(
        "#clip { position: absolute; left: 120px; top: 60px; width: 300px; height: 200px; overflow: hidden; border-radius: 30px; "
        "transform: rotate(12deg); background-color: #203040; }\n"
        ".item { position: absolute; width: 120px; height: 60px; background-color: #e08020; filter: drop-shadow(4px 4px 0px #000000); }\n",
        "<div id=\"clip\"><div class=\"item\" style=\"left: -20px; top: 20px\"></div>"
        "<div class=\"item\" style=\"left: 200px; top: 150px\"></div>"
        "<div style=\"position: absolute; left: 10px; top: 100px; font-size: 24px\">clipped text</div></div>")));
    Frames clipped(runner);
    for (int i = 0; i < 30; i++)
        clipped.next();
    const HtmlUIRenderStats stencil = clipped.next();
    const std::vector<uint8_t> discarded = clipped.layer();
    // every depth-stencil stored, as before: the same image, a store per layer pass
    RmlRenderer::setDepthStencilDiscardForTesting(false);
    const HtmlUIRenderStats stored = clipped.again();
    const std::vector<uint8_t> kept = clipped.layer();
    RmlRenderer::setDepthStencilDiscardForTesting(true);
    std::printf("[ s92 ] rotated clip: %u passes, %u stencil draws, %u depth-stencil stores (%u storing every one)\n", stencil.passes,
                stencil.stencilDraws, stencil.depthStencilStores, stored.depthStencilStores);
    EXPECT_GT(stencil.stencilDraws, 0u);
    EXPECT_EQ(stencil.passes, stored.passes);
    EXPECT_LT(stencil.depthStencilStores, stored.depthStencilStores) << "the last pass on each depth-stencil stores it";
    EXPECT_EQ(firstDifference(discarded, kept), -1) << "the images with and without the discards";
}

TEST(RenderFrameReuse, VerificationReportsAStaleKeptFrame){
    JASPER_REQUIRE_GPU();
    const bool verification = RmlRenderer::isKeptFrameVerification();
    RmlRenderer::setKeptFrameVerificationForTesting(true);
    RenderTestContext test(160, 90);
    ASSERT_TRUE(test.isValid());
    ReuseQuad quad(test.renderer, 0, 0, 40, 40, Rml::ColourbPremultiplied(255, 255, 255, 255));
    std::unique_ptr<HtmlUITexture> owned = HtmlUITexture::createDynamic("frame-reuse-stale", 4, 4);
    ASSERT_TRUE(owned);
    HtmlUITexture* texture = owned.release();
    const Rml::TextureHandle handle = reinterpret_cast<Rml::TextureHandle>(texture);
    std::vector<uint8_t> red(4 * 4 * 4, 0), blue(4 * 4 * 4, 0);
    for (size_t i = 0; i < red.size(); i += 4){
        red[i] = red[i + 3] = 255;
        blue[i + 2] = blue[i + 3] = 255;
    }
    ASSERT_TRUE(texture->update(red.data(), red.size()));
    SystemRender::commit();
    auto frame = [&]{
        beginTestFrame();
        test.renderer.beginFrame(test.target, test.contextSize);
        test.renderer.RenderGeometry(quad.handle, Rml::Vector2f(10, 10), handle);
        const bool drawn = test.renderer.endFrame();
        SystemRender::commit();
        return drawn;
    };
    TestSystem::get().clearLogs();
    for (int i = 0; i < 30 && !test.renderer.wasLastFrameKept(); i++)
        frame();
    ASSERT_TRUE(test.renderer.wasLastFrameKept());
    frame();
    test.renderer.checkKeptFrame();
    EXPECT_EQ(test.renderer.getKeptMismatchCount(), 0u) << "a kept frame that is what it shows";

    // texels changed behind the renderer's back (no content version): the kept frame is stale
    ASSERT_TRUE(texture->render->updateTexture(blue.data(), blue.size()));
    SystemRender::commit();
    frame();
    EXPECT_TRUE(test.renderer.wasLastFrameKept());
    frame();
    test.renderer.checkKeptFrame();
    EXPECT_GE(test.renderer.getKeptMismatchCount(), 1u);
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI] kept frame differs"), 1u);
    test.renderer.ReleaseTexture(handle);
    test.renderer.ReleaseGeometry(quad.handle);
    RmlRenderer::setKeptFrameVerificationForTesting(verification);
}
