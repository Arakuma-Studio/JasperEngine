// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// S95 REQ-13, D-7: the fixed step across scene loads. Project Settings > Time sets the step before
// the first scene; a load into a scene with its own Fixed Timestep applies it before that scene's
// init, a load into one without brings the project's back, and otherwise the step a script set
// stays (Unity keeps Time.fixedDeltaTime across loads). Layers never change it. The scenes are
// built by SceneManager factories, the way the generated main of an exported game builds them.

#include "ScriptTestHarness.h"

#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::scripttest;

namespace {

    class EngineTimeSettingsTest : public ScriptTest {
    protected:
        void SetUp() override {
            ScriptTest::SetUp();
            // logs the step its init reads, as 1/step
            writeScript("StepProbe",
                "local M = {}\n"
                "function M:init() log[#log + 1] = self.scene:getEntityName(self.entity) .. ':' .. "
                "string.format('%.0f', 1 / Time.fixedDeltaTime) end\n"
                "return M\n");
        }

        void TearDown() override {
            Engine::setProjectUpdateTime(1.0 / 60.0);
            ScriptTest::TearDown();
        }

        // a scene stack of one scene with a StepProbe on an entity named `name`
        void registerScene(uint32_t id, const std::string& name, float fixedTimestep) {
            SceneManager::registerScene(id, name, [this, id, name, fixedTimestep]() {
                Scene* scene = newScene();
                scene->setFixedTimestep(fixedTimestep);
                SceneManager::setScenePtr(id, scene);
                addScript(scene, entity(scene, name), "StepProbe");
                LuaBinding::initializeLuaScripts(scene);
                Engine::setScene(scene);
            });
        }

        // the load of a running game happens at the next frame
        void load(const std::string& name) {
            ASSERT_TRUE(SceneManager::loadScene(name));
            frame();
        }

        double hz() const { return 1.0 / Engine::getUpdateTime(); }
    };

}

TEST_F(EngineTimeSettingsTest, SceneOverrideAppliesOnLoadAndTheProjectStepComesBack) {
    Engine::setProjectUpdateTime(1.0 / 50.0);
    EXPECT_EQ(Engine::getUpdateTime(), (float)(1.0 / 50.0));
    registerScene(1, "Menu", 0.0f);
    registerScene(2, "C", 1.0f / 400.0f);
    registerScene(3, "D", 0.0f);

    ASSERT_TRUE(SceneManager::loadScene("Menu"));
    EXPECT_EQ(takeLog(), "Menu:50");

    load("C");
    EXPECT_EQ(takeLog(), "C:400") << "the override is the step C's init reads";
    EXPECT_NEAR(hz(), 400.0, 1e-3);

    // a layer with its own step changes nothing
    SceneManager::registerScene(4, "Layer", [this]() {
        Scene* layer = newScene();
        layer->setFixedTimestep(1.0f / 30.0f);
        SceneManager::setScenePtr(4, layer);
        LuaBinding::initializeLuaScripts(layer);
    }, [this]() {
        Scene* layer = newScene();
        layer->setFixedTimestep(1.0f / 30.0f);
        SceneManager::setScenePtr(4, layer);
        LuaBinding::initializeLuaScripts(layer);
    }, {4});
    ASSERT_TRUE(SceneManager::addChildScene("Layer"));
    frame();
    EXPECT_NEAR(hz(), 400.0, 1e-3) << "an added scene never changes the step";

    load("Menu");
    EXPECT_EQ(takeLog(), "Menu:50") << "a scene without one after C: the project's step";

    // a script's step survives a load into a scene without an override (Unity)
    lua("Time.fixedDeltaTime = 1 / 120");
    load("D");
    EXPECT_EQ(takeLog(), "D:120");
    EXPECT_NEAR(hz(), 120.0, 1e-2);

    // and a scene loaded again applies its step again
    load("C");
    load("C");
    EXPECT_EQ(takeLog(), "C:400 C:400");
}

// the project step is set as a double: the default 1/60 is the engine's own step to the bit, so a
// frame of exactly 1/60 s runs exactly one fixed step
TEST_F(EngineTimeSettingsTest, ProjectStepKeepsTheExactDefault) {
    Engine::setProjectUpdateTime(1.0 / 60.0);
    EXPECT_EQ(Engine::getProjectUpdateTime(), 1.0 / 60.0);
    Scene* scene = newScene();
    Engine::setScene(scene);
    int steps = 0;
    Engine::onFixedUpdate.add("timesettings", std::function<void()>([&]() { steps++; }));
    for (int i = 0; i < 10; i++) frame();
    Engine::onFixedUpdate.remove("timesettings");
    EXPECT_EQ(steps, 10);
}
