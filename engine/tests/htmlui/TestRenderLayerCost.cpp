// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S92 layer cost (docs/specs/S92-htmlui-render-cost.md R29, AC10): the passes per frame of the
// port's UI patterns (fixtures/s92/), cold (the first frame drawn), warm (another element changed)
// and idle (nothing changed), at 4 samples and at 1. Every row is printed ("[ s92 ]" lines); the
// budgets asserted are those of the packages that landed (S92-A: the idle column and the static
// row; the cold and warm columns of the layered rows are S92-B2's).

#include "FixtureRunner.h"
#include "TestRenderCommon.h"

#include "HtmlUIClock.h"
#include "HtmlUITexture.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    struct PatternFrames {
        HtmlUIRenderStats cold, warm, idle;
    };

    // the element a warm frame changes (#tick, or the fixture's own)
    void changeText(FixtureRunner& runner, const char* id, int value){
        Rml::Element* element = runner.document() ? runner.document()->GetElementById(id) : nullptr;
        ASSERT_NE(element, nullptr) << "#" << id;
        element->SetInnerRML(std::to_string(value));
    }

    // cold: the first frame that draws; then a frame for what it loaded; warm: #changed changed; idle:
    // the second frame after it with nothing changed
    bool measurePattern(const std::string& name, int samples, const char* changed, PatternFrames& frames){
        FixtureRunner runner(1280, 720, samples);
        if (!runner.valid())
            return false;
        std::string error;
        if (!runner.load("fixtures/s92/" + name + ".html", &error)){
            ADD_FAILURE() << name << ": " << error;
            return false;
        }
        double clock = runner.time();
        bool cold = false;
        for (int i = 0; i < 60 && !cold; i++){
            clock += 1.0 / 60.0;
            runner.frameAt(clock, true);
            cold = runner.renderer()->getStats().passes > 0;
            if (cold)
                frames.cold = runner.renderer()->getStats();
        }
        if (!cold){
            ADD_FAILURE() << name << ": no frame was drawn";
            return false;
        }
        HtmlUI::getFontEngine().finishDistanceGlyphsForTesting();
        clock += 1.0 / 60.0;
        runner.frameAt(clock, true);
        changeText(runner, changed, 1);
        clock += 1.0 / 60.0;
        runner.frameAt(clock, true);
        frames.warm = runner.renderer()->getStats();
        for (int i = 0; i < 2; i++){
            clock += 1.0 / 60.0;
            runner.frameAt(clock, true);
        }
        frames.idle = runner.renderer()->getStats();
        return true;
    }

    void printPattern(const std::string& name, int samples, const PatternFrames& f){
        std::printf("[ s92 ] %-16s %dx cold: passes %3u layers %3u layerPixels %8u | warm: passes %3u layers %3u kept %u cache hits %2u misses %2u | "
                    "idle: passes %3u draws %3u kept %u compositePixels %7u\n",
                    name.c_str(), samples, f.cold.passes, f.cold.layersPushed, f.cold.layerPixels, f.warm.passes, f.warm.layersPushed,
                    f.warm.framesKept, f.warm.layerCacheHits, f.warm.layerCacheMisses, f.idle.passes, f.idle.drawCalls, f.idle.framesKept,
                    f.idle.compositePixels);
    }

}

