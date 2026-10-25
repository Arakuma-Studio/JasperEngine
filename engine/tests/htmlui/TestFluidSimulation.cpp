// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The CPU side of the fluid simulation feature (not HTML UI: engine-level coverage hosted by
// this harness): FluidPresets (apply, match, the fields they list), the FluidSimulation
// wrapper (validation, queued splats/bursts/steps, presets), EntityCloner's reset of the
// runtime state and the Lua API (FluidSimulation, FluidOutput, FluidSimulationComponent).
// The GPU side (FluidSimulator, FluidSystem) is tested with it.

#include "harness/GpuEnvironment.h"
#include "harness/TestSystem.h"

#include "Scene.h"
#include "component/FluidSimulationComponent.h"
#include "fluid/FluidPresets.h"
#include "fluid/FluidSimulator.h"
#include "component/MeshComponent.h"
#include "component/PointsComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/FluidSimulation.h"
#include "object/Sprite.h"
#include "registry/EntityCloner.h"
#include "subsystem/FluidSystem.h"
#include "script/LuaBinding.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // a component whose compositing and clock differ from the defaults (presets leave them)
    FluidSimulationComponent customized(){
        FluidSimulationComponent fluid;
        fluid.enabled = false;
        fluid.output = FluidOutput::TEXTURE;
        fluid.interactive = false;
        fluid.aspectRatio = 2.0f;
        fluid.simulationRate = 30.0f;
        fluid.maxSubsteps = 2;
        fluid.timeScale = 0.5f;
        fluid.paused = true;
        fluid.splatsOnStart = false;
        fluid.randomSplatCount = 7;
        fluid.seed = 1234;
        fluid.backColor = Vector3(0.1f, 0.2f, 0.3f);
        fluid.transparent = true;
        return fluid;
    }

    void expectCompositingAndClockKept(const FluidSimulationComponent& fluid, const std::string& id){
        const FluidSimulationComponent expected = customized();
        EXPECT_EQ(fluid.enabled, expected.enabled) << id;
        EXPECT_EQ(fluid.output, expected.output) << id;
        EXPECT_EQ(fluid.interactive, expected.interactive) << id;
        EXPECT_EQ(fluid.aspectRatio, expected.aspectRatio) << id;
        EXPECT_EQ(fluid.simulationRate, expected.simulationRate) << id;
        EXPECT_EQ(fluid.maxSubsteps, expected.maxSubsteps) << id;
        EXPECT_EQ(fluid.timeScale, expected.timeScale) << id;
        EXPECT_EQ(fluid.paused, expected.paused) << id;
        EXPECT_EQ(fluid.splatsOnStart, expected.splatsOnStart) << id;
        EXPECT_EQ(fluid.randomSplatCount, expected.randomSplatCount) << id;
        EXPECT_EQ(fluid.seed, expected.seed) << id;
        EXPECT_EQ(fluid.backColor, expected.backColor) << id;
        EXPECT_EQ(fluid.transparent, expected.transparent) << id;
    }

}

// ---------------------------------------------------------------- presets


TEST(FluidPresetsTest, DefaultIsPavelsConfig){
    const FluidPreset* preset = findFluidPreset("default");
    ASSERT_NE(preset, nullptr);
    const FluidSimulationComponent& v = preset->values;
    // script.js config
    EXPECT_EQ(v.simResolution, 128u);
    EXPECT_EQ(v.dyeResolution, 1024u);
    EXPECT_EQ(v.densityDissipation, 1.0f);
    EXPECT_EQ(v.velocityDissipation, 0.2f);
    EXPECT_EQ(v.pressure, 0.8f);
    EXPECT_EQ(v.pressureIterations, 20u);
    EXPECT_EQ(v.curl, 30.0f);
    EXPECT_EQ(v.gradientScale, 1.0f);
    EXPECT_EQ(v.splatRadius, 0.25f);
    EXPECT_EQ(v.splatForce, 6000.0f);
    EXPECT_TRUE(v.shading);
    EXPECT_TRUE(v.colorful);
    EXPECT_EQ(v.colorUpdateSpeed, 10.0f);
    EXPECT_TRUE(v.bloom);
    EXPECT_EQ(v.bloomIterations, 8u);
    EXPECT_EQ(v.bloomResolution, 256u);
    EXPECT_EQ(v.bloomIntensity, 0.8f);
    EXPECT_EQ(v.bloomThreshold, 0.6f);
    EXPECT_EQ(v.bloomSoftKnee, 0.7f);
    EXPECT_TRUE(v.sunrays);
    EXPECT_EQ(v.sunraysResolution, 196u);
    EXPECT_EQ(v.sunraysWeight, 1.0f);

    // a new component is the default look
    EXPECT_EQ(matchFluidPreset(FluidSimulationComponent()), "default");
}

