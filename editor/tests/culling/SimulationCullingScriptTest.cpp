// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Culling Groups and Simulation Culling with Lua scripts on whole engine frames
// (docs/specs/culling-group-and-simulation-distance.md AC5, AC6's Lua part, AC7's scripts, AC10,
// AC12): the scripts' guard, the reduced band's Engine.deltatime, the two messages, a group's
// ownership and re-entry from its callback. The engine-side cases (bands, cameras, bodies,
// pacing) are in engine/tests/core/subsystem/CullingSystemTest.cpp.

#include "ScriptTestHarness.h"

#include "component/CameraComponent.h"
#include "component/SimulationCullingComponent.h"
#include "object/SimulationCulling.h"
#include "subsystem/CullingSystem.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace SimulationCullingScriptTest_cpp {

using namespace jasperengine;
using namespace jasperengine::scripttest;

namespace {

    class SimulationCullingScriptTest : public ScriptTest {
    protected:
        Scene* scene = nullptr;
        Entity camera = NULL_ENTITY;

        void SetUp() override {
            ScriptTest::SetUp();
            scene = newScene();
            setScene("testScene", scene);
            camera = entity(scene, "Camera");
            CameraComponent component;
            component.type = CameraType::CAMERA_PERSPECTIVE;
            scene->addComponent<CameraComponent>(camera, component);
            scene->setCamera(camera);
        }

        void TearDown() override {
            SimulationCulling::setDistanceScale(1.0f);
            ScriptTest::TearDown();
        }

        Entity culledObject(const std::string& name, const Vector3& position, float distance, float reduced = 0.0f) {
            const Entity e = entity(scene, name);
            moveTo(e, position);
            SimulationCullingComponent culling;
            culling.simulationDistance = distance;
            culling.reducedRateDistance = reduced;
            culling.hysteresis = 0.0f;
            culling.boundsRadius = 0.5f;
            culling.simulateWhenVisible = false;
            culling.freezeWhenGroundNotLoaded = false;
            scene->addComponent<SimulationCullingComponent>(e, culling);
            return e;
        }

        void moveTo(Entity e, const Vector3& position) {
            Transform& transform = scene->getComponent<Transform>(e);
            transform.position = position;
            transform.needUpdate = true;
        }

        void start() {
            LuaBinding::initializeLuaScripts(scene);
            Engine::setScene(scene);
        }

        void frames(int count) {
            for (int i = 0; i < count; i++) frame();
        }

        int number(const std::string& expression) {
            return std::stoi(lua("return tostring(math.floor(" + expression + "))"));
        }

        SimulationCullingComponent& culling(Entity e) {
            return scene->getComponent<SimulationCullingComponent>(e);
        }
    };

}

// AC7's scripts: culled, its update callbacks stop and its input events do not
TEST_F(SimulationCullingScriptTest, DistanceCullsScriptUpdates) {
    writeScript("Counter",
        "local M = {}\n"
        "function M:init() counts = {u = 0, f = 0, p = 0, k = 0}\n"
        "  RegisterEngineEvent(self, 'onUpdate'); RegisterEngineEvent(self, 'onFixedUpdate')\n"
        "  RegisterEngineEvent(self, 'onPostUpdate'); RegisterEngineEvent(self, 'onKeyDown') end\n"
        "function M:onUpdate() counts.u = counts.u + 1 end\n"
        "function M:onFixedUpdate() counts.f = counts.f + 1 end\n"
        "function M:onPostUpdate() counts.p = counts.p + 1 end\n"
        "function M:onKeyDown() counts.k = counts.k + 1 end\n"
        "return M\n");
    const Entity far = culledObject("Far", Vector3(0, 0, 100), 64.0f);
    addScript(scene, far, "Counter");
    start();
    frames(10);
    EXPECT_EQ(culling(far).state, SimulationState::Culled);
    EXPECT_EQ(number("counts.u"), 0);
    EXPECT_EQ(number("counts.f"), 0);
    EXPECT_EQ(number("counts.p"), 0);
    Engine::onKeyDown.call(65, false, 0);
    EXPECT_EQ(number("counts.k"), 1) << "input events still reach it";

    moveTo(far, Vector3(0, 0, 10));
    frames(3);
    EXPECT_EQ(culling(far).state, SimulationState::Full);
    EXPECT_GE(number("counts.u"), 2);
    EXPECT_GE(number("counts.f"), 2);
    EXPECT_GE(number("counts.p"), 2);
}