TEST(RenderLayerCost, ArcadePatterns){
    JASPER_REQUIRE_GPU();
    struct Row {
        const char* name;
        const char* changed;
        uint32_t cold;      // R29's budget for the first frame drawn
    };
    const Row rows[] = {{"static", "tick", 1},     {"rounded-gradient", "tick", 1}, {"btn", "tick", 4},     {"card", "tick", 7},
                        {"inset-clip", "tick", 1}, {"cards4", "timer", 1 + 4 * 6},  {"o2-frame", "tick", 1}};
    for (int samples : {4, 1}){
        for (const Row& row : rows){
            PatternFrames frames;
            if (!measurePattern(row.name, samples, row.changed, frames))
                continue;
            printPattern(row.name, samples, frames);
            // S92-A (R1, R29's idle column and static row): nothing changed, nothing drawn
            EXPECT_EQ(frames.idle.passes, 0u) << row.name << " at " << samples << "x";
            EXPECT_EQ(frames.idle.drawCalls, 0u) << row.name << " at " << samples << "x";
            EXPECT_EQ(frames.idle.framesKept, 1u) << row.name << " at " << samples << "x";
            if (std::string(row.name) == "static"){
                EXPECT_EQ(frames.cold.passes, 1u) << samples << "x";
                EXPECT_EQ(frames.warm.passes, 1u) << samples << "x";
            }
            EXPECT_LE(frames.cold.passes, row.cold) << row.name << " at " << samples << "x: cold";
            // S92-B2 (R9): the warm frame draws every layer scope from the cache: one pass. Not
            // inset-clip: its scopes hold text, whose glyph page grows with the warm frame's new
            // glyph (a page's generation is part of what a scope reads)
            if (std::string(row.name) != "inset-clip"){
                EXPECT_EQ(frames.warm.passes, 1u) << row.name << " at " << samples << "x: warm";
                EXPECT_EQ(frames.warm.layerCacheMisses, 0u) << row.name << " at " << samples << "x: warm";
                EXPECT_EQ(frames.warm.layersPushed, 0u) << row.name << " at " << samples << "x: warm";
            }
        }
    }
}

// S92 B2: the layer cache (R9, AC9) and element-sized layers (R10, R30, AC11)

namespace {

    // a filtered button (a drop shadow without blur: its result moves with it pixel for pixel), a
    // child in it, and a box elsewhere another frame changes
    std::string cacheDocument(int left, const std::string& button = "", const std::string& child = "#f0c020",
                              const std::string& tick = "#20a040"){
        return "<rml><head><style>body { width: 320px; height: 200px; margin: 0; padding: 0; } div { display: block; } "
               "#btn { position: absolute; left: " + std::to_string(left) + "px; top: 30px; width: 120px; height: 60px; "
               "background-color: #3060c0; border-radius: 12px; filter: drop-shadow(#00000066 0px 6px 0px); " + button + " } "
               "#child { position: absolute; left: 10px; top: 10px; width: 40px; height: 20px; background-color: " + child + "; } "
               "#tick { position: absolute; left: 260px; top: 160px; width: 20px; height: 20px; background-color: " + tick + "; } "
               "</style></head><body><div id=\"btn\"><div id=\"child\"/></div><div id=\"tick\"/></body></rml>";
    }

    struct CacheFrame {
        std::vector<uint8_t> rgba;
        HtmlUIRenderStats stats;
    };

    // frames until one is drawn: that one (the shaders compile on the first frames)
    bool firstDrawn(RenderTestContext& render, CacheFrame& frame){
        for (int i = 0; i < 30; i++){
            if (render.renderFrame()){
                frame.stats = render.renderer.getStats();
                return render.readLayer(frame.rgba);
            }
        }
        return false;
    }

    // the next frame, replayed: the cache is measured on frames S92 R1 would keep when nothing changed
    bool nextFrame(RenderTestContext& render, CacheFrame& frame){
        render.renderer.addLiveSource(RmlRenderer::LIVE_REQUESTED);
        if (!render.renderFrame())
            return false;
        frame.stats = render.renderer.getStats();
        return render.readLayer(frame.rgba);
    }

    // the first frames, without the cache: what the first drawn frame loaded is in place (the
    // elements' geometry is made again once), then the frame with the cache that stores
    bool settledThenStored(RenderTestContext& render, CacheFrame& stored){
        RmlRenderer::setLayerCacheForTesting(false);
        CacheFrame settling;
        const bool settled = firstDrawn(render, settling) && nextFrame(render, settling);
        RmlRenderer::setLayerCacheForTesting(true);
        return settled && nextFrame(render, stored);
    }

    // the document's image drawn by a renderer of its own, the layer cache on or off (its first frame: a miss)
    bool freshImage(const std::string& rml, bool cache, std::vector<uint8_t>& rgba, int width = 320, int height = 200, int samples = 4){
        const bool before = RmlRenderer::isLayerCache();
        RmlRenderer::setLayerCacheForTesting(cache);
        RenderTestContext render(width, height, width, height, samples);
        CacheFrame frame;
        const bool drawn = render.isValid() && render.loadFromMemory(rml, "layer-cache") && firstDrawn(render, frame);
        RmlRenderer::setLayerCacheForTesting(before);
        rgba = std::move(frame.rgba);
        return drawn;
    }