TEST(FluidPresetsTest, MobileIsPavelsFallback){
    const FluidPreset* preset = findFluidPreset("mobile");
    ASSERT_NE(preset, nullptr);
    EXPECT_EQ(preset->values.dyeResolution, 512u);
    EXPECT_FALSE(preset->values.shading);
    EXPECT_FALSE(preset->values.bloom);
    EXPECT_FALSE(preset->values.sunrays);
    EXPECT_EQ(preset->values.simResolution, 128u);
    EXPECT_EQ(preset->values.curl, 30.0f);
}

TEST(FluidPresetsTest, ApplySetsOnlyItsFieldsAndMatches){
    for (const FluidPreset& preset : getFluidPresets()){
        FluidSimulationComponent fluid = customized();
        ASSERT_TRUE(applyFluidPreset(fluid, preset.id));
        EXPECT_EQ(matchFluidPreset(fluid), preset.id);
        expectCompositingAndClockKept(fluid, preset.id);

        // from any other preset, the same result
        for (const FluidPreset& before : getFluidPresets()){
            FluidSimulationComponent other = customized();
            ASSERT_TRUE(applyFluidPreset(other, before.id));
            ASSERT_TRUE(applyFluidPreset(other, preset.id));
            EXPECT_EQ(matchFluidPreset(other), preset.id) << before.id << " then " << preset.id;
        }
    }
}

TEST(FluidPresetsTest, AnyChangedFieldIsCustom){
    FluidSimulationComponent fluid;
    ASSERT_TRUE(applyFluidPreset(fluid, "ink"));
    ASSERT_EQ(matchFluidPreset(fluid), "ink");
    fluid.curl += 0.001f;
    EXPECT_EQ(matchFluidPreset(fluid), "");
    ASSERT_TRUE(applyFluidPreset(fluid, "ink"));
    fluid.bloomIterations = 3;     // a field of a disabled effect still counts
    EXPECT_EQ(matchFluidPreset(fluid), "");

    // a field no preset sets does not
    ASSERT_TRUE(applyFluidPreset(fluid, "psychedelic"));
    fluid.backColor = Vector3(1.0f, 1.0f, 1.0f);
    fluid.seed = 99;
    EXPECT_EQ(matchFluidPreset(fluid), "psychedelic");


}

TEST(FluidPresetsTest, UnknownPresetChangesNothing){
    FluidSimulationComponent fluid;
    fluid.curl = 12.0f;
    EXPECT_FALSE(applyFluidPreset(fluid, "lava"));
    EXPECT_EQ(fluid.curl, 12.0f);
    EXPECT_EQ(matchFluidPreset(fluid), "");
}

// ---------------------------------------------------------------- the wrapper

namespace {

    class FluidSimulationTest : public ::testing::Test {
    protected:
        void SetUp() override{
            ASSERT_TRUE(GpuEnvironment::ensure()) << "no headless GPU context";
            TestSystem::get().clearLogs();
            scene.reset(new Scene());
        }

        void TearDown() override{
            scene.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << "errors logged";
        }

        // the errors logged since the last call (then cleared)
        size_t takeErrors(){
            size_t count = TestSystem::get().countLogs(D_LOG_ERROR);
            TestSystem::get().clearLogs();
            return count;
        }

        std::unique_ptr<Scene> scene;
    };

}


