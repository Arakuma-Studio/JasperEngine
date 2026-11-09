// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86 R37, R63 (AC32): Realtime 3D through S81's TitleArt. Lua's qualityTier round trips and
// refuses other values, getMeshStats raises on a Baked title and is nil before a render; the tier
// a title renders at (a named one, else the project's Realtime 3D Quality) goes into its state and
// its hash, and a 2D title has none. On the headless device a Realtime 3D title reports its mesh
// (triangles, vertices, solids, tier, overBudget) after its render, and an Exposed "spin" bound to
// transform.rotation_y re-renders it without a new mesh (the 3D renderer uploads none; red
// control: a depth change uploads one).

#include "titleart/render/TitleRenderTestSupport.h"
#include "titleart/support/TitleTestAssets.h"

#include "Scene.h"
#include "component/TitleArtRendererComponent.h"
#include "component/Transform.h"
#include "device/QualityResolver.h"
#include "script/LuaBinding.h"
#include "subsystem/TitleArtRuntime.h"
#include "titleart/runtime/TitleArtLive.h"
#include "titleart/runtime/TitleArtScheduler.h"
#include "titleart/three_d/Title3DRenderer.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <chrono>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::test;

namespace {

    std::string runLua(lua_State* L, const std::string& code) {
        if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
            std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
            lua_pop(L, 1);
            return error;
        }
        return std::string();
    }

    // a gold 3D plate whose Exposed "spin" turns it about Y and "depth" changes its geometry
    std::shared_ptr<const TitleArtDoc> spinDoc() {
        Json layer = shapeLayer("00000001", "rect", 120, solidFace("#ff0000"), Json{{"aspect", 2.5}, {"dimension", "3d"}, {"name", "Logo"}});
        layer["style_3d"] = Json{{"face", materialPreset("gold")}, {"bevel", {{"width", 10}, {"depth", 5}, {"profile", "round"}}}};
        Json exposed = Json::array({
            {{"name", "spin"}, {"type", "number"}, {"targets", Json::array({{{"layer", "00000001"}, {"field", "transform.rotation_y"}}})}},
            {{"name", "depth"}, {"type", "number"}, {"targets", Json::array({{{"layer", "00000001"}, {"field", "style_3d.depth"}}})}}});
        return makeDoc({{"canvas", canvas(512, 256)}, {"layers", Json::array({layer})}, {"exposed", exposed}, {"scene_3d", {{"supersample", 1}}}});
    }

    TitleArtOverride number(const char* name, float value) {
        TitleArtOverride o;
        o.name = name;
        o.type = "number";
        o.value = Vector4(value, 0, 0, 0);
        return o;
    }

    // frames of the scheduler over one live until it has no work
    void runFrames(TitleArtLive& live, const TitleJobFactory& factory) {
        TitleLiveLimits limits;
        double clock = 0.0;
        const auto start = std::chrono::steady_clock::now();
        while (live.hasWork() && std::chrono::steady_clock::now() - start < std::chrono::minutes(3)) {
            SystemRender::updateGpuCreation();
            RuntimeShaderCompiler::beginFrame();
            TitleArtScheduler::schedule({&live}, factory, 1000000000u, 1.0);
            SystemRender::commit();
            size_t cache = 0;
            clock += 1.0 / 60.0;
            live.endFrame(clock, 1.0 / 60.0, limits, cache);
        }
    }

    void setState(TitleArtLive& live, const std::vector<TitleArtOverride>& overrides, const std::string& qualityTier) {
        TitleArtLiveState s;
        s.label = "TitleArt/Test";
        s.overrides = overrides;
        s.qualityTier = qualityTier;
        s.tier = TitleArtRuntime::tierFor(live, qualityTier);
        TitleArtRuntime::setState(live, s);
    }

    class TitleArt3DLiveGpu : public ::testing::Test {
    protected:
        void SetUp() override { JASPER_REQUIRE_GPU(); }
    };

}