    // the pixels of a and b within the rect that differ (a channel), the first one's position
    size_t differingPixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int width, Rml::Rectanglei rect, std::string* first = nullptr){
        size_t count = 0;
        for (int y = rect.Top(); y < rect.Bottom(); y++){
            for (int x = rect.Left(); x < rect.Right(); x++){
                const size_t i = (size_t(y) * width + size_t(x)) * 4;
                if (std::memcmp(&a[i], &b[i], 4) != 0){
                    if (count == 0 && first)
                        *first = std::to_string(x) + "," + std::to_string(y);
                    count++;
                }
            }
        }
        return count;
    }

    struct LayerCacheOn {
        bool before = RmlRenderer::isLayerCache();
        LayerCacheOn(){ RmlRenderer::setLayerCacheForTesting(true); }
        ~LayerCacheOn(){ RmlRenderer::setLayerCacheForTesting(before); RmlRenderer::setLayerCacheBudgetForTesting(0); }
        LayerCacheOn(const LayerCacheOn&) = delete;
        LayerCacheOn& operator=(const LayerCacheOn&) = delete;
    };

}

// A filtered element draws from the layer cache while it does not change: the frame that stored its
// result and the frames that draw it from the cache give the same pixels, as a renderer without the
// cache. A move, an opacity fade and two equal elements hit; a changed child misses once.
TEST(RenderLayerCost, CacheHitsWhileUnchanged){
    JASPER_REQUIRE_GPU();
    LayerCacheOn on;
    const Rml::Rectanglei all = Rml::Rectanglei::FromSize(Rml::Vector2i(320, 200));
    const Rml::Rectanglei button = Rml::Rectanglei::FromSize(Rml::Vector2i(240, 140));
    for (int samples : {4, 1}){
        SCOPED_TRACE(std::to_string(samples) + " samples");
        RenderTestContext render(320, 200, 320, 200, samples);
        ASSERT_TRUE(render.isValid());
        Rml::ElementDocument* document = render.loadFromMemory(cacheDocument(40), "layer-cache");
        ASSERT_NE(document, nullptr);
        Rml::Element* btn = document->GetElementById("btn");
        Rml::Element* child = document->GetElementById("child");
        Rml::Element* tick = document->GetElementById("tick");
        ASSERT_TRUE(btn && child && tick);

        // the first frame stores the result
        CacheFrame miss;
        ASSERT_TRUE(settledThenStored(render, miss));
        EXPECT_EQ(miss.stats.layerCacheHits, 0u);
        EXPECT_GE(miss.stats.layerCacheMisses, 1u);
        EXPECT_GE(miss.stats.filtersApplied, 1u);
        EXPECT_GT(miss.stats.layerCacheBytes, 0u);
        std::vector<uint8_t> uncached;
        ASSERT_TRUE(freshImage(cacheDocument(40), false, uncached, 320, 200, samples));
        std::string where;
        EXPECT_EQ(differingPixels(miss.rgba, uncached, 320, all, &where), 0u) << "the stored frame differs from the uncached one at " << where;

        // another element changed: the button is drawn from the cache, no layer, no filter
        tick->SetProperty("background-color", "#a02040");
        CacheFrame hit;
        ASSERT_TRUE(nextFrame(render, hit));
        EXPECT_EQ(hit.stats.layerCacheMisses, 0u);
        EXPECT_GE(hit.stats.layerCacheHits, 1u);
        EXPECT_EQ(hit.stats.filtersApplied, 0u);
        EXPECT_EQ(hit.stats.layersPushed, 0u);
        EXPECT_EQ(hit.stats.passes, 1u);
        EXPECT_EQ(differingPixels(miss.rgba, hit.rgba, 320, button, &where), 0u) << "the cached frame differs from the stored one at " << where;
        ASSERT_TRUE(freshImage(cacheDocument(40, "", "#f0c020", "#a02040"), false, uncached, 320, 200, samples));
        EXPECT_EQ(differingPixels(hit.rgba, uncached, 320, all, &where), 0u) << "the cached frame differs from the uncached one at " << where;

        // moved by 7 px: the same result relative to its window
        btn->SetProperty("left", "47px");
        CacheFrame moved;
        ASSERT_TRUE(nextFrame(render, moved));
        EXPECT_EQ(moved.stats.layerCacheMisses, 0u) << "a moved element misses";
        EXPECT_GE(moved.stats.layerCacheHits, 1u);
        ASSERT_TRUE(freshImage(cacheDocument(47, "", "#f0c020", "#a02040"), false, uncached, 320, 200, samples));
        EXPECT_EQ(differingPixels(moved.rgba, uncached, 320, all, &where), 0u) << "the moved cached frame differs at " << where;

        // a changed child: one miss, then hits again
        child->SetProperty("background-color", "#c02020");
        CacheFrame changed, again;
        ASSERT_TRUE(nextFrame(render, changed));
        EXPECT_EQ(changed.stats.layerCacheHits, 0u);
        EXPECT_GE(changed.stats.layerCacheMisses, 1u);
        ASSERT_TRUE(nextFrame(render, again));
        EXPECT_EQ(again.stats.layerCacheMisses, 0u);
        EXPECT_GE(again.stats.layerCacheHits, 1u);
        EXPECT_EQ(differingPixels(changed.rgba, again.rgba, 320, all, &where), 0u) << where;

        // an opacity fade: every frame hits (the opacity is applied at the composite), and gives the
        // pixels of a renderer that stored the result at that opacity
        uint32_t fadeHits = 0, fadeMisses = 0;
        for (int k = 0; k < 10; k++){
            const float opacity = 0.95f - 0.1f * (float)k;
            btn->SetProperty("opacity", std::to_string(opacity));
            CacheFrame fade;
            ASSERT_TRUE(nextFrame(render, fade));
            fadeHits += fade.stats.layerCacheHits;
            fadeMisses += fade.stats.layerCacheMisses;
            if (k == 4){
                std::vector<uint8_t> stored;
                ASSERT_TRUE(freshImage(cacheDocument(47, "opacity: " + std::to_string(opacity) + ";", "#c02020", "#a02040"), true, stored, 320, 200,
                                       samples));
                EXPECT_EQ(differingPixels(fade.rgba, stored, 320, all, &where), 0u) << "the faded cached frame differs at " << where;
            }
        }
        EXPECT_EQ(fadeHits, 10u);
        EXPECT_EQ(fadeMisses, 0u);
    }
}