// AC10: interval 4: onUpdate on one frame in four, its Engine.deltatime the four frames' time,
// onFixedUpdate every step; neighbouring GameObjects on different frames
TEST_F(SimulationCullingScriptTest, ReducedBandAccumulatesDelta) {
    writeScript("Reduced",
        "local M = {}\n"
        "function M:init() calls[self.entity] = {}; fixed[self.entity] = 0\n"
        "  RegisterEngineEvent(self, 'onUpdate'); RegisterEngineEvent(self, 'onFixedUpdate') end\n"
        "function M:onUpdate() local c = calls[self.entity]; c[#c + 1] = {Engine.frameCount, Engine.deltatime} end\n"
        "function M:onFixedUpdate() fixed[self.entity] = fixed[self.entity] + 1 end\n"
        "return M\n");
    const Entity a = culledObject("A", Vector3(0, 0, 50), 100.0f, 20.0f);
    const Entity b = culledObject("B", Vector3(0, 0, 51), 100.0f, 20.0f);
    ASSERT_EQ(b, a + 1);
    culling(a).reducedUpdateInterval = 4;
    culling(b).reducedUpdateInterval = 4;
    addScript(scene, a, "Reduced");
    addScript(scene, b, "Reduced");
    // the Lua state outlives a test: another suite's global of the same name must not leak in
    lua("calls = {}; fixed = {}");
    start();
    frames(2);
    ASSERT_EQ(culling(a).state, SimulationState::Reduced);
    lua("for _, c in pairs(calls) do for i = #c, 1, -1 do c[i] = nil end end; for k in pairs(fixed) do fixed[k] = 0 end");
    frames(40);
    const double frameDelta = Engine::getDeltatime();
    for (Entity e : {a, b}) {
        const std::string c = "calls[" + std::to_string(e) + "]";
        const int n = number("#" + c);
        EXPECT_EQ(n, 10) << "one frame in four";
        for (int i = 1; i <= n; i++) {
            const int frame = number(c + "[" + std::to_string(i) + "][1]");
            EXPECT_EQ((frame + static_cast<int>(e)) % 4, 0) << "phase: (frameCount + entity) % interval";
            if (i > 1) {
                const double delta = std::stod(lua("return string.format('%.9f', " + c + "[" + std::to_string(i) + "][2])"));
                EXPECT_NEAR(delta, 4.0 * frameDelta, 1e-6);
            }
        }
        EXPECT_EQ(number("fixed[" + std::to_string(e) + "]"), 40) << "onFixedUpdate every step";
    }
    EXPECT_NE(number("calls[" + std::to_string(a) + "][1][1]") % 4, number("calls[" + std::to_string(b) + "][1][1]") % 4);
}

// AC12: one message per transition into and out of Culled, none between Reduced and Full,
// disabled scripts included, in entity order
TEST_F(SimulationCullingScriptTest, MessagesOncePerTransition) {
    writeScript("Listener",
        "local M = {}\n"
        "function M:onSimulationCulled(reason) log[#log + 1] = 'culled:' .. self.scene:getEntityName(self.entity) .. ':' .. reason end\n"
        "function M:onSimulationResumed() log[#log + 1] = 'resumed:' .. self.scene:getEntityName(self.entity) end\n"
        "return M\n");
    const Entity a = culledObject("A", Vector3(0, 0, 10), 100.0f, 40.0f);
    const Entity b = culledObject("B", Vector3(0, 0, 11), 100.0f, 40.0f);
    addScript(scene, a, "Listener");
    addScript(scene, b, "Listener", false);
    start();
    frames(2);
    EXPECT_EQ(takeLog(), "");
    moveTo(camera, Vector3(0, 0, -500));
    frames(2);
    EXPECT_EQ(takeLog(), "culled:A:distance culled:B:distance");
    EXPECT_EQ(culling(a).state, SimulationState::Culled);
    // into the reduced band: resumed
    moveTo(camera, Vector3(0, 0, -60));
    frames(2);
    EXPECT_EQ(culling(a).state, SimulationState::Reduced);
    EXPECT_EQ(takeLog(), "resumed:A resumed:B");
    // Reduced to Full and back: nothing
    moveTo(camera, Vector3(0, 0, 0));
    frames(2);
    EXPECT_EQ(culling(a).state, SimulationState::Full);
    moveTo(camera, Vector3(0, 0, -60));
    frames(2);
    EXPECT_EQ(takeLog(), "");
    (void)b;
}