TEST_F(FluidSimulationTest, SettersRejectWhatTheSolverCannotUse){
    FluidSimulation fluid(scene.get());
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    fluid.setCurl(nan);
    EXPECT_EQ(fluid.getCurl(), 30.0f);
    fluid.setDensityDissipation(-1.0f);
    EXPECT_EQ(fluid.getDensityDissipation(), 1.0f);
    fluid.setSplatForce(inf);
    EXPECT_EQ(fluid.getSplatForce(), 6000.0f);
    fluid.setSplatRadius(0.0f);
    EXPECT_EQ(fluid.getSplatRadius(), 0.25f);
    fluid.setSimulationRate(0.0f);
    EXPECT_EQ(fluid.getSimulationRate(), 60.0f);
    fluid.setSimResolution(0);
    EXPECT_EQ(fluid.getSimResolution(), 128u);
    fluid.setDyeResolution(0);
    EXPECT_EQ(fluid.getDyeResolution(), 1024u);
    fluid.setMaxSubsteps(0);
    EXPECT_EQ(fluid.getMaxSubsteps(), 4u);
    fluid.setBackColor(Vector3(nan, 0.0f, 0.0f));
    EXPECT_EQ(fluid.getBackColor(), Vector3(0.0f, 0.0f, 0.0f));
    fluid.setAspectRatio(-2.0f);
    EXPECT_EQ(fluid.getAspectRatio(), 0.0f);
    EXPECT_EQ(takeErrors(), 10u);

    // zero where it means something
    fluid.setTimeScale(0.0f);
    fluid.setAspectRatio(0.0f);
    fluid.setVelocityDissipation(0.0f);
    fluid.setPressure(0.0f);
    EXPECT_EQ(fluid.getTimeScale(), 0.0f);
    EXPECT_EQ(fluid.getVelocityDissipation(), 0.0f);
    EXPECT_EQ(takeErrors(), 0u);
}


// ---------------------------------------------------------------- Lua


TEST_F(FluidSimulationTest, ClonesRemapEveryTextureSlot){
    // every texture slot showing a copied camera's or fluid's framebuffer shows the copy's,
    // its link id naming the copy
    FluidSimulation fluid(scene.get());
    fluid.setOutput(FluidOutput::TEXTURE);
    Framebuffer* output = fluid.getTexture().getFramebuffer();
    const Entity fluidEntity = fluid.getEntity();
    auto linked = [&](){
        Texture texture(output);
        texture.setId("fluid|" + std::to_string(fluidEntity));
        return texture;
    };
    Sprite sprite(scene.get());
    fluid.addChild(&sprite);
    MeshComponent& mesh = sprite.getComponent<MeshComponent>();
    mesh.submeshes[0].material.baseColorTexture = linked();
    mesh.submeshes[0].material.shaderTextures.push_back({"u_custom", linked()});
    mesh.submeshes[0].material.extraPass.edit().shaderTextures.push_back({"u_outline", linked()});
    // other components on other children
    const Entity pointsEntity = scene->createEntity();
    scene->addComponent<Transform>(pointsEntity, {});
    PointsComponent points;
    points.texture = linked();
    points.shaderTextures.push_back({"u_points", linked()});
    scene->addComponent<PointsComponent>(pointsEntity, points);
    fluid.addChild(pointsEntity);
    const Entity terrainEntity = scene->createEntity();
    scene->addComponent<Transform>(terrainEntity, {});
    TerrainComponent terrain;
    terrain.heightMap = linked();
    terrain.blendMaps.push_back(linked());
    scene->addComponent<TerrainComponent>(terrainEntity, terrain);
    fluid.addChild(terrainEntity);

    std::unordered_map<Entity, Entity> map;
    std::vector<Entity> copies = EntityCloner::clone(scene.get(), EntityCloner::collectHierarchy(scene.get(), fluidEntity), scene.get(), NULL_ENTITY, &map);
    ASSERT_EQ(copies.size(), 4u);
    const Entity fluidCopy = map[fluidEntity];
    FluidSimulationComponent& copy = scene->getComponent<FluidSimulationComponent>(fluidCopy);
    ASSERT_NE(copy.simulator, nullptr);
    Framebuffer* copyOutput = copy.simulator->getOutputFramebuffer();
    const std::string copyId = "fluid|" + std::to_string(fluidCopy);
    auto expectCopy = [&](const Texture& texture, const char* slot){
        EXPECT_EQ(texture.getFramebuffer(), copyOutput) << slot;
        EXPECT_EQ(texture.getId(), copyId) << slot;
    };
    const Submesh& submesh = scene->getComponent<MeshComponent>(map[sprite.getEntity()]).submeshes[0];
    expectCopy(submesh.material.baseColorTexture, "base colour");
    expectCopy(submesh.material.shaderTextures[0].second, "material shader texture");
    expectCopy(submesh.material.extraPass->shaderTextures[0].second, "extra pass texture");
    const PointsComponent& copiedPoints = scene->getComponent<PointsComponent>(map[pointsEntity]);
    expectCopy(copiedPoints.texture, "points");
    expectCopy(copiedPoints.shaderTextures[0].second, "points shader texture");
    const TerrainComponent& copiedTerrain = scene->getComponent<TerrainComponent>(map[terrainEntity]);
    expectCopy(copiedTerrain.heightMap, "height map");
    expectCopy(copiedTerrain.blendMaps[0], "blend map");
    // the originals keep theirs
    EXPECT_EQ(mesh.submeshes[0].material.shaderTextures[0].second.getFramebuffer(), output);
    EXPECT_EQ(scene->getComponent<TerrainComponent>(terrainEntity).heightMap.getId(), "fluid|" + std::to_string(fluidEntity));

    for (auto it = copies.rbegin(); it != copies.rend(); ++it)
        scene->destroyEntity(*it);
    scene->destroyEntity(terrainEntity);
    scene->destroyEntity(pointsEntity);
}