// Two equal elements share one entry (no element identity: the scope's signature relative to its window)
TEST(RenderLayerCost, CacheSharesEqualElements){
    JASPER_REQUIRE_GPU();
    LayerCacheOn on;
    const std::string rml =
        "<rml><head><style>body { width: 320px; height: 200px; margin: 0; padding: 0; } div { display: block; } "
        ".b { position: absolute; top: 30px; width: 80px; height: 50px; background-color: #3060c0; border-radius: 10px; "
        "filter: drop-shadow(#00000080 0px 4px 0px); }</style></head><body>"
        "<div class=\"b\" style=\"left: 20px\"/><div class=\"b\" style=\"left: 131px\"/></body></rml>";
    RenderTestContext render(320, 200, 320, 200, 4);
    ASSERT_TRUE(render.isValid());
    ASSERT_NE(render.loadFromMemory(rml, "layer-cache"), nullptr);
    CacheFrame first, second;
    ASSERT_TRUE(settledThenStored(render, first));
    // the second draws what the first stored in the same frame
    EXPECT_EQ(first.stats.layerCacheMisses, 1u);
    EXPECT_EQ(first.stats.layerCacheHits, 1u);
    EXPECT_EQ(first.stats.filtersApplied, 1u);
    EXPECT_EQ(render.renderer.getLayerCacheEntryCount(), 1u);
    ASSERT_TRUE(nextFrame(render, second));
    EXPECT_EQ(second.stats.layerCacheHits, 2u);
    EXPECT_EQ(second.stats.passes, 1u);
    std::vector<uint8_t> uncached;
    ASSERT_TRUE(freshImage(rml, false, uncached));
    std::string where;
    EXPECT_EQ(differingPixels(second.rgba, uncached, 320, Rml::Rectanglei::FromSize(Rml::Vector2i(320, 200)), &where), 0u) << where;
}

