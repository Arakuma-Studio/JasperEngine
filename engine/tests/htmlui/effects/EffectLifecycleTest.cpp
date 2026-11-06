// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// 100 cycles of loading, drawing and unloading a document with the five reference effects
// (phase-5 spec 5.1): afterwards no effect instance is live, the element-state map is empty and
// the renderer's GPU objects (geometry, textures, buffers and images) are back to the baseline of
// the first cycle. Phase 6: the same for a document with every stateful effect (their state images
// return to the pool, the pool is trimmed, and the renderer's GPU objects are back to the baseline).

#include "EffectTestCommon.h"

#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/DocumentLoader.h"
#include "htmlui/HtmlUIClock.h"

#include <cstdio>
#include "htmlui/effects/EffectSimulator.h"

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;
namespace hc = jasperengine::htmlui::compat;

namespace {

    const char* kDocument = R"(<!DOCTYPE html>
<html><head><style>
html, body { margin: 0; width: 320px; height: 200px; background: #101218; font-family: sans-serif; }
div { position: absolute; width: 140px; height: 80px; border-radius: 8px; }
#a { left: 10px; top: 10px; background-image: effect(stripes, density 6); }
#b { left: 170px; top: 10px; background-image: effect(spotlight, intensity 0.8), linear-gradient(#123, #456); }
#c { left: 10px; top: 110px; background-image: effect(cursor-trail); }
#d { left: 170px; top: 110px; background: #e74c3c; filter: effect(pixelate, size 6px); }
#e { left: 90px; top: 60px; backdrop-filter: effect(frosted-glass, blur 8px); }
#t { left: 20px; top: 20px; font-size: 24px; font-weight: bold; background-image: effect(stripes); background-clip: text; color: transparent; }
</style></head>
<body><div id="a"></div><div id="b"></div><div id="c"></div><div id="d">PX</div><div id="t">FX</div><div id="e"></div></body></html>)";

    struct Counters {
        size_t geometry = 0, textures = 0, geometryBuffers = 0, buffers = 0, images = 0;
        bool operator==(const Counters& o) const{
            return geometry == o.geometry && textures == o.textures && geometryBuffers == o.geometryBuffers && buffers == o.buffers && images == o.images;
        }
    };

    std::ostream& operator<<(std::ostream& os, const Counters& c){
        return os << "geometry " << c.geometry << ", textures " << c.textures << ", geometry buffers " << c.geometryBuffers << ", buffers "
                  << c.buffers << ", images " << c.images;
    }

    Counters countersOf(RenderTestContext& r){
        Counters c;
        c.geometry = r.renderer.getLiveGeometryCount();
        c.textures = r.renderer.getLiveTextureCount();
        c.geometryBuffers = r.renderer.getGeometryBufferCount();
        std::vector<uint32_t> buffers, images;
        r.renderer.getGpuHandles(buffers, images);
        c.buffers = buffers.size();
        c.images = images.size();
        return c;
    }

}