TEST_F(FluidSimulationTest, QueuesAreBounded){
    // a disabled fluid while a script splats every frame keeps the latest splats only
    FluidSimulation fluid(scene.get());
    fluid.setEnabled(false);
    for (int i = 0; i < 3000; i++)
        fluid.splat(Vector2((float)i / 3000.0f, 0.5f), Vector2(0.0f, 0.0f), Vector3(0.1f, 0.1f, 0.1f));
    FluidSimulationComponent& c = fluid.getComponent();
    ASSERT_EQ(c.pendingSplats.size(), FLUID_MAX_PENDING_SPLATS);
    EXPECT_FLOAT_EQ(c.pendingSplats.back().position.x, 2999.0f / 3000.0f) << "the latest are kept";
    EXPECT_FLOAT_EQ(c.pendingSplats.front().position.x, (float)(3000 - FLUID_MAX_PENDING_SPLATS) / 3000.0f);
    for (int i = 0; i < 100; i++)
        fluid.randomSplats(4000000000u);
    ASSERT_EQ(c.pendingBursts.size(), FLUID_MAX_PENDING_BURSTS);
    EXPECT_EQ(c.pendingBursts.back(), FLUID_MAX_BURST_SPLATS);

    // raw C++ pushes are trimmed by the system while the fluid does not run
    for (int i = 0; i < 5000; i++)
        c.pendingSplats.push_back(FluidSplat());
    for (int i = 0; i < 500; i++)
        c.pendingBursts.push_back(3);
    scene->getSystem<FluidSystem>()->update(1.0 / 60.0);
    EXPECT_EQ(c.pendingSplats.size(), FLUID_MAX_PENDING_SPLATS);
    EXPECT_EQ(c.pendingBursts.size(), FLUID_MAX_PENDING_BURSTS);
    EXPECT_EQ(c.simulator, nullptr) << "a disabled fluid makes nothing";
}

namespace {

    class FluidSimulationLuaTest : public FluidSimulationTest {
    protected:
        void SetUp() override{
            FluidSimulationTest::SetUp();
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
            ASSERT_TRUE(luabridge::push(L, scene.get()));
            lua_setglobal(L, "fluidScene");
        }

        void TearDown() override{
            lua("fluidScene = nil; fluid = nil; result = nil; collectgarbage(); collectgarbage()");
            FluidSimulationTest::TearDown();
        }