namespace {

    struct RecordedScope {
        bool drawn = false;
        HtmlUIRenderStats stats;
    };

    // one frame recording a layer scope through the render interface: a quad (textured with
    // texture), composited through filters, or (backdrop) the layer under it composited into the
    // pushed layer first
    RecordedScope recordScope(RenderTestContext& test, Rml::CompiledGeometryHandle quad, Rml::TextureHandle texture,
                              Rml::CompiledFilterHandle filter, bool backdrop = false){
        RecordedScope result;
        for (int i = 0; i < 30 && !result.drawn; i++){
            beginTestFrame();
            // replayed: S92 R1 keeps a frame that records what the last one did
            test.renderer.addLiveSource(RmlRenderer::LIVE_REQUESTED);
            test.renderer.beginFrame(test.target, test.contextSize);
            test.renderer.RenderGeometry(quad, Rml::Vector2f(4, 4), 0);
            Rml::Rectanglei region = Rml::Rectanglei::FromCorners(Rml::Vector2i(10, 10), Rml::Vector2i(70, 50));
            const Rml::LayerHandle layer = test.renderer.PushLayerRegion(region);
            if (backdrop)
                test.renderer.CompositeLayers(0, layer, Rml::BlendMode::Blend, {});
            test.renderer.RenderGeometry(quad, Rml::Vector2f(20, 20), texture);
            const Rml::CompiledFilterHandle filters[1] = {filter};
            test.renderer.CompositeLayers(layer, 0, Rml::BlendMode::Blend, Rml::Span<const Rml::CompiledFilterHandle>(filters, 1));
            test.renderer.PopLayer();
            result.drawn = test.renderer.endFrame();
            result.stats = test.renderer.getStats();
            SystemRender::commit();
        }
        return result;
    }

}

// What a scope reads that its signature cannot version (a live texture), a layer outside it (a
// backdrop) is never kept; the same scope without them is
TEST(RenderLayerCost, CacheSkipsLiveAndBackdropScopes){
    JASPER_REQUIRE_GPU();
    LayerCacheOn on;
    RenderTestContext test(160, 90, 160, 90, 4);
    ASSERT_TRUE(test.isValid());
    Rml::Mesh mesh;
    Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(0, 0), Rml::Vector2f(30, 20), Rml::ColourbPremultiplied(40, 90, 200, 255));
    const Rml::CompiledGeometryHandle quad = test.renderer.CompileGeometry(mesh.vertices, mesh.indices);
    const Rml::CompiledFilterHandle shadow = test.renderer.CompileFilter(
        "drop-shadow", Rml::Dictionary{{"sigma", Rml::Variant(0.0f)}, {"color", Rml::Variant(Rml::Colourb(0, 0, 0, 128))},
                                       {"offset", Rml::Variant(Rml::Vector2f(0.0f, 3.0f))}});
    ASSERT_NE(shadow, 0u);
    std::unique_ptr<HtmlUITexture> owned = HtmlUITexture::createDynamic("layer-cache-live", 4, 4);
    ASSERT_TRUE(owned);
    owned->liveContent = true;
    const Rml::TextureHandle live = reinterpret_cast<Rml::TextureHandle>(owned.release());

    for (int frame = 0; frame < 2; frame++){
        const RecordedScope scope = recordScope(test, quad, live, shadow);
        ASSERT_TRUE(scope.drawn);
        EXPECT_EQ(scope.stats.layerCacheHits + scope.stats.layerCacheMisses, 0u) << "a live scope was looked up";
        EXPECT_EQ(scope.stats.filtersApplied, 1u);
    }
    for (int frame = 0; frame < 2; frame++){
        const RecordedScope scope = recordScope(test, quad, 0, shadow, true);
        ASSERT_TRUE(scope.drawn);
        EXPECT_EQ(scope.stats.layerCacheHits + scope.stats.layerCacheMisses, 0u) << "a backdrop scope was looked up";
    }
    EXPECT_EQ(test.renderer.getLayerCacheEntryCount(), 0u);

    const RecordedScope stored = recordScope(test, quad, 0, shadow);
    EXPECT_EQ(stored.stats.layerCacheMisses, 1u);
    const RecordedScope hit = recordScope(test, quad, 0, shadow);
    EXPECT_EQ(hit.stats.layerCacheHits, 1u);
    EXPECT_EQ(hit.stats.filtersApplied, 0u);
    EXPECT_EQ(test.renderer.getLayerCacheEntryCount(), 1u);

    // the view lost: the cache goes with it
    std::vector<uint32_t> buffers, images;
    test.renderer.getGpuHandles(buffers, images);
    const size_t imagesWithCache = images.size();
    test.renderer.releaseGpuResources();
    EXPECT_EQ(test.renderer.getLayerCacheEntryCount(), 0u);
    EXPECT_EQ(test.renderer.getLayerCacheBytes(), 0u);
    test.renderer.getGpuHandles(buffers, images);
    EXPECT_LT(images.size(), imagesWithCache);

    test.renderer.ReleaseTexture(live);
    test.renderer.ReleaseFilter(shadow);
    test.renderer.ReleaseGeometry(quad);
}