// AC5: a group made in init goes with its script's GameObject; a callback that disposes its group
// stops the evaluation's other events; distances changed in a callback apply from the next frame;
// the 257th group fails naming the limit
TEST_F(SimulationCullingScriptTest, LuaCullingGroupOwnershipAndReentry) {
    writeScript("Owner",
        "local M = {}\n"
        "function M:init() self.group = CullingGroup(self.scene); held = self.group end\n"
        "return M\n");
    const Entity owner = entity(scene, "Owner");
    addScript(scene, owner, "Owner");
    start();
    frame();
    EXPECT_EQ(lua("return tostring(held:isValid())"), "true");
    lua("GameObject.destroy(GameObject(testScene, " + std::to_string(owner) + "))");
    frames(2);
    EXPECT_EQ(lua("return tostring(held:isValid())"), "false") << "released with its script";

    // disposed from its own callback: the other events of that evaluation are dropped
    lua("seen = 0\n"
        "g = CullingGroup(testScene)\n"
        "g:setBoundingSpheres({BoundingSphere(Vector3(0, 0, 1), 1), BoundingSphere(Vector3(0, 0, 2), 1), BoundingSphere(Vector3(0, 0, 3), 1)})\n"
        "g.onStateChanged = function(i) seen = seen + 1; g:dispose() end\n");
    frame();
    EXPECT_EQ(number("seen"), 1);
    EXPECT_EQ(lua("return tostring(g:isValid())"), "false");

    // distances changed inside the callback: the next frame's bands
    lua("bands = {}\n"
        "h = CullingGroup(testScene)\n"
        "h:setDistanceReferencePoint(Vector3(0, 0, 0))\n"
        "h:setBoundingSphereCount(1)\n"
        "h:setSphere(1, BoundingSphere(Vector3(0, 0, 15), 0))\n"
        "h.onStateChanged = function(i, v, w, band) bands[#bands + 1] = band; h:setBoundingDistances({10}) end\n");
    frame();
    EXPECT_EQ(lua("return table.concat(bands, ',')"), "0");
    frame();
    EXPECT_EQ(lua("return table.concat(bands, ',')"), "0,1");
    lua("h:dispose()");

    // the 257th group of a scene
    lua("many = {}; for i = 1, 256 do many[i] = CullingGroup(testScene) end");
    EXPECT_EQ(lua("local n = 0; for _, m in ipairs(many) do if m:isValid() then n = n + 1 end end; return tostring(n)"), "256");
    lua("extra = CullingGroup(testScene)");
    EXPECT_EQ(lua("return tostring(extra:isValid())"), "false");
    EXPECT_TRUE(logged("at most 256"));
    lua("for _, m in ipairs(many) do m:dispose() end");
}

// AC6's Lua part: events and queries into a kept table make no Lua garbage
TEST_F(SimulationCullingScriptTest, LuaCullingGroupCallbackMakesNoGarbage) {
    const Entity mover = entity(scene, "Mover");
    start();
    lua("events = 0; out = {}\n"
        "q = CullingGroup(testScene)\n"
        "q:setBoundingDistances({5, 10})\n"
        "q:setDistanceReferencePoint(" + std::to_string(mover) + ")\n"
        "local spheres = {}\n"
        "for i = 1, 200 do spheres[i] = BoundingSphere(Vector3(0, 0, i * 0.1), 0) end\n"
        "q:setBoundingSpheres(spheres)\n"
        "for i = 1, 200 do out[i] = 0 end\n"
        "q.onStateChanged = function(i, v, w, b, pb) events = events + 1 end\n");
    frames(2);
    const int eventsBefore = number("events");
    lua("collectgarbage('collect'); collectgarbage('stop')");
    lua_State* state = L();
    const int before = lua_gc(state, LUA_GCCOUNT, 0) * 1024 + lua_gc(state, LUA_GCCOUNTB, 0);
    for (int i = 0; i < 100; i++) {
        moveTo(mover, Vector3(0, 0, (i % 2) ? 12.0f : 0.0f));
        frame();
    }
    // the C API, as a script's call would reach it, without a Lua chunk of its own allocating
    lua_getglobal(state, "q");
    lua_getfield(state, -1, "queryIndices");
    lua_pushvalue(state, -2);
    lua_pushinteger(state, 0);
    lua_getglobal(state, "out");
    ASSERT_EQ(lua_pcall(state, 3, 1, 0), LUA_OK) << lua_tostring(state, -1);
    lua_pop(state, 2);
    const int after = lua_gc(state, LUA_GCCOUNT, 0) * 1024 + lua_gc(state, LUA_GCCOUNTB, 0);
    lua("collectgarbage('restart')");
    EXPECT_GT(number("events"), eventsBefore + 1000);
    EXPECT_EQ(after, before) << "Lua bytes over 100 frames of events";
    lua("q:dispose()");
}

}  // namespace SimulationCullingScriptTest_cpp