        std::string lua(const std::string& code){
            if (luaL_loadstring(L, code.c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK){
                std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
                lua_pop(L, 1);
                return error;
            }
            return std::string();
        }

        // a Lua expression's value as a string (tostring)
        std::string luaValue(const std::string& expression){
            EXPECT_EQ(lua("result = tostring(" + expression + ")"), "") << expression;
            lua_getglobal(L, "result");
            std::string value = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            return value;
        }

        Entity luaEntity(const std::string& expression){
            EXPECT_EQ(lua("result = " + expression + ".entity"), "");
            lua_getglobal(L, "result");
            Entity entity = static_cast<Entity>(lua_tointeger(L, -1));
            lua_pop(L, 1);
            return entity;
        }

        lua_State* L = nullptr;
    };

}

TEST_F(FluidSimulationLuaTest, ScriptsDriveAFluid){
    ASSERT_EQ(lua(R"(
        fluid = FluidSimulation(fluidScene)
        fluid.output = FluidOutput.SCREEN
        fluid.curl = 12
        fluid.dyeResolution = 512
        fluid.bloom = false
        fluid.seed = 77
        fluid.backColor = Vector3(0.5, 0.25, 0)
        fluid:setBackColor(0.5, 0.25, 0.125)
        fluid:splat(0.5, 0.5, 100, 0, 1, 0.2, 0.1)
        fluid:splat(Vector2(0.1, 0.9), Vector2(0, -50), Vector3(0, 0, 1))
        fluid:splat(Vector2(0.9, 0.1), Vector2(5, 5), Vector3(0, 1, 0), 0.6)
        fluid:randomSplats()
        fluid:randomSplats(8)
        fluid:step()
        fluid:step(5)
        fluid:reset()
    )"), "");
    const Entity entity = luaEntity("fluid");
    ASSERT_TRUE(scene->isEntityCreated(entity));
    FluidSimulationComponent& component = scene->getComponent<FluidSimulationComponent>(entity);
    EXPECT_EQ(component.output, FluidOutput::SCREEN);
    EXPECT_EQ(component.curl, 12.0f);
    EXPECT_EQ(component.dyeResolution, 512u);
    EXPECT_FALSE(component.bloom);
    EXPECT_EQ(component.seed, 77u);
    EXPECT_EQ(component.backColor, Vector3(0.5f, 0.25f, 0.125f));
    ASSERT_EQ(component.pendingSplats.size(), 3u);
    EXPECT_EQ(component.pendingSplats[0].velocity, Vector2(100.0f, 0.0f));
    EXPECT_EQ(component.pendingSplats[1].position, Vector2(0.1f, 0.9f));
    EXPECT_EQ(component.pendingSplats[1].radius, 0.0f);
    EXPECT_EQ(component.pendingSplats[2].radius, 0.6f);
    EXPECT_EQ(component.pendingBursts, (std::vector<unsigned int>{0u, 8u}));
    EXPECT_EQ(component.pendingSteps, 6u);
    EXPECT_TRUE(component.needReset);

    // reads back
    EXPECT_EQ(luaValue("fluid.output == FluidOutput.SCREEN"), "true");
    EXPECT_EQ(luaValue("fluid.curl"), "12.0");
    EXPECT_EQ(luaValue("fluid.backColor.z"), "0.125");
    EXPECT_EQ(luaValue("FluidOutput.TEXTURE"), "2");

    // presets
    EXPECT_EQ(luaValue("fluid.preset"), "");
    EXPECT_EQ(luaValue("fluid:applyPreset('calm-water')"), "true");
    EXPECT_EQ(luaValue("fluid.preset"), "calm-water");
    EXPECT_EQ(luaValue("fluid:getPreset()"), "calm-water");
    EXPECT_EQ(luaValue("fluid:applyPreset('lava')"), "false");
    EXPECT_EQ(luaValue("#FluidSimulation.getPresetIds()"), "6");
    EXPECT_EQ(luaValue("FluidSimulation.getPresetIds()[1]"), "default");
    EXPECT_EQ(component.curl, 5.0f);
    EXPECT_EQ(component.seed, 77u) << "a preset changed the seed";

    // the component, stats
    EXPECT_EQ(luaValue("fluid:getFluidSimulationComponent().curl"), "5.0");
    EXPECT_EQ(lua("fluid:getFluidSimulationComponent().pressureIterations = 9"), "");
    EXPECT_EQ(component.pressureIterations, 9u);
    EXPECT_EQ(luaValue("fluid:getFluidSimulationComponent().output == FluidOutput.SCREEN"), "true");
    EXPECT_EQ(luaValue("fluid:getStats().steps"), "0");

    // a rejected value is logged and kept
    EXPECT_EQ(lua("fluid.splatRadius = -1"), "");
    EXPECT_EQ(component.splatRadius, 0.4f);
    EXPECT_EQ(takeErrors(), 1u);

    // the Lua handle owns the entity it made
    ASSERT_EQ(lua("fluid = nil; collectgarbage(); collectgarbage()"), "");
    EXPECT_FALSE(scene->isEntityCreated(entity));
}

TEST_F(FluidSimulationLuaTest, ScriptPropertiesHoldAFluid){
    // a script property typed FluidSimulation is pushed as one
    FluidSimulation fluid(scene.get());
    fluid.setCurl(3.0f);
    // what a script property of type FluidSimulation is resolved with (no script on the entity)
    ASSERT_TRUE(LuaBinding::pushScriptObject(L, scene.get(), fluid.getEntity(), "FluidSimulation"));
    lua_setglobal(L, "fluid");
    EXPECT_EQ(luaValue("fluid.curl"), "3.0");
    EXPECT_EQ(lua("fluid:randomSplats(4)"), "");
    EXPECT_EQ(fluid.getComponent().pendingBursts, (std::vector<unsigned int>{4u}));
    EXPECT_EQ(luaValue("fluid.entityOwned"), "false");
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "unknown type"), 0u);
}