// What a kept scope's result depends on, changed alone, misses: its filter's parameters, the texels
// of a texture drawn in it (uploaded again, not live), its geometry. The frame after the change has
// the pixels of a renderer without the cache, then the new result is drawn from the cache
TEST(RenderLayerCost, CacheMissesOnEveryScopeInput){
    JASPER_REQUIRE_GPU();
    LayerCacheOn on;
    RenderTestContext test(160, 90, 160, 90, 4);
    ASSERT_TRUE(test.isValid());
    // RmlUi's contract: a compiled geometry's vertices stay valid until it is released (the
    // renderer reads them until the frame's end), so the meshes outlive their handles
    std::deque<Rml::Mesh> meshes;
    auto compileQuad = [&](Rml::ColourbPremultiplied colour){
        Rml::Mesh& mesh = meshes.emplace_back();
        Rml::MeshUtilities::GenerateQuad(mesh, Rml::Vector2f(0, 0), Rml::Vector2f(30, 20), colour);
        return test.renderer.CompileGeometry(mesh.vertices, mesh.indices);
    };
    auto compileShadow = [&](Rml::Colourb colour){
        return test.renderer.CompileFilter("drop-shadow", Rml::Dictionary{{"sigma", Rml::Variant(0.0f)}, {"color", Rml::Variant(colour)},
                                                                          {"offset", Rml::Variant(Rml::Vector2f(0.0f, 3.0f))}});
    };
    Rml::CompiledGeometryHandle quad = compileQuad(Rml::ColourbPremultiplied(255, 255, 255, 255));
    Rml::CompiledFilterHandle shadow = compileShadow(Rml::Colourb(0, 0, 0, 128));
    ASSERT_NE(shadow, 0u);
    std::unique_ptr<HtmlUITexture> owned = HtmlUITexture::createDynamic("layer-cache-texels", 4, 4);
    ASSERT_TRUE(owned);
    HtmlUITexture* texels = owned.get();
    const Rml::TextureHandle texture = reinterpret_cast<Rml::TextureHandle>(owned.release());
    std::vector<uint8_t> rgba(4 * 4 * 4, 200);
    ASSERT_TRUE(texels->update(rgba.data(), rgba.size()));
    SystemRender::commit();

    // the scope stored, then drawn from the cache
    ASSERT_EQ(recordScope(test, quad, texture, shadow).stats.layerCacheMisses, 1u);
    ASSERT_EQ(recordScope(test, quad, texture, shadow).stats.layerCacheHits, 1u);

    struct Change {
        const char* what;
        std::function<void()> apply;
    };
    const Change changes[] = {
        {"the filter's colour", [&]{
            test.renderer.ReleaseFilter(shadow);
            shadow = compileShadow(Rml::Colourb(200, 0, 0, 128));
        }},
        {"the texture's texels", [&]{
            std::fill(rgba.begin(), rgba.end(), (uint8_t)90);
            ASSERT_TRUE(texels->update(rgba.data(), rgba.size()));
            SystemRender::commit();
        }},
        {"the geometry", [&]{
            test.renderer.ReleaseGeometry(quad);
            quad = compileQuad(Rml::ColourbPremultiplied(40, 200, 90, 255));
        }},
    };
    for (const Change& change : changes){
        change.apply();
        const RecordedScope missed = recordScope(test, quad, texture, shadow);
        ASSERT_TRUE(missed.drawn) << change.what;
        EXPECT_EQ(missed.stats.layerCacheHits, 0u) << change.what << ": the old result was drawn";
        EXPECT_EQ(missed.stats.layerCacheMisses, 1u) << change.what;
        std::vector<uint8_t> cached, uncached;
        ASSERT_TRUE(test.readLayer(cached));
        {
            LayerCacheOff off;
            ASSERT_TRUE(recordScope(test, quad, texture, shadow).drawn);
        }
        ASSERT_TRUE(test.readLayer(uncached));
        std::string where;
        EXPECT_EQ(differingPixels(cached, uncached, 160, Rml::Rectanglei::FromSize(Rml::Vector2i(160, 90)), &where), 0u)
            << change.what << ": differs from a frame without the cache at " << where;
        EXPECT_EQ(recordScope(test, quad, texture, shadow).stats.layerCacheHits, 1u) << change.what << ": the new result is kept";
    }

    test.renderer.ReleaseTexture(texture);
    test.renderer.ReleaseFilter(shadow);
    test.renderer.ReleaseGeometry(quad);
}