TEST(TitleArt3DLua, QualityTierAndMeshStats) {
    lua_State* L = LuaBinding::getLuaState();
    ASSERT_NE(L, nullptr);
    Scene scene;
    const Entity realtime = scene.createEntity();
    scene.addComponent<Transform>(realtime, {});
    scene.setEntityName(realtime, "Logo");
    TitleArtRendererComponent live;
    live.title = AssetRef("titles/logo.titleart");
    live.renderMode = TitleArtRenderMode::REALTIME;
    scene.addComponent<TitleArtRendererComponent>(realtime, std::move(live));
    const Entity baked = scene.createEntity();
    scene.addComponent<Transform>(baked, {});
    scene.setEntityName(baked, "Badge");
    TitleArtRendererComponent plain;
    plain.title = AssetRef("titles/badge.titleart");
    scene.addComponent<TitleArtRendererComponent>(baked, std::move(plain));

    luabridge::setGlobal(L, &scene, "titleScene");
    lua_pushinteger(L, realtime);
    lua_setglobal(L, "REALTIME");
    lua_pushinteger(L, baked);
    lua_setglobal(L, "BAKED");
    EXPECT_EQ(runLua(L, R"(
        local t = TitleArt(titleScene, REALTIME)
        assert(t.qualityTier == "auto", "auto by default")
        t.qualityTier = "low"
        assert(t.qualityTier == "low", "round trip")
        local ok, err = pcall(function() t.qualityTier = "ultra" end)
        assert(not ok and string.find(err, "auto, high, medium or low", 1, true), tostring(err))
        assert(t.qualityTier == "low", "a refused value changes nothing")
        assert(t:getMeshStats("Logo") == nil, "no render yet")
        local b = TitleArt(titleScene, BAKED)
        local okBaked, errBaked = pcall(function() return b:getMeshStats("Logo") end)
        assert(not okBaked and string.find(errBaked, "Baked", 1, true), tostring(errBaked))
    )"), "");
    EXPECT_EQ(scene.getComponent<TitleArtRendererComponent>(realtime).qualityTier, "low");
}

TEST(TitleArt3DLua, TheTierGoesIntoTheState) {
    TitleArtLive live3D("titles/spin.titleart", TitleArtRenderMode::REALTIME, spinDoc(), 1);
    EXPECT_TRUE(live3D.has3D());
    EXPECT_EQ(TitleArtRuntime::tierFor(live3D, "low"), (int)TitleTier::Low);
    EXPECT_EQ(TitleArtRuntime::tierFor(live3D, "medium"), (int)TitleTier::Medium);
    // auto: the project's Realtime 3D Quality for this platform (the scheduler's settings) where no
    // device tier applies
    TitleArtRuntimeSettings settings = TitleArtScheduler::get().settings();
    const TitleArtRuntimeSettings before = settings;
    for (int& tier : settings.realtime3DTier) tier = 2;
    TitleArtScheduler::get().configure(settings);
    if (!QualityResolver::getTier().has_value()) EXPECT_EQ(TitleArtRuntime::tierFor(live3D, "auto"), (int)TitleTier::Medium);
    TitleArtScheduler::get().configure(before);
    TitleArtLiveState low, high;
    low.tier = 3;
    high.tier = 1;
    EXPECT_NE(TitleArtRuntime::stateHashOf(live3D, low), TitleArtRuntime::stateHashOf(live3D, high));
    EXPECT_NE(TitleArtRuntime::prebakeKeyOf(live3D, low), TitleArtRuntime::prebakeKeyOf(live3D, high)) << "a prebake per tier";

    auto flat = makeDoc({{"canvas", canvas(64, 64)}, {"layers", Json::array({shapeLayer("00000001", "rect", 20, solidFace("#ffffff"))})}});
    TitleArtLive live2D("titles/flat.titleart", TitleArtRenderMode::REALTIME, flat, 2);
    EXPECT_FALSE(live2D.has3D());
    EXPECT_EQ(TitleArtRuntime::tierFor(live2D, "low"), 0) << "a 2D title has no tier: its keys stay as before";
}

TEST_F(TitleArt3DLiveGpu, MeshStatsAndASpinWithoutAMesh) {
    TitleArtRenderer renderer;
    renderer.setContentProvider(makeGeometryTitleContentProvider());
    const TitleJobFactory factory = makeRendererJobFactory(renderer, nullptr);
    TitleArtLive live("titles/spin.titleart", TitleArtRenderMode::REALTIME, spinDoc(), 3);
    setState(live, {}, "high");
    runFrames(live, factory);
    ASSERT_TRUE(live.status().ready) << live.status().error;

    TitleArtMeshStats stats;
    bool found = false;
    std::string error;
    ASSERT_TRUE(TitleArtRuntime::meshStats(live, "Logo", stats, found, &error)) << error;
    ASSERT_TRUE(found);
    EXPECT_GT(stats.triangles, 0u);
    EXPECT_GT(stats.vertices, 0u);
    EXPECT_GE(stats.solids, 1u);
    EXPECT_EQ(stats.tier, "high");
    EXPECT_FALSE(stats.overBudget);
    EXPECT_FALSE(stats.cooked);
    EXPECT_TRUE(TitleArtRuntime::meshStats(live, "00000001", stats, found, &error)) << "by id too";
    EXPECT_FALSE(TitleArtRuntime::meshStats(live, "Missing", stats, found, &error));
    EXPECT_NE(error.find("its 3D layers are Logo"), std::string::npos) << error;

    const std::shared_ptr<void> held = renderer.title3DRendererForTesting();
    ASSERT_TRUE(held);
    const Title3DStats& counters = static_cast<const Title3DRenderer*>(held.get())->stats();
    const uint64_t uploads = counters.meshUploads;
    const uint64_t renders = counters.renders;
    setState(live, {number("spin", 35.0f)}, "high");
    runFrames(live, factory);
    EXPECT_TRUE(live.status().ready);
    EXPECT_GT(counters.renders, renders) << "the spin re-renders";
    EXPECT_EQ(counters.meshUploads, uploads) << "with the mesh it had";
    // red control: a geometry parameter makes a new mesh
    setState(live, {number("spin", 35.0f), number("depth", 30.0f)}, "high");
    runFrames(live, factory);
    EXPECT_GT(counters.meshUploads, uploads);
}