TEST(EffectLifecycleTest, HundredLoadUnloadCycles){
    TestSystem::get().clearLogs();
    // the headless device first: RenderTestContext does not make it, and this may be the first
    // GPU test of the run (the shaders cannot warm up without it)
    if (!GpuEnvironment::ensure())
        GTEST_SKIP() << "no GPU";
    RenderTestContext r(320, 200, 0, 0, 1);
    if (!r.isValid())
        GTEST_SKIP() << "no GPU";
    for (const char* name : {"stripes", "spotlight", "cursor-trail", "pixelate", "frosted-glass"}){
        const EffectManifest* m = EffectRegistry::get().find(name);
        ASSERT_NE(m, nullptr) << name;
        ASSERT_TRUE(warmUpEffect(*m)) << name;
    }
    const std::string path = "@htmlui/test/effects/lifecycle.html";
    hc::VirtualFiles::add(path, kDocument);
    hc::CompatOptions options;
    options.viewport = hc::CompatSize{320, 200};
    options.dpRatio = 1.0f;
    EffectRuntime& runtime = EffectRuntime::get();
    const size_t liveBefore = runtime.liveInstanceCount();
    const size_t statesBefore = runtime.elementStateCount();

    Counters baseline;
    for (int cycle = 0; cycle < 100; cycle++){
        std::shared_ptr<const hc::CompatDocument> compiled;
        Rml::ElementDocument* document = hc::DocumentLoader::load(r.context, path, options, &compiled);
        ASSERT_NE(document, nullptr) << "cycle " << cycle;
        document->Show();
        runtime.setTestPointer(r.context, Rml::Vector2f(80.f, 150.f), false, 0.1f);
        ASSERT_TRUE(r.renderUntilDrawn()) << "cycle " << cycle;
        if (cycle == 0){
            // every effect drew: six instances (the text fill is the stripes' filter form)
            EXPECT_GE(runtime.liveInstanceCount() - liveBefore, 6u);
            EXPECT_GT(runtime.elementStateCount(), statesBefore);
        }
        runtime.clearTestInput();
        document->Close();
        r.context->Update();
        ASSERT_TRUE(r.renderFrame()) << "cycle " << cycle;
        EXPECT_EQ(runtime.liveInstanceCount(), liveBefore) << "cycle " << cycle << ": effect instances outlive their document";
        EXPECT_EQ(runtime.elementStateCount(), statesBefore) << "cycle " << cycle << ": element states outlive their document";
        if (cycle == 0)
            baseline = countersOf(r);
        else if (cycle == 99){
            EXPECT_EQ(countersOf(r), baseline) << "the GPU objects grew over 100 cycles";
        }
    }
    hc::VirtualFiles::remove(path);
    EXPECT_EQ(runtime.liveInstanceCount(), 0u);
    EXPECT_EQ(runtime.elementStateCount(), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}

TEST(EffectLifecycleTest, HundredCyclesOfEveryStatefulEffect){
    TestSystem::get().clearLogs();
    if (!GpuEnvironment::ensure())
        GTEST_SKIP() << "no GPU";
    RenderTestContext r(320, 200, 0, 0, 1);
    if (!r.isValid())
        GTEST_SKIP() << "no GPU";
    // every built-in stateful effect, one small element each (a filter over a coloured child)
    std::string css = "html, body { margin: 0; width: 320px; height: 200px; background: #101218; }\n"
                      "div.s { position: absolute; width: 50px; height: 60px; }\n"
                      "div.s div { position: absolute; left: 10px; top: 10px; width: 30px; height: 30px; background: #e74c3c; }\n";
    std::string body;
    int count = 0;
    std::vector<std::string> effects;
    for (const EffectManifest* m : EffectRegistry::get().builtins()){
        if (!m->stateful())
            continue;
        ASSERT_TRUE(warmUpEffect(*m)) << m->name;
        const std::string id = "s" + std::to_string(count);
        // Keep populations small while exercising actual agents, passes and state-image lifetimes.
        std::string parameters;
        if (const EffectParamDef* population = m->findParam("count"); population && population->hasRange)
            parameters = ", count " + std::to_string(std::max(1, (int)std::ceil(population->minValue)));
        if (m->name == "reaction-diffusion") parameters += ", speed 1";
        if (m->name == "fluid") parameters += ", pressure-iterations 1";
        const std::string call = "effect(" + m->name + parameters + ")";
        const std::string decl = m->kind == EffectKind::Paint ? "background-image: " + call
                                                              : std::string(m->backdrop ? "backdrop-filter" : "filter") + ": " + call;
        effects.push_back(m->name);
        css += "#" + id + " { left: " + std::to_string(4 + (count % 6) * 52) + "px; top: " + std::to_string(4 + (count / 6) * 64) + "px; " + decl + "; }\n";
        body += "<div class=\"s\" id=\"" + id + "\"><div></div></div>";
        count++;
    }
    ASSERT_GE(count, 18) << "the phase-6 catalogue has 18 stateful effects";
    const std::string path = "@htmlui/test/effects/lifecycle-stateful.html";
    hc::VirtualFiles::add(path, "<!DOCTYPE html><html><head><style>" + css + "</style></head><body>" + body + "</body></html>");
    hc::CompatOptions options;
    options.viewport = hc::CompatSize{320, 200};
    options.dpRatio = 1.0f;
    EffectRuntime& runtime = EffectRuntime::get();
    EffectStatePool& pool = EffectStatePool::get();
    const size_t liveBefore = runtime.liveInstanceCount();
    const size_t imagesBefore = pool.liveImages();
    // the soft budget (8 a document) is not what this test is about
    EffectStatefulBudget budget;
    budget.perDocument = 64;
    budget.global = 64;
    budget.bytesPerDocument = 1024u * 1024u * 1024u;
    runtime.setStatefulBudget(budget);

    Counters baseline;
    uint64_t created = 0;
    uint64_t documentBytes = 0;
    // Control the live clock explicitly; the effect clock has a separate pinned override.
    EffectClock::setFixedTime(std::nullopt);
    HtmlUIClock::setFixedTime(0.0);
    double clock = 0.0;
    for (int cycle = 0; cycle < 100; cycle++){
        std::shared_ptr<const hc::CompatDocument> compiled;
        Rml::ElementDocument* document = hc::DocumentLoader::load(r.context, path, options, &compiled);
        ASSERT_NE(document, nullptr) << "cycle " << cycle;
        document->Show();
        runtime.setTestPointer(r.context, Rml::Vector2f(80.f, 50.f), false, 0.1f);
        ASSERT_TRUE(r.renderUntilDrawn()) << "cycle " << cycle;
        // Finish any warm-up split across frames before advancing the live simulations.
        for (int frame = 0; frame < 30 && runtime.pendingReplays() != 0; frame++)
            ASSERT_TRUE(r.renderFrame()) << "cycle " << cycle;
        ASSERT_EQ(runtime.pendingReplays(), 0u) << "cycle " << cycle;
        // a live step
        clock += 1.0 / 60.0;
        HtmlUIClock::setFixedTime(clock);
        ASSERT_TRUE(r.renderFrame()) << "cycle " << cycle;
        for (size_t i = 0; i < effects.size(); i++){
            const EffectSimulationInfo simulation = runtime.simulationInfo(document->GetElementById("s" + std::to_string(i)), effects[i], 0);
            EXPECT_TRUE(simulation.exists) << effects[i] << " cycle " << cycle;
            EXPECT_GT(simulation.steps, 0) << effects[i] << " did not step in cycle " << cycle;
        }
        if (cycle == 0){
            EXPECT_EQ(runtime.simulationCount(), (size_t)count) << "every stateful effect simulates";
            documentBytes = runtime.stats().stateBytes;
        }
        runtime.clearTestInput();
        document->Close();
        r.context->Update();
        ASSERT_TRUE(r.renderFrame()) << "cycle " << cycle;
        EXPECT_EQ(runtime.liveInstanceCount(), liveBefore) << "cycle " << cycle << ": effect instances outlive their document";
        EXPECT_EQ(runtime.simulationCount(), 0u) << "cycle " << cycle << ": simulations outlive their document";
        EXPECT_EQ(pool.liveImages(), imagesBefore) << "cycle " << cycle << ": state images outlive their document";
        if (cycle == 0){
            baseline = countersOf(r);
            created = pool.createdImages();
        }else if (cycle == 99){
            EXPECT_EQ(countersOf(r), baseline) << "the GPU objects grew over 100 cycles";
            // the pool keeps at most 64 MB: a document with more state than that makes the excess
            // again each cycle (every stateful effect at once has more)
            if (documentBytes <= EffectStatePool::kMaxPooledBytes){
                EXPECT_EQ(pool.createdImages(), created) << "the pool made images after the first cycle";
            }
            std::printf("[ effects  ] every stateful effect: %.1f MB of state images; the pool made %llu images in the first cycle, %llu "
                        "in the 99 others\n", (double)documentBytes / (1024.0 * 1024.0), (unsigned long long)created,
                        (unsigned long long)(pool.createdImages() - created));
        }
    }
    hc::VirtualFiles::remove(path);
    EXPECT_LE(pool.pooledBytes(), EffectStatePool::kMaxPooledBytes);
    pool.trim(0);
    EXPECT_EQ(pool.pooledImages(), 0u) << "trim(0) destroys every pooled image";
    runtime.setStatefulBudget(EffectStatefulBudget());
    HtmlUIClock::setFixedTime(0.0);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}