// Over its budget the cache drops the least recently used entries and keeps within it; the
// scopes it has no room for draw as without the cache. An entry not drawn for kLayerCacheKeepFrames
// replays goes
TEST(RenderLayerCost, CacheBudget){
    JASPER_REQUIRE_GPU();
    LayerCacheOn on;
    const size_t budget = (size_t)8 * 1024 * 1024;
    RmlRenderer::setLayerCacheBudgetForTesting(budget);
    // twelve different 600 x 300 results (about 1 MiB each, with their granules)
    std::string rml = "<rml><head><style>body { width: 1280px; height: 720px; margin: 0; padding: 0; } div { display: block; } "
                      ".b { position: absolute; width: 600px; height: 300px; filter: drop-shadow(#00000080 0px 4px 0px); }"
                      "</style></head><body>";
    for (int i = 0; i < 12; i++){
        rml += "<div class=\"b\" id=\"b" + std::to_string(i) + "\" style=\"left: " + std::to_string(20 + 50 * i) + "px; top: " +
               std::to_string(20 + 30 * i) + "px; background-color: rgb(" + std::to_string(20 * i) + ", 80, 160)\"/>";
    }
    rml += "</body></rml>";
    RenderTestContext render(1280, 720, 1280, 720, 1);
    ASSERT_TRUE(render.isValid());
    Rml::ElementDocument* document = render.loadFromMemory(rml, "layer-cache");
    ASSERT_NE(document, nullptr);
    std::vector<uint8_t> uncached;
    ASSERT_TRUE(freshImage(rml, false, uncached, 1280, 720, 1));
    const Rml::Rectanglei all = Rml::Rectanglei::FromSize(Rml::Vector2i(1280, 720));
    std::string where;

    CacheFrame frame;
    ASSERT_TRUE(settledThenStored(render, frame));
    EXPECT_EQ(frame.stats.layerCacheMisses, 12u);
    EXPECT_LE(render.renderer.getLayerCacheBytes(), budget);
    EXPECT_GT(render.renderer.getLayerCacheEntryCount(), 0u);
    EXPECT_LT(render.renderer.getLayerCacheEntryCount(), 12u) << "the budget holds fewer than the twelve";
    EXPECT_EQ(differingPixels(frame.rgba, uncached, 1280, all, &where), 0u) << where;
    const size_t kept = render.renderer.getLayerCacheEntryCount();
    ASSERT_TRUE(nextFrame(render, frame));
    EXPECT_EQ(frame.stats.layerCacheHits, (uint32_t)kept);
    EXPECT_LE(render.renderer.getLayerCacheBytes(), budget);
    EXPECT_EQ(differingPixels(frame.rgba, uncached, 1280, all, &where), 0u) << where;

    // the first half hidden: the second half's misses take the room of the entries not drawn
    for (int i = 0; i < 6; i++)
        document->GetElementById("b" + std::to_string(i))->SetProperty("display", "none");
    ASSERT_TRUE(nextFrame(render, frame));
    EXPECT_LE(render.renderer.getLayerCacheBytes(), budget);
    ASSERT_TRUE(nextFrame(render, frame));
    EXPECT_EQ(frame.stats.layerCacheHits, 6u) << "the least recently used entries were not dropped for the visible ones";
    EXPECT_EQ(frame.stats.layerCacheMisses, 0u);
    EXPECT_LE(render.renderer.getLayerCacheBytes(), budget);

    // nothing of it drawn for kLayerCacheKeepFrames replays (the replays go on: a box changes)
    for (int i = 6; i < 12; i++)
        document->GetElementById("b" + std::to_string(i))->SetProperty("display", "none");
    Rml::Element* last = document->GetElementById("b0");
    last->SetProperty("display", "block");
    last->SetProperty("filter", "none");
    for (uint64_t i = 0; i <= RmlRenderer::kLayerCacheKeepFrames; i++){
        last->SetProperty("background-color", (i % 2) ? "#102030" : "#203040");
        ASSERT_TRUE(nextFrame(render, frame));
        if (i + 1 < RmlRenderer::kLayerCacheKeepFrames){
            ASSERT_GT(render.renderer.getLayerCacheEntryCount(), 0u) << "replay " << i;
        }
    }
    EXPECT_EQ(render.renderer.getLayerCacheEntryCount(), 0u);
    // their images, kept for a store of the next replay, go when it takes none
    ASSERT_TRUE(nextFrame(render, frame));
    EXPECT_EQ(render.renderer.getLayerCacheBytes(), 0u);
}

