// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// 100 cycles of loading, drawing and unloading a document with the five reference effects
// (phase-5 spec 5.1): afterwards no effect instance is live, the element-state map is empty and
// the renderer's GPU objects (geometry, textures, buffers and images) are back to the baseline of
// the first cycle.

#include "EffectTestCommon.h"

#include "htmlui/compat/CompatOptions.h"
#include "htmlui/compat/DocumentLoader.h"

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
        else if (cycle == 99)
            EXPECT_EQ(countersOf(r), baseline) << "the GPU objects grew over 100 cycles";
    }
    hc::VirtualFiles::remove(path);
    EXPECT_EQ(runtime.liveInstanceCount(), 0u);
    EXPECT_EQ(runtime.elementStateCount(), 0u);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR, "[HTML UI]"), 0u) << logText(D_LOG_ERROR);
}