// R10, R30: every layer of the s92 fixtures covers its element's window, not the target: each
// layer and post-process pass is at most 1.5 x the granules of the window of the layer it serves,
// and none is the target's size
TEST(RenderLayerCost, LayersSizedToTheirElement){
    JASPER_REQUIRE_GPU();
    LayerCacheOn on;
    for (const char* name : {"static", "rounded-gradient", "btn", "card", "inset-clip", "cards4", "o2-frame", "hud"}){
        SCOPED_TRACE(name);
        FixtureRunner runner(1280, 720, 4);
        if (!runner.valid()){
            GTEST_SKIP() << "no GPU";
        }
        std::string error;
        ASSERT_TRUE(runner.load(std::string("fixtures/s92/") + name + ".html", &error)) << error;
        double clock = runner.time();
        bool drawn = false;
        for (int i = 0; i < 60 && !drawn; i++){
            clock += 1.0 / 60.0;
            runner.frameAt(clock, true);
            drawn = runner.renderer()->getStats().passes > 0;
        }
        ASSERT_TRUE(drawn);
        const HtmlUIRenderStats& stats = runner.renderer()->getStats();
        const RmlRenderer::LayerPassStats passes = runner.renderer()->getLastLayerPassStats();
        std::printf("[ s92 ] %-16s layers %3u layerPixels %8u window granules %8llu largest pass %7u | passes %3u: layer 0 and copies %3u, "
                    "element layers %3u, post-process %3u\n",
                    name, stats.layersPushed, stats.layerPixels, (unsigned long long)passes.windowPixels, passes.largestPass, stats.passes,
                    stats.passes - passes.layerPasses - passes.postPasses, passes.layerPasses, passes.postPasses);
        EXPECT_LE((double)stats.layerPixels, 1.5 * (double)passes.windowPixels);
        EXPECT_LT(passes.largestPass, 1280u * 720u) << "a layer within the viewport is the target's size";
    }
}
