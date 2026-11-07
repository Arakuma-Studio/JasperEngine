// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// Coroutines, timers and Time (spec docs/superpowers/specs/2026-10-31-coroutines-design.md, section
// 10): Lua scripts on GameObjects of a scene, deterministic frames (Engine::setFixedDeltatime(1/60),
// Engine::systemUpdate), the scripts write what happens to the global TestLog with the frame
// number since the scene started (F()).

#include "ScriptDataTestSupport.h"

#include "Engine.h"
#include "EngineTime.h"
#include "Scene.h"
#include "component/ActionComponent.h"
#include "component/ScriptComponent.h"
#include "component/Transform.h"
#include "htmlui/anim/AnimEngine.h"
#include "manager/ObjectManager.h"
#include "object/ui/HtmlUIScriptInternal.h"
#include "script/LuaBinding.h"
#include "script/LuaFunction.h"
#include "script/ScriptBase.h"
#include "script/ScriptRuntime.h"
#include "script/ScriptScheduler.h"
#include "util/Scheduler.h"
#include "script/Timer.h"

#include "lua.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "log/Logger.h"

using namespace jasperengine;

namespace {

    size_t countOf(const std::string& text, const std::string& what) {
        size_t n = 0;
        for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) n++;
        return n;
    }

    // the test System, keeping what the engine logs (Log::* reach System::platformLog)
    class CapturingSystem : public scriptdata::TestSystem {
    public:
        std::string text;
        void platformLog(const int type, const char* fmt, va_list args) override {
            (void)type;
            char buffer[16384];
            std::vsnprintf(buffer, sizeof(buffer), fmt, args);
            text += buffer;
            text += "\n";
        }
    };

    class CoroutineTest : public scriptdata::ScriptDataTest {
    protected:
        std::unique_ptr<Scene> scene;
        CapturingSystem capture;
        System* swapped = nullptr;

        void SetUp() override {
            ScriptDataTest::SetUp();
            ASSERT_NE(L, nullptr);
            capture.root = system.root;
            swapped = jasperengine::SystemTestAccess::swap(&capture);
            Engine::pauseGameEvents(false);
            Engine::setFixedDeltatime(1.0 / 60.0);
            Engine::setTimeScale(1.0f);
            Engine::setUpdateTime(1.0f / 60.0f);
            ScriptScheduler::clear();
            // the global Timer's timers (the C++ Scheduler)
            Scheduler::game().clear();
            LuaBinding::clearLoadedProjectModules();
            scene = std::make_unique<Scene>();
            ASSERT_EQ(run("TestLog = {}; BaseFrame = Engine.frameCount\n"
                          "function F() return Time.frameCount - BaseFrame end\n"
                          "function Log(s) TestLog[#TestLog + 1] = tostring(s) end"), "");
        }

        void TearDown() override {
            ScriptScheduler::clear();
            Scheduler::game().clear();
            AnimEngine::instance().reset();
            if (scene) LuaBinding::cleanupLuaScripts(scene.get());
            scene.reset();
            Engine::onFixedUpdate.remove("test");
            Engine::pauseGameEvents(true);
            Engine::setFixedDeltatime(0.0);
            Engine::setTimeScale(1.0f);
            Engine::setUpdateTime(1.0f / 60.0f);
            run("TestLog = nil; Kept = nil; collectgarbage('collect')");
            ::jasperengine::Logger::flush();   // nothing is delivered to the capture after it goes
            jasperengine::SystemTestAccess::swap(swapped);
            ScriptDataTest::TearDown();
        }

        // a GameObject with the Lua script `className` (its file written now)
        Entity addScript(const std::string& className, const std::string& code, Entity parent = NULL_ENTITY) {
            scriptdata::writeFile(root / "lua" / (className + ".lua"), code);
            const Entity entity = scene->createEntity();
            scene->addComponent<Transform>(entity, Transform());
            if (parent != NULL_ENTITY) scene->addEntityChild(parent, entity, false);
            scene->setEntityName(entity, className);
            ScriptComponent scripts;
            ScriptEntry entry;
            entry.type = ScriptType::LUA;
            entry.path = className + ".lua";
            entry.className = className;
            entry.enabled = true;
            scripts.scripts.push_back(entry);
            scene->addComponent<ScriptComponent>(entity, scripts);
            return entity;
        }

        Entity addObject(Entity parent = NULL_ENTITY) {
            const Entity entity = scene->createEntity();
            scene->addComponent<Transform>(entity, Transform());
            if (parent != NULL_ENTITY) scene->addEntityChild(parent, entity, false);
            return entity;
        }

        // the scripts' instances and init(), then frames count from here
        void start() {
            LuaBinding::initializeLuaScripts(scene.get());
            ASSERT_EQ(run("BaseFrame = Engine.frameCount"), "");
        }

        void frames(int n) {
            for (int i = 0; i < n; i++) Engine::systemUpdate();
        }

        std::string log() {
            return eval("return table.concat(TestLog, ' ')");
        }

        void clearLog() {
            ASSERT_EQ(run("TestLog = {}"), "");
        }

        std::string logged() {
            ::jasperengine::Logger::flush();   // the engine's logger delivers on its own thread
            return capture.text;
        }

        // the global Timer's pending timers (Timer.after / every... on the C++ Scheduler)
        size_t luaTimersPending() {
            size_t count = 0;
            for (const TimerInfo& info : Scheduler::game().pending(nullptr, 100000))
                if (info.debugName.rfind("Timer.", 0) == 0) count++;
            return count;
        }

        void* instanceOf(Entity entity) {
            ScriptComponent* scripts = scene->findComponent<ScriptComponent>(entity);
            return scripts && !scripts->scripts.empty() ? scripts->scripts[0].instance : nullptr;
        }
    };

}

// ================================================================== 1. frame order

TEST_F(CoroutineTest, StartRunsToTheFirstWaitThenResumesAfterUpdateBeforePostUpdate) {
    addScript("Order", R"(
        local M = {}
        function M:init()
            RegisterEngineEvent(self, "onUpdate")
            RegisterEngineEvent(self, "onPostUpdate")
            self.co = self:startCoroutine(function()
                Log("first")
                Coroutine.nextFrame()
                Log("co" .. F())
                coroutine.yield()           -- a plain yield is the next frame too
                Log("co" .. F())
                return "done"
            end)
            Log("after:" .. self.co.status)
        end
        function M:onUpdate() Log("update" .. F()) end
        function M:onPostUpdate() Log("post" .. F()) end
        return M
    )");
    start();
    EXPECT_EQ(log(), "first after:suspended");
    frames(3);
    EXPECT_EQ(log(), "first after:suspended update1 co1 post1 update2 co2 post2 update3 post3");
    EXPECT_EQ(ScriptScheduler::coroutineCount(), 0u);
}

TEST_F(CoroutineTest, FirstSegmentWithoutWaitFinishesAtOnce) {
    addScript("Quick", R"(
        local M = {}
        function M:init()
            local co = self:startCoroutine(function(a, b) Log("ran" .. a .. b) return a + b end, 1, 2)
            Log(co.status)
            Log(tostring(co:isDone()))
        end
        return M
    )");
    start();
    EXPECT_EQ(log(), "ran12 finished true");
}

TEST_F(CoroutineTest, StartByMethodNamePassesSelfAndArguments) {
    addScript("ByName", R"(
        local M = {}
        function M:init()
            self.tag = "me"
            self:startCoroutine("run", 5)
        end
        function M:run(n)
            Coroutine.nextFrame()
            Log(self.tag .. n)
        end
        return M
    )");
    start();
    frames(1);
    EXPECT_EQ(log(), "me5");
}

// ================================================================== 2. seconds, scaled and realtime

TEST_F(CoroutineTest, WaitSecondsIsScaledAndWaitRealtimeIsNot) {
    addScript("Clock", R"(
        local M = {}
        function M:init()
            self:startCoroutine(function()
                local elapsed = Coroutine.wait(0.5)
                Log(string.format("scaled%d:%.3f", F(), elapsed))
            end)
        end
        return M
    )");
    start();
    frames(29);
    EXPECT_EQ(log(), "");
    frames(1);
    EXPECT_EQ(log(), "scaled30:0.500");

    // at time scale 0 a scaled wait never ends, a realtime one does
    clearLog();
    Engine::setTimeScale(0.0f);
    ASSERT_EQ(run(R"(
        Coroutine.start(function() Coroutine.wait(0.1); Log("never") end)
        local base = F()
        Coroutine.start(function()
            local elapsed = Coroutine.waitRealtime(0.5)
            Log(string.format("realtime%d:%.3f", F() - base, elapsed))
        end)
    )"), "");
    frames(29);
    EXPECT_EQ(log(), "");
    frames(1);
    EXPECT_EQ(log(), "realtime30:0.500");
    frames(60);
    EXPECT_EQ(log(), "realtime30:0.500");
    EXPECT_EQ(ScriptScheduler::coroutineCount(), 1u);
}

TEST_F(CoroutineTest, WaitZeroIsTheNextFrame) {
    ASSERT_EQ(run(R"(
        local base = F()
        Coroutine.start(function() Coroutine.wait(0); Log("w" .. (F() - base)); Coroutine.wait(-1); Log("w" .. (F() - base)) end)
    )"), "");
    frames(3);
    EXPECT_EQ(log(), "w1 w2");
}

TEST_F(CoroutineTest, WaitFramesCountsFrames) {
    ASSERT_EQ(run(R"(
        local base = F()
        Coroutine.start(function() Coroutine.waitFrames(3); Log("f" .. (F() - base)) end)
    )"), "");
    frames(5);
    EXPECT_EQ(log(), "f3");
    EXPECT_NE(eval("return select(2, pcall(Coroutine.start, function() Coroutine.waitFrames(0) end))"), "");
}

// ================================================================== 3. fixed update

TEST_F(CoroutineTest, WaitForFixedUpdateResumesAfterTheStepEveryOtherFrameAt30Hz) {
    Engine::setUpdateTime(1.0f / 30.0f);
    addScript("Fixed", R"(
        local M = {}
        function M:init()
            RegisterEngineEvent(self, "onFixedUpdate")
            self:startCoroutine(function()
                for i = 1, 3 do
                    Coroutine.waitForFixedUpdate()
                    Log("co" .. F() .. (Time.inFixedTimeStep and "in" or "out") .. string.format("%.4f", Time.deltaTime))
                end
            end)
        end
        function M:onFixedUpdate() Log("fixed" .. F()) end
        return M
    )");
    start();
    frames(6);
    // 60 fps frames, 30 Hz steps: a step every other frame; the coroutine runs after onFixedUpdate
    // (and the physics) of the step, inside it, where Time.deltaTime is the fixed step
    EXPECT_EQ(log(), "fixed2 co2in0.0333 fixed4 co4in0.0333 fixed6 co6in0.0333");
}

TEST_F(CoroutineTest, AFixedWaitRecordedInAStepResumesInTheNextStep) {
    Engine::onFixedUpdate.add("test", std::function<void()>([this](){ ASSERT_EQ(run("Log('step' .. F())"), ""); }));
    addScript("FixedNext", R"(
        local M = {}
        function M:init()
            RegisterEngineEvent(self, "onFixedUpdate")
        end
        function M:onFixedUpdate()
            if not self.started then
                self.started = true
                self:startCoroutine(function() Coroutine.waitForFixedUpdate(); Log("co" .. F()) end)
            end
        end
        return M
    )");
    start();
    frames(2);
    EXPECT_EQ(log(), "step1 step2 co2");
}

TEST_F(CoroutineTest, NoFixedStepAtTimeScaleZero) {
    Engine::setTimeScale(0.0f);
    ASSERT_EQ(run("Coroutine.start(function() Coroutine.waitForFixedUpdate(); Log('fixed') end)"), "");
    frames(10);
    EXPECT_EQ(log(), "");
    Engine::setTimeScale(1.0f);
    frames(1);
    EXPECT_EQ(log(), "fixed");
}

// ================================================================== 4. end of frame

TEST_F(CoroutineTest, WaitForEndOfFrameResumesInTheSameFrameAfterTheUpdate) {
    addScript("Eof", R"(
        local M = {}
        function M:init()
            RegisterEngineEvent(self, "onUpdate")
            RegisterEngineEvent(self, "onPostUpdate")
        end
        function M:onUpdate()
            if F() == 1 then
                self:startCoroutine(function()
                    Coroutine.waitForEndOfFrame()
                    Log("eof" .. F())
                    Coroutine.waitForEndOfFrame()
                    Log("eof" .. F())
                end)
            end
        end
        function M:onPostUpdate() Log("post" .. F()) end
        return M
    )");
    start();
    frames(2);
    EXPECT_EQ(log(), "post1 eof1 post2 eof2");
}

// ================================================================== 5. predicates

TEST_F(CoroutineTest, WaitUntilAndWaitWhileWithAndWithoutTimeout) {
    ASSERT_EQ(run(R"(
        Flag = false
        local base = F()
        Coroutine.start(function() local ok = Coroutine.waitUntil(function() return Flag end); Log("until" .. (F() - base) .. tostring(ok)) end)
        Coroutine.start(function() local ok = Coroutine.waitWhile(function() return not Flag end); Log("while" .. (F() - base) .. tostring(ok)) end)
        Coroutine.start(function() local ok = Coroutine.waitUntil(function() return false end, 0.1); Log("timeout" .. (F() - base) .. tostring(ok)) end)
        Coroutine.start(function() local ok = Coroutine.waitUntil(function() return true end, 5); Log("now" .. (F() - base) .. tostring(ok)) end)
    )"), "");
    frames(1);
    EXPECT_EQ(log(), "now1true");
    frames(4);
    ASSERT_EQ(run("Flag = true"), "");
    frames(1);
    // the 0.1 s timeout ran out in the same frame (6 frames of 1/60 s): false
    EXPECT_EQ(log(), "now1true until6true while6true timeout6false");
}

TEST_F(CoroutineTest, WaitUntilTimeoutReturnsFalse) {
    ASSERT_EQ(run(R"(
        local base = F()
        Coroutine.start(function() local ok = Coroutine.waitUntil(function() return false end, 0.1); Log("t" .. (F() - base) .. tostring(ok)) end)
    )"), "");
    frames(8);
    EXPECT_EQ(log(), "t6false");
}

TEST_F(CoroutineTest, APredicateErrorEndsTheCoroutineWithATraceback) {
    ASSERT_EQ(run(R"(
        Kept = Coroutine.start(function() Coroutine.waitUntil(function() error("bad predicate") end); Log("never") end)
    )"), "");
    frames(2);
    EXPECT_EQ(log(), "");
    EXPECT_EQ(eval("return Kept.status"), "error");
    const std::string text = logged();
    EXPECT_NE(text.find("Lua coroutine error (started at"), std::string::npos) << text;
    EXPECT_NE(text.find("waitUntil predicate failed"), std::string::npos) << text;
    EXPECT_NE(text.find("bad predicate"), std::string::npos) << text;
    EXPECT_NE(text.find("stack traceback"), std::string::npos) << text;
    EXPECT_NE(eval("return Kept.error"), "nil");
}

TEST_F(CoroutineTest, APredicateCannotWait) {
    ASSERT_EQ(run(R"(
        Kept = Coroutine.start(function() Coroutine.waitUntil(function() Coroutine.wait(1) return true end) end)
    )"), "");
    frames(1);
    EXPECT_EQ(eval("return Kept.status"), "error");
    EXPECT_NE(logged().find("not inside an engine coroutine"), std::string::npos) << logged();
}

// ================================================================== 6. waitFor

TEST_F(CoroutineTest, WaitForAChildCoroutineGetsItsResultsErrorOrStop) {
    ASSERT_EQ(run(R"(
        local base = F()
        Coroutine.start(function()
            local child = Coroutine.start(function() Coroutine.waitFrames(2); return 7, "x" end)
            local ok, a, b = Coroutine.waitFor(child)
            Log("child" .. (F() - base) .. tostring(ok) .. a .. b)
            -- it already ended: at once, no frame
            local again, c = Coroutine.waitFor(child)
            Log("again" .. (F() - base) .. tostring(again) .. c)
            local failing = Coroutine.start(function() Coroutine.nextFrame(); error("child failed") end)
            local ok2, err = Coroutine.waitFor(failing)
            Log("failed" .. tostring(ok2) .. tostring(err:find("child failed") ~= nil))
            local endless = Coroutine.start(function() Coroutine.wait(100) end)
            Timer.after(0.05, function() endless:stop() end)
            local ok3, why = Coroutine.waitFor(endless)
            Log("stopped" .. tostring(ok3) .. why)
            -- a plain yield of a handle waits for it the same
            local other = Coroutine.start(function() Coroutine.nextFrame(); return "y" end)
            local ok4, v = coroutine.yield(other)
            Log("yielded" .. tostring(ok4) .. v)
        end)
    )"), "");
    frames(12);
    const std::string text = log();
    // the child ends in frame 2 after its parent was polled (creation order): the parent sees it in frame 3
    EXPECT_NE(text.find("child3true7x"), std::string::npos) << text;
    EXPECT_NE(text.find("again3true7"), std::string::npos) << text;
    EXPECT_NE(text.find("failedfalsetrue"), std::string::npos) << text;
    EXPECT_NE(text.find("stoppedfalsestopped"), std::string::npos) << text;
    EXPECT_NE(text.find("yieldedtruey"), std::string::npos) << text;
}

TEST_F(CoroutineTest, WaitForTimersTweensAudioAndIsDone) {
    addScript("Waiter", R"(
        local M = {}
        function M:init()
            self:startCoroutine(function()
                local t = Timer.after(0.05, function() end)
                Log("timer" .. tostring(Coroutine.waitFor(t)) .. F())
                local c = Timer.after(1, function() end)
                Timer.after(0.05, function() c:cancel() end)
                Log("cancelled" .. tostring(Coroutine.waitFor(c)))
                local counted = Timer.every(0.02, function() end, {count = 2})
                Log("counted" .. tostring(Coroutine.waitFor(counted)))
                local ok, err = pcall(Coroutine.waitFor, Timer.every(1, function() end))
                Log("endless" .. tostring(ok) .. tostring(err:find("never done") ~= nil))

                local tween = PositionTween(self.scene)
                tween:start()
                Timer.after(0.05, function() tween:stop() end)
                Log("tween" .. tostring(Coroutine.waitFor(tween)))

                local audio = AudioSource(self.scene)
                Log("silent" .. tostring(Coroutine.waitFor(audio)))
                audio:play()
                Timer.after(0.05, function() audio:stop() end)
                Log("audio" .. tostring(Coroutine.waitFor(audio)))

                local job = { done = false }
                function job:isDone() return self.done end
                Timer.after(0.05, function() job.done = true end)
                local ok2, same = Coroutine.waitFor(job)
                Log("job" .. tostring(ok2) .. tostring(same == job))

                local bad = pcall(Coroutine.waitFor, 42)
                Log("number" .. tostring(bad))
                local me = pcall(Coroutine.waitFor, Coroutine.running())
                Log("self" .. tostring(me))
            end)
        end
        return M
    )");
    start();
    frames(60);
    EXPECT_EQ(log(), "timertrue3 cancelledfalse countedtrue endlessfalsetrue tweentrue silenttrue audiotrue jobtruetrue numberfalse selffalse");
}

TEST_F(CoroutineTest, WaitForATweenWhoseGameObjectGoesIsFalse) {
    addScript("Gone", R"(
        local M = {}
        function M:init()
            self:startCoroutine(function()
                local tween = PositionTween(self.scene)
                tween:start()
                Kept = tween
                Log("gone" .. tostring(Coroutine.waitFor(tween)))
            end)
        end
        return M
    )");
    start();
    frames(2);
    const Entity tween = static_cast<Entity>(std::stoul(eval("return Kept.entity")));
    ASSERT_TRUE(scene->isEntityCreated(tween));
    scene->destroyEntity(tween);
    frames(1);
    EXPECT_EQ(log(), "gonefalse");
}

TEST_F(CoroutineTest, WaitForAnAnimeTimer) {
    ASSERT_EQ(run(R"(
        Coroutine.start(function()
            local timer = anime.createTimer({duration = 50})
            Log("anime" .. tostring(Coroutine.waitFor(timer)))
        end)
    )"), "");
    frames(1);
    const double ui = AnimEngine::instance().now(AnimClock::UI) / 1000.0;
    AnimEngine::instance().tick(ui + 0.1);
    frames(1);
    EXPECT_EQ(log(), "animetrue");
}

// ================================================================== 7. ownership

TEST_F(CoroutineTest, DeactivatingStopsCoroutinesNotTimersAndDisablingStopsNeither) {
    const Entity entity = addScript("Owned", R"(
        local M = {}
        function M:init()
            Kept = self:startCoroutine(function()
                local guard <close> = setmetatable({}, {__close = function() Log("closed") end})
                while true do Coroutine.nextFrame() Log("tick") end
            end)
            self:invokeRepeating(function() Log("timer") end, 0, 0.0001)
        end
        return M
    )");
    start();
    frames(1);
    EXPECT_EQ(log(), "timer tick");
    clearLog();

    // disabling the script stops neither (Unity)
    scene->getComponent<ScriptComponent>(entity).scripts[0].enabled = false;
    frames(1);
    EXPECT_EQ(log(), "timer tick");
    scene->getComponent<ScriptComponent>(entity).scripts[0].enabled = true;
    clearLog();

    scene->setEntityActive(entity, false);
    EXPECT_EQ(log(), "closed");
    EXPECT_EQ(eval("return Kept.status"), "stopped");
    frames(2);
    EXPECT_EQ(log(), "closed timer timer");
    EXPECT_EQ(ScriptScheduler::coroutineCountOf(instanceOf(entity)), 0u);
    EXPECT_EQ(ScriptScheduler::timerCountOf(instanceOf(entity)), 1u);

    // startCoroutine on an inactive GameObject raises
    lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<lua_Integer>(reinterpret_cast<intptr_t>(instanceOf(entity))));
    lua_setglobal(L, "Owner");
    EXPECT_NE(eval("return select(2, pcall(Owner.startCoroutine, Owner, function() end))").find("GameObject 'Owned' is inactive"),
              std::string::npos);
}

TEST_F(CoroutineTest, AParentDeactivationAndAMoveUnderAnInactiveParentStopTheCoroutines) {
    const Entity parent = addObject();
    addScript("Child", R"(
        local M = {}
        function M:init() Kept = self:startCoroutine(function() while true do Coroutine.nextFrame() Log("c") end end) end
        return M
    )", parent);
    const Entity inactive = addObject();
    const Entity moved = addScript("Moved", R"(
        local M = {}
        function M:init() Kept2 = self:startCoroutine(function() while true do Coroutine.nextFrame() Log("m") end end) end
        return M
    )");
    start();
    frames(1);
    EXPECT_EQ(log(), "c m");
    scene->setEntityActive(parent, false);
    EXPECT_EQ(eval("return Kept.status"), "stopped");
    scene->setEntityActive(inactive, false);
    EXPECT_EQ(eval("return Kept2.status"), "suspended");
    scene->addEntityChild(inactive, moved, false);
    EXPECT_EQ(eval("return Kept2.status"), "stopped");
    clearLog();
    frames(2);
    EXPECT_EQ(log(), "");
    run("Kept2 = nil");
}

TEST_F(CoroutineTest, DestroyCancelsCoroutinesAndTimers) {
    addScript("Doomed", R"(
        local M = {}
        function M:init()
            RegisterEngineEvent(self, "onUpdate")
            Kept = self:startCoroutine(function() while true do Coroutine.nextFrame() Log("co") end end)
            KeptTimer = self:invokeRepeating("tick", 0, 0.0001)
        end
        function M:tick() Log("timer") end
        function M:onUpdate() if F() == 2 then GameObject.destroy(self) end end
        return M
    )");
    start();
    frames(2);
    EXPECT_EQ(log(), "timer co timer co");
    frames(2);
    EXPECT_EQ(log(), "timer co timer co");
    EXPECT_EQ(eval("return Kept.status"), "stopped");
    EXPECT_EQ(eval("return tostring(KeptTimer:isDone())"), "true");
    EXPECT_EQ(ScriptScheduler::coroutineCount(), 0u);
    EXPECT_EQ(ScriptScheduler::timerCount(), 0u);
    run("KeptTimer = nil");
}

TEST_F(CoroutineTest, ASceneUnloadEndsOwnedWorkAndKeepsUnownedWork) {
    addScript("Level", R"(
        local M = {}
        function M:init()
            self:startCoroutine(function() while true do Coroutine.nextFrame() Log("owned") end end)
            self:invoke(function() Log("ownedTimer") end, 0.5)
            Coroutine.start(function() while true do Coroutine.nextFrame() Log("free") end end)
            Timer.after(0.05, function() Log("freeTimer") end)
            Timer.after(0.05, function() Log("boundTimer") end, {owner = self})
        end
        return M
    )");
    start();
    frames(1);
    EXPECT_EQ(log(), "owned free");
    // the generated cleanupScripts / the editor's Play stop: the scene's scripts are released
    LuaBinding::cleanupLuaScripts(scene.get());
    clearLog();
    frames(4);
    // (the 0.05 s timer: frame 3, before the coroutines of that frame)
    EXPECT_EQ(log(), "free freeTimer free free free");
    EXPECT_EQ(ScriptScheduler::coroutineCount(), 1u);
}

// ================================================================== 8. stopping

TEST_F(CoroutineTest, StopByHandleByNameAllAndFromInside) {
    addScript("Stopper", R"(
        local M = {}
        function M:loop(tag) while true do Coroutine.nextFrame() Log(tag) end end
        function M:init()
            RegisterEngineEvent(self, "onUpdate")
            self.a = self:startCoroutine(function() self:loop("a") end)
            self.b = self:startCoroutine("loop", "b")
            self.b2 = self:startCoroutine("loop", "b2")
            self.c = self:startCoroutine(function()
                local guard <close> = setmetatable({}, {__close = function() Log("cclosed") end})
                Coroutine.nextFrame()
                self:stopAllCoroutines()
                Log("c-continues")
                Coroutine.nextFrame()
                Log("c-never")
            end)
        end
        function M:onUpdate()
            if F() == 2 then self:stopCoroutine(self.a) end
            if F() == 3 then self:stopCoroutine("loop") end
        end
        return M
    )");
    start();
    frames(1);
    // c stops everything of this script, itself included at its next wait
    EXPECT_EQ(log(), "a b b2 c-continues cclosed");
    clearLog();
    frames(2);
    EXPECT_EQ(log(), "");
    EXPECT_EQ(ScriptScheduler::coroutineCount(), 0u);
}

TEST_F(CoroutineTest, StopRulesForOtherScriptsAndNormalCoroutines) {
    addScript("Other", R"(
        local M = {}
        function M:init() OtherCo = self:startCoroutine(function() while true do Coroutine.nextFrame() end end) OtherSelf = self end
        return M
    )");
    addScript("Mine", R"(
        local M = {}
        function M:init()
            local ok, err = pcall(self.stopCoroutine, self, OtherCo)
            Log("other" .. tostring(ok) .. tostring(err:find("does not belong") ~= nil))
            self:stopAllCoroutines()   -- not the other script's
            Log(OtherCo.status)
            -- a parent in status "normal" stopped by its child stops at its next wait
            self.parent = self:startCoroutine(function()
                local me = Coroutine.running()
                self:startCoroutine(function() me:stop() Log("child-stopped-parent") end)
                Log("parent-continues")
                Coroutine.nextFrame()
                Log("parent-never")
            end)
            Log(self.parent.status)
        end
        return M
    )");
    start();
    frames(2);
    EXPECT_EQ(log(), "otherfalsetrue suspended child-stopped-parent parent-continues stopped");
    run("OtherCo = nil; OtherSelf = nil");
}

// ================================================================== 9. timers

TEST_F(CoroutineTest, InvokeAndInvokeRepeating) {
    addScript("Timers", R"(
        local M = {}
        function M:init()
            self:invoke("once", 1.0)
            self:invokeRepeating("rep", 0.5, 0.25)
            self:invokeRepeating(function() Fast = (Fast or 0) + 1 end, 0, 0.001)
            Log("invoking" .. tostring(self:isInvoking()) .. tostring(self:isInvoking("once")) .. tostring(self:isInvoking("nothing")))
        end
        function M:once() Log("once" .. F()) end
        function M:rep()
            Log("rep" .. F())
            if F() >= 60 then self:cancelInvoke("rep") Log("cancelled" .. tostring(self:isInvoking("rep"))) end
        end
        return M
    )");
    start();
    frames(90);
    EXPECT_EQ(log(), "invokingtruetruefalse rep30 rep45 once60 rep60 cancelledfalse");
    // an interval smaller than the frame: once a frame, no catch-up burst
    EXPECT_EQ(eval("return Fast"), "90");
}

TEST_F(CoroutineTest, TimerEveryCountFalseAndUnscaled) {
    Engine::setTimeScale(0.0f);
    ASSERT_EQ(run(R"(
        local base = F()
        Timer.after(0.05, function() Log("scaled") end)
        Timer.after(0.05, function() Log("unscaled" .. (F() - base)) end, {unscaled = true})
        local n = 0
        Kept = Timer.every(0.05, function(t) n = n + 1 Log("every" .. n) if n == 2 then return false end end, {unscaled = true})
        Timer.every(0.05, function() Log("counted") end, {unscaled = true, count = 3, delay = 0})
    )"), "");
    frames(30);
    // counted: frames 1, 3, 6 (delay 0, then every 0.05 s); every: 3, 6
    EXPECT_EQ(log(), "counted unscaled3 every1 counted every2 counted");
    EXPECT_EQ(eval("return tostring(Kept:isDone())"), "true");
    // the scaled one waits at time scale 0 (the global Timer's timers are the C++ Scheduler's)
    EXPECT_EQ(luaTimersPending(), 1u);
}

TEST_F(CoroutineTest, AnErrorInARepeatingTimerIsLoggedAndItContinues) {
    addScript("Faulty", R"(
        local M = {}
        function M:init()
            self:invokeRepeating(function() Log("call") error("timer boom") end, 0, 0.0001)
            self:invoke("missing", 0)
        end
        return M
    )");
    start();
    frames(3);
    EXPECT_EQ(log(), "call call call");
    const std::string text = logged();
    EXPECT_EQ(countOf(text, "timer boom"), 3u) << text;
    EXPECT_NE(text.find("Lua timer error in Faulty (GameObject 'Faulty'"), std::string::npos) << text;
    EXPECT_NE(text.find("created at Faulty.lua:4"), std::string::npos) << text;
    // a missing method: logged once, the timer ends
    EXPECT_EQ(countOf(text, "no method 'missing'"), 1u) << text;
}

TEST_F(CoroutineTest, TimerArgumentErrors) {
    EXPECT_NE(eval("return select(2, pcall(Timer.every, 0, function() end))").find("interval must be > 0"), std::string::npos);
    EXPECT_NE(eval("return select(2, pcall(Timer.after, 'x', function() end))").find("delay must be a number"), std::string::npos);
    EXPECT_NE(eval("return select(2, pcall(Timer.after, 1, function() end, {owner = {}}))").find("owner must be a live script instance"),
              std::string::npos);
    EXPECT_NE(eval("return select(2, pcall(Timer.every, 1, function() end, {count = 0}))").find("count must be an integer"),
              std::string::npos);
    const std::string text = eval("local t = Timer.every(2, function() end); local s = tostring(t); t:cancel(); return s .. ' ' .. tostring(t)");
    EXPECT_NE(text.find(", 2.00 s left, every 2.00 s) Timer("), std::string::npos) << text;
    EXPECT_NE(text.find(", cancelled, every 2.00 s)"), std::string::npos) << text;
}

// ================================================================== waits outside coroutines, C boundaries, hooks

TEST_F(CoroutineTest, WaitsOutsideAnEngineCoroutineRaiseAClearError) {
    const std::string mainError = eval("return select(2, pcall(Coroutine.wait, 1))");
    EXPECT_NE(mainError.find("Coroutine.wait: not inside an engine coroutine"), std::string::npos) << mainError;
    EXPECT_NE(mainError.find("event handlers like onUpdate cannot wait"), std::string::npos) << mainError;
    // a Lua coroutine inside an engine coroutine is not one either (generators still work)
    ASSERT_EQ(run(R"(
        Coroutine.start(function()
            local gen = coroutine.wrap(function() for i = 1, 3 do coroutine.yield(i) end end)
            Log("gen" .. gen() .. gen() .. gen())
            local inner = coroutine.wrap(function() return pcall(Coroutine.nextFrame) end)
            local ok, err = inner()
            Log("inner" .. tostring(ok) .. tostring(err:find("not inside an engine coroutine") ~= nil))
            Log("inside" .. tostring(Coroutine.isInside()))
        end)
        Log("outside" .. tostring(Coroutine.isInside()) .. tostring(Coroutine.running()))
    )"), "");
    EXPECT_EQ(log(), "gen123 innerfalsetrue insidetrue outsidefalsenil");
}

TEST_F(CoroutineTest, AWaitAcrossACFunctionGetsAHint) {
    ASSERT_EQ(run(R"(
        Kept = Coroutine.start(function()
            local t = {3, 1, 2}
            table.sort(t, function(a, b) Coroutine.nextFrame() return a < b end)
        end)
        Kept2 = Coroutine.start(function()
            local t = {3, 1, 2}
            table.sort(t, function(a, b) coroutine.yield() return a < b end)
        end)
    )"), "");
    EXPECT_EQ(eval("return Kept.status .. Kept2.status"), "errorerror");
    const std::string text = logged();
    EXPECT_NE(text.find("waits cannot cross a C function"), std::string::npos) << text;
    EXPECT_EQ(countOf(text, "waits cannot cross a C function"), 2u) << text;
    run("Kept2 = nil");
}

TEST_F(CoroutineTest, AYieldOfSomethingElseIsAnError) {
    ASSERT_EQ(run("Kept = Coroutine.start(function() coroutine.yield(5) end)"), "");
    EXPECT_EQ(eval("return Kept.status"), "error");
    EXPECT_NE(logged().find("coroutine.yield(a number) in an engine coroutine"), std::string::npos) << logged();
}

namespace {
    int hookCalls = 0;
    void countingHook(lua_State*, lua_Debug*) { hookCalls++; }
}

TEST_F(CoroutineTest, OnlyTheFirstSegmentRunsUnderTheCallersHook) {
    hookCalls = 0;
    lua_sethook(L, countingHook, LUA_MASKCOUNT, 1);
    ASSERT_EQ(run("Coroutine.start(function() local x = 0 for i = 1, 50 do x = x + i end Coroutine.nextFrame() for i = 1, 500 do x = x + i end Log(x) end)"), "");
    lua_sethook(L, nullptr, 0, 0);
    const int during = hookCalls;
    EXPECT_GT(during, 50);
    frames(1);
    EXPECT_EQ(log(), "126525");
    EXPECT_EQ(hookCalls, during);
}

// ================================================================== 10. main-thread normalisation

TEST_F(CoroutineTest, CallbacksRegisteredInsideACoroutineKeepTheMainThread) {
    addScript("Registrar", R"(
        local M = {}
        function M:viewLoaded() Log("registerEvent") end
        function M:init()
            self:startCoroutine(function()
                Coroutine.nextFrame()
                RegisterEvent(self, Engine.onViewLoaded, "viewLoaded")
                Engine.onViewDestroyed = function() Log("assigned") end
                anime.createTimer({duration = 10, onComplete = function() Log("anime") end})
            end)
        end
        return M
    )");
    start();
    frames(2);
    EXPECT_EQ(ScriptScheduler::coroutineCount(), 0u);
    ASSERT_EQ(run("collectgarbage('collect'); collectgarbage('collect')"), "");
    Engine::onViewLoaded.call();
    Engine::onViewDestroyed.call();
    const double ui = AnimEngine::instance().now(AnimClock::UI) / 1000.0;
    AnimEngine::instance().tick(ui + 1.0);
    EXPECT_EQ(log(), "registerEvent assigned anime");
    Engine::onViewDestroyed.remove("luaFunction");
}

TEST_F(CoroutineTest, AnHtmlUILuaRefMadeOnACoroutineThreadPushesOnAnyThread) {
    lua_State* co = lua_newthread(L);
    const int coRef = luaL_ref(L, LUA_REGISTRYINDEX);
    luaL_loadstring(co, "return 'listener'");
    HtmlUILuaRef ref(co, -1);
    lua_pop(co, 1);
    EXPECT_EQ(ref.L, L);
    luaL_unref(L, LUA_REGISTRYINDEX, coRef);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    ASSERT_TRUE(ref.push(L));
    ASSERT_EQ(lua_pcall(L, 0, 1, 0), LUA_OK);
    EXPECT_STREQ(lua_tostring(L, -1), "listener");
    lua_pop(L, 1);
    // and on a thread of the same state, from a coroutine
    lua_State* other = lua_newthread(L);
    ASSERT_TRUE(ref.push(other));
    EXPECT_EQ(lua_gettop(other), 1);
    lua_pop(L, 1);
}

TEST_F(CoroutineTest, ALuaFunctionMadeOnACoroutineThreadKeepsTheMainThread) {
    lua_State* co = lua_newthread(L);
    const int coRef = luaL_ref(L, LUA_REGISTRYINDEX);
    luaL_loadstring(co, "Log('luaFunction')");
    std::function<void()> fn = LuaFunction<void>(co);
    luaL_unref(L, LUA_REGISTRYINDEX, coRef);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    fn();
    EXPECT_EQ(log(), "luaFunction");
}

// ================================================================== 11. errors

TEST_F(CoroutineTest, ErrorsNameTheScriptGameObjectOriginAndCarryTheCoroutineTraceback) {
    addScript("Spawner", R"(
        local M = {}
        local function deeper() error("boom") end
        function M:spawn() Coroutine.nextFrame() deeper() end
        function M:init()
            Kept = self:startCoroutine("spawn")
            KeptFirst = self:startCoroutine(function() error("first segment") end)
            Log(KeptFirst.status)
        end
        return M
    )");
    start();
    EXPECT_EQ(log(), "error");
    frames(1);
    EXPECT_EQ(eval("return Kept.status"), "error");
    const std::string text = logged();
    // (oe-port loads a script as the chunk "@<path>": Lua's own messages name the plain path too)
    EXPECT_NE(text.find("Lua coroutine error in Spawner (GameObject 'Spawner'), started at Spawner.lua:6: Spawner.lua:3: boom"),
              std::string::npos) << text;
    EXPECT_NE(text.find("stack traceback:"), std::string::npos) << text;
    EXPECT_NE(text.find("in upvalue 'deeper'"), std::string::npos) << text;
    EXPECT_NE(text.find("Spawner.lua:4: in function"), std::string::npos) << text;
    EXPECT_NE(text.find("started at Spawner.lua:7: Spawner.lua:7: first segment"), std::string::npos) << text;
    EXPECT_NE(eval("return Kept.error"), "nil");
    EXPECT_NE(eval("return tostring(Kept)").find("Coroutine("), std::string::npos);
    EXPECT_NE(eval("return tostring(Kept)").find("error, Spawner.lua:6)"), std::string::npos);
}

// ================================================================== 12. Time

TEST_F(CoroutineTest, TimeValues) {
    addScript("Clocks", R"(
        local M = {}
        function M:init() RegisterEngineEvent(self, "onFixedUpdate") RegisterEngineEvent(self, "onUpdate") end
        function M:onFixedUpdate()
            FixedDelta = Time.deltaTime
            FixedIn = Time.inFixedTimeStep
            FixedTime = Time.time == Time.fixedTime
        end
        function M:onUpdate()
            Delta = Time.deltaTime
            Unscaled = Time.unscaledDeltaTime
            Out = Time.inFixedTimeStep
        end
        return M
    )");
    start();
    Engine::setTimeScale(0.5f);
    frames(4);
    EXPECT_EQ(eval("return string.format('%.4f %s %s', FixedDelta, tostring(FixedIn), tostring(FixedTime))"), "0.0167 true true");
    EXPECT_EQ(eval("return string.format('%.4f %.4f %s', Delta, Unscaled, tostring(Out))"), "0.0083 0.0167 false");
    EXPECT_EQ(eval("return string.format('%.4f', Time.fixedUnscaledDeltaTime)"), "0.0333");
    EXPECT_EQ(eval("return string.format('%.4f', Engine.unscaledDeltatime)"), "0.0167");
    frames(60);   // the average converges on the new delta
    EXPECT_EQ(eval("return string.format('%.4f', Time.smoothDeltaTime)"), "0.0083");

    // captureDeltaTime is Engine.fixedDeltatime; fixedDeltaTime is Engine.updateTime
    EXPECT_EQ(eval("Time.captureDeltaTime = 0.02; return string.format('%.3f', Engine.fixedDeltatime)"), "0.020");
    EXPECT_EQ(eval("Time.fixedDeltaTime = 0.04; return string.format('%.3f', Engine.updateTime)"), "0.040");
    EXPECT_EQ(eval("Time.maximumDeltaTime = 0.5; return string.format('%.2f %.2f', Engine.maxDeltatime, Time.maximumDeltaTime)"), "0.50 0.50");
    EXPECT_EQ(eval("Time.maximumDeltaTime = -1; return string.format('%.2f', Time.maximumDeltaTime)"), "0.50");
    Engine::setMaxDeltatime(0.25f);
    EXPECT_EQ(eval("Time.timeScale = 2; return Engine.timeScale"), "2.0");
    EXPECT_EQ(eval("return Time.frameCount == Engine.frameCount and Time.time == Engine.gameTime"), "true");
    EXPECT_EQ(eval("return Time.realtimeSinceStartup > 0 and Time.unscaledTime > 0"), "true");

    // timeSinceLevelLoad counts from the last scene load
    frames(3);
    EXPECT_GT(Engine::getTimeSinceLevelLoad(), 0.0);
    Engine::markLevelLoad();
    EXPECT_EQ(eval("return Time.timeSinceLevelLoad"), "0.0");
    frames(1);
    EXPECT_NEAR(Engine::getTimeSinceLevelLoad(), 2.0 * 0.02, 1e-6);
}

TEST_F(CoroutineTest, NothingRunsWhileTheGameEventsArePaused) {
    ASSERT_EQ(run("Coroutine.start(function() Coroutine.waitRealtime(0.05) Log('real') end) Timer.after(0, function() Log('timer') end, {unscaled = true})"), "");
    Engine::pauseGameEvents(true);
    frames(20);
    EXPECT_EQ(log(), "");
    Engine::pauseGameEvents(false);
    frames(1);
    EXPECT_EQ(log(), "timer");
    frames(1);
    EXPECT_EQ(log(), "timer");
    // 0.05 s = 3 frames of unpaused time
    frames(1);
    EXPECT_EQ(log(), "timer real");
}

// ================================================================== 13. C++

namespace {
    class TimedScript : public ScriptBase {
    public:
        std::vector<std::string>* events;
        TimedScript(Scene* scene, Entity entity, std::vector<std::string>* events): ScriptBase(scene, entity), events(events) {
            invoke([this](){ this->events->push_back("once"); }, 0.05f);
            invokeRepeating([this](){ this->events->push_back("rep"); }, 0.0f, 0.0001f);
            unscaledId = invoke([this](){ this->events->push_back("unscaled"); }, 0.05f, TimeMode::Unscaled);
        }
        TaskId unscaledId = 0;
    };
}

TEST_F(CoroutineTest, CppTimersAndScriptBaseInvokeEndWithTheScript) {
    std::vector<std::string> events;
    auto script = std::make_unique<TimedScript>(scene.get(), NULL_ENTITY, &events);
    EXPECT_TRUE(script->isInvoking());
    EXPECT_TRUE(script->isInvoking(script->unscaledId));
    int freeCalls = 0;
    const TaskId free = ScriptScheduler::every(0.0, 0.0001, [&](){ freeCalls++; }, TimeMode::Scaled, nullptr, 3);
    const TaskId later = Timer::after(10.0, [&](){ freeCalls += 100; });
    frames(3);
    EXPECT_EQ(events.size(), 5u);   // rep in each frame, once and unscaled in frame 3
    EXPECT_EQ(freeCalls, 3);
    EXPECT_FALSE(ScriptScheduler::isPending(free));
    EXPECT_TRUE(Timer::isPending(later));
    EXPECT_TRUE(Timer::cancel(later));
    EXPECT_FALSE(Timer::cancel(later));
    script.reset();   // ~ScriptBase cancels its timers
    EXPECT_EQ(ScriptScheduler::timerCount(), 0u);
    frames(2);
    EXPECT_EQ(freeCalls, 3);
}

// runtime-core spec 6.1: without the editor's crash hook (an exported game) a C++ timer that
// throws is logged and the game goes on; a repeating one keeps its schedule
TEST_F(CoroutineTest, CppTimerExceptionIsLoggedAndTheGameGoesOn) {
    int calls = 0;
    const TaskId repeating = ScriptScheduler::every(0.0, 0.0001, [&calls]() {
        calls++;
        throw std::runtime_error("cpp timer boom");
    }, TimeMode::Scaled, nullptr, 3);
    frames(3);
    EXPECT_EQ(calls, 3) << "the timer fired every frame although it threw";
    EXPECT_FALSE(ScriptScheduler::isPending(repeating));
    const std::string text = logged();
    EXPECT_EQ(countOf(text, "timer:C++ failed: C++ exception std::runtime_error: cpp timer boom"), 1u)
        << "logged once a second: " << text;
}

TEST_F(CoroutineTest, CppTimersFireAtTheirFrame) {
    std::vector<std::string> events;
    {
        TimedScript script(scene.get(), NULL_ENTITY, &events);
        frames(3);
        // rep every frame; once and unscaled at 0.05 s = frame 3
        std::string joined;
        for (const std::string& e : events) joined += e + " ";
        EXPECT_EQ(joined, "rep rep once rep unscaled ");
        EXPECT_FALSE(script.cancelInvoke(script.unscaledId));   // it fired
        script.cancelAllInvokes();
        EXPECT_FALSE(script.isInvoking());
    }
    EXPECT_EQ(ScriptScheduler::timerCount(), 0u);
}

// ================================================================== no leaks, stats, re-entrancy

TEST_F(CoroutineTest, NothingLeaksThreadsOrReferences) {
    const size_t refsBefore = ScriptScheduler::luaRefCount();
    EXPECT_EQ(refsBefore, 0u);
    ASSERT_EQ(run(R"(
        for i = 1, 50 do
            Coroutine.start(function() Coroutine.waitUntil(function() return false end) end)
            Coroutine.start(function() Coroutine.waitFor(Timer.after(100, function() end)) end)
            Coroutine.start(function() return i end)
            Timer.every(1, function() end)
        end
        Kept = Coroutine.start(function() return 1, 2, 3 end)
    )"), "");
    EXPECT_GT(ScriptScheduler::luaRefCount(), 150u);
    frames(2);
    ScriptScheduler::clear();
    EXPECT_EQ(ScriptScheduler::coroutineCount(), 0u);
    EXPECT_EQ(ScriptScheduler::timerCount(), 0u);
    EXPECT_EQ(ScriptScheduler::luaRefCount(), 0u);
    run("Kept = nil collectgarbage('collect') collectgarbage('collect')");
    const int memoryKb = lua_gc(L, LUA_GCCOUNT, 0);
    // a second round leaves the same memory: the threads went
    ASSERT_EQ(run("for i = 1, 200 do Coroutine.start(function() Coroutine.nextFrame() end) end"), "");
    frames(2);
    ScriptScheduler::clear();
    run("collectgarbage('collect') collectgarbage('collect')");
    EXPECT_LE(lua_gc(L, LUA_GCCOUNT, 0), memoryKb + 16);
    EXPECT_EQ(ScriptScheduler::luaRefCount(), 0u);
}

TEST_F(CoroutineTest, FinishedHandlesKeepTheirResultsUntilCollected) {
    ASSERT_EQ(run("Kept = Coroutine.start(function() return 'r' end)"), "");
    frames(1);
    EXPECT_GE(ScriptScheduler::luaRefCount(), 1u);   // the results, for a later waitFor
    ASSERT_EQ(run("Kept = nil collectgarbage('collect') collectgarbage('collect')"), "");
    frames(1);
    EXPECT_EQ(ScriptScheduler::luaRefCount(), 0u);
}

TEST_F(CoroutineTest, StartingFromTimersPredicatesAndCoroutinesIsAllowed) {
    ASSERT_EQ(run(R"(
        Timer.after(0, function()
            Coroutine.start(function() Log("fromTimer" .. F()) Coroutine.nextFrame() Log("fromTimerNext" .. F()) end)
        end)
        Coroutine.start(function()
            Coroutine.waitUntil(function()
                Coroutine.start(function() Log("fromPredicate") end)
                return true
            end)
            local child = Coroutine.start(function() Coroutine.nextFrame() Log("nested") end)
            Coroutine.waitFor(child)
            Log("parentAfterChild")
        end)
    )"), "");
    frames(4);
    EXPECT_EQ(log(), "fromTimer1 fromPredicate fromTimerNext2 nested parentAfterChild");
}

TEST_F(CoroutineTest, HandlesAreUniquePerCoroutine) {
    EXPECT_EQ(eval("local co co = Coroutine.start(function() Same = Coroutine.running() Coroutine.nextFrame() end) return tostring(co == Same)"), "true");
    EXPECT_EQ(eval("local t = Timer.after(1, function() end) return tostring(t.interval) .. tostring(t.unscaled) .. string.format('%.1f', t.remaining)"),
              "0.0false1.0");
    run("Same = nil");
}

TEST_F(CoroutineTest, ManyWaitingCoroutinesAndTimersCostLittlePerFrame) {
    ASSERT_EQ(run(R"(
        for i = 1, 10000 do Coroutine.start(function() Coroutine.wait(1000 + i) end) end
        for i = 1, 1000 do Timer.after(1000 + i, function() end) end
    )"), "");
    frames(2);
    const auto begin = std::chrono::steady_clock::now();
    frames(20);
    const double perFrameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / 20.0;
    std::printf("[ScriptScheduler] 10000 coroutines waiting seconds + 1000 timers: %.3f ms per frame (whole frame)\n", perFrameMs);
    // the spec's target is 0.3 ms on desktop; generous here (shared, loaded test machines)
    EXPECT_LT(perFrameMs, 5.0);
    EXPECT_EQ(ScriptScheduler::coroutineCount(), 10000u);
}

// ================================================================== instance methods, C++ start

TEST_F(CoroutineTest, ScriptsWithTheirOwnMetatablesStillGetTheInstanceMethods) {
    addScript("Inherits", R"(
        local Base = {}
        Base.__index = Base
        function Base:hello() return "base" end
        local M = setmetatable({}, Base)
        function M:init()
            Log(self:hello())
            Log(type(self.startCoroutine))
            Log(tostring(self.missing))
        end
        return M
    )");
    addScript("Fallback", R"(
        local M = setmetatable({}, {__index = function(t, k) if k == "dynamic" then return "dyn" end end})
        function M:init() Log(self.dynamic) Log(type(self.invoke)) end
        return M
    )");
    addScript("SelfMeta", R"(
        local M = {}
        M.__index = M
        setmetatable(M, M)
        function M:init() Log(type(self.init)) end
        return M
    )");
    addScript("Own", R"(
        local M = {}
        function M:invoke() return "mine" end
        function M:init() Log(self:invoke()) Log(type(self.stopAllCoroutines)) end
        return M
    )");
    start();
    EXPECT_EQ(log(), "base function nil dyn function function mine function");
}

TEST_F(CoroutineTest, TheLifecycleStartRunsAsACoroutineAndMayWait) {
    // Unity's IEnumerator Start(): the lifecycle's start may wait (it runs as an engine coroutine)
    addScript("WaitingStart", R"(
        local M = {}
        function M:start()
            Log("start" .. F())
            Coroutine.wait(0.1)
            Log("waited" .. F())
        end
        return M
    )");
    start();
    frames(4);
    const std::string early = log();
    EXPECT_EQ(early.find("waited"), std::string::npos) << early;
    EXPECT_NE(early.find("start"), std::string::npos) << early;
    frames(10);
    const std::string done = log();
    EXPECT_NE(done.find("waited"), std::string::npos) << "0.1 s of 1/60 s frames: " << done;
    EXPECT_EQ(logged().find("not inside an engine coroutine"), std::string::npos) << logged();
}

TEST_F(CoroutineTest, CppStartsALuaCoroutineForAnInstance) {
    const Entity entity = addScript("Starter", R"(
        local M = {}
        function M:init() end
        function M:run() Log("start" .. F()) Coroutine.nextFrame() Log("resumed" .. F()) end
        return M
    )");
    start();
    lua_rawgeti(L, LUA_REGISTRYINDEX, static_cast<lua_Integer>(reinterpret_cast<intptr_t>(instanceOf(entity))));
    lua_getfield(L, -1, "run");
    lua_pushvalue(L, -2);
    const TaskId id = ScriptScheduler::startCoroutine(L, -3, -2, 1, "run");
    lua_pop(L, 3);
    EXPECT_NE(id, 0u);
    const std::vector<ScriptCoroutineInfo> infos = ScriptScheduler::coroutinesOf(instanceOf(entity));
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(infos[0].method, "run");
    EXPECT_EQ(infos[0].origin, "Starter.lua:4");
    EXPECT_EQ(infos[0].waiting, "frames 1");
    frames(1);
    EXPECT_EQ(log(), "start0 resumed1");
    // not a live instance: nothing starts
    lua_newtable(L);
    lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
    EXPECT_EQ(ScriptScheduler::startCoroutine(L, -2, -1, 0), 0u);
    lua_pop(L, 2);
}

// ================================================================== review regressions

TEST_F(CoroutineTest, AnIsDoneThatStopsTheWaitingCoroutineIsSafe) {
    const Entity entity = addScript("SelfStopper", R"(
        local M = {}
        function M:init()
            local me = self
            local byHandle = { }
            function byHandle:isDone() KeptA:stop() return true end
            local byOwner = { }
            function byOwner:isDone() me:stopAllCoroutines() return true end
            KeptA = Coroutine.start(function() Coroutine.waitFor(byHandle) Log("a-never") end)
            KeptB = self:startCoroutine(function() Coroutine.waitFor(byOwner) Log("b-never") end)
        end
        return M
    )");
    const Entity other = addScript("Deactivator", R"(
        local M = {}
        function M:init()
            local go = GameObject(self.scene, self.entity)
            local byDeactivation = { }
            function byDeactivation:isDone() go:setActive(false) collectgarbage("collect") return true end
            KeptC = self:startCoroutine(function() Coroutine.waitFor(byDeactivation) Log("c-never") end)
        end
        return M
    )");
    start();
    frames(3);
    EXPECT_EQ(log(), "");
    EXPECT_EQ(eval("return KeptA.status .. ' ' .. KeptB.status .. ' ' .. KeptC.status"), "stopped stopped stopped");
    EXPECT_FALSE(scene->isEntityActive(other));
    EXPECT_EQ(ScriptScheduler::coroutineCountOf(instanceOf(entity)), 0u);
    run("KeptA = nil KeptB = nil KeptC = nil");
}

TEST_F(CoroutineTest, ARepeatingCppTimerKeepsItsCallable) {
    std::vector<int> calls;
    ScriptScheduler::every(0.0, 0.0001, [&calls, n = 0]() mutable { calls.push_back(++n); }, TimeMode::Scaled, nullptr, 4);
    frames(5);
    EXPECT_EQ(calls, (std::vector<int>{1, 2, 3, 4}));
}

TEST_F(CoroutineTest, DeactivatingGameObjectsWithoutCoroutinesIsCheapWithManyCoroutines) {
    addScript("Many", R"(
        local M = {}
        function M:init() for i = 1, 10000 do self:startCoroutine(function() Coroutine.wait(1000) end) end end
        return M
    )");
    std::vector<Entity> pooled;
    for (int i = 0; i < 100; i++) pooled.push_back(addObject());
    start();
    const auto begin = std::chrono::steady_clock::now();
    for (int round = 0; round < 10; round++) {
        for (Entity e : pooled) scene->setEntityActive(e, false);
        for (Entity e : pooled) scene->setEntityActive(e, true);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    std::printf("[ScriptScheduler] 1000 deactivations with 10000 coroutines elsewhere: %.3f ms\n", ms);
    EXPECT_LT(ms, 20.0);   // was O(all coroutines) each: ~250 ms
    EXPECT_EQ(ScriptScheduler::coroutineCount(), 10000u);
}

TEST_F(CoroutineTest, TheUnscaledClockStopsWhileTheGameEventsArePaused) {
    frames(2);
    EXPECT_GT(Engine::getUnscaledDeltatime(), 0.0f);
    const double before = Engine::getUnscaledTime();
    Engine::pauseGameEvents(true);
    frames(5);
    EXPECT_EQ(Engine::getUnscaledDeltatime(), 0.0f);
    EXPECT_EQ(Engine::getUnscaledTime(), before);
    EXPECT_EQ(eval("return Time.unscaledDeltaTime"), "0.0");
    Engine::pauseGameEvents(false);
    frames(1);
    EXPECT_NEAR(Engine::getUnscaledTime(), before + 1.0 / 60.0, 1e-9);
}

TEST_F(CoroutineTest, AStoppedCoroutineDoesNotGoOnThroughAnImmediateWaitFor) {
    ASSERT_EQ(run(R"(
        local ended = Coroutine.start(function() return 1 end)
        Kept = Coroutine.start(function()
            Coroutine.running():stop()
            local ok = Coroutine.waitFor(ended)
            Log("never" .. tostring(ok))
        end)
    )"), "");
    EXPECT_EQ(log(), "");
    EXPECT_EQ(eval("return Kept.status"), "stopped");
}

TEST_F(CoroutineTest, CoroutineResumeOnAnEngineCoroutineIsRefusedAndStopIsSafe) {
    ASSERT_EQ(run(R"(
        Kept = Coroutine.start(function() T = coroutine.running() Coroutine.wait(10) Log("woke") end)
        local ok, err = coroutine.resume(T)
        Log("resume" .. tostring(ok) .. tostring(err:find("resumed by coroutine.resume") ~= nil))
    )"), "");
    EXPECT_EQ(log(), "resumefalsetrue");
    frames(1);
    EXPECT_EQ(eval("return Kept.status"), "error");
    EXPECT_NE(logged().find("resumed with coroutine.resume outside the engine"), std::string::npos) << logged();

    // a stop while user code runs the thread is deferred (never a close of a running thread)
    clearLog();
    ASSERT_EQ(run(R"(
        Kept2 = Coroutine.start(function()
            T = coroutine.running()
            coroutine.yield()
            Kept2:stop()
            Log("after-stop:" .. Kept2.status)
            coroutine.yield()
            Log("never")
        end)
        coroutine.resume(T)
    )"), "");
    frames(2);
    EXPECT_EQ(log(), "after-stop:suspended");
    EXPECT_EQ(eval("return Kept2.status"), "stopped");
    run("T = nil Kept2 = nil");
}

TEST_F(CoroutineTest, MutualWaitsAreAnError) {
    ASSERT_EQ(run(R"(
        Kept = Coroutine.start(function()
            local me = Coroutine.running()
            local other = Coroutine.start(function() local ok = Coroutine.waitFor(me) Log("other" .. tostring(ok)) end)
            local ok, err = pcall(Coroutine.waitFor, other)
            Log("cycle" .. tostring(ok) .. tostring(err:find("wait for each other forever") ~= nil))
            Coroutine.nextFrame()
            return "a"
        end)
    )"), "");
    frames(3);
    EXPECT_EQ(log(), "cyclefalsetrue othertrue");
}

TEST_F(CoroutineTest, WaitForEndOfFrameResumesAfterTheDraw) {
    Engine::onDraw.add("test", std::function<void()>([this](){ ASSERT_EQ(run("Log('draw' .. F())"), ""); }));
    addScript("EofDraw", R"(
        local M = {}
        function M:init()
            RegisterEngineEvent(self, "onPostUpdate")
            self:startCoroutine(function() for i = 1, 2 do Coroutine.waitForEndOfFrame() Log("eof" .. F()) end end)
        end
        function M:onPostUpdate() Log("post" .. F()) end
        return M
    )");
    start();
    Engine::systemDraw();
    Engine::systemDraw();
    Engine::onDraw.remove("test");
    EXPECT_EQ(log(), "post1 draw1 eof1 post2 draw2 eof2");
}

TEST_F(CoroutineTest, CoroutinesAndTimersFollowAGameObjectMovedToAnotherScene) {
    const Entity entity = addScript("Mover2", R"(
        local M = {}
        function M:init()
            Kept = self:startCoroutine(function() while true do Coroutine.nextFrame() Log("co") end end)
            self:invokeRepeating(function() Log("timer") end, 0, 0.0001)
        end
        return M
    )");
    start();
    frames(1);
    EXPECT_EQ(log(), "timer co");
    void* owner = instanceOf(entity);
    Scene other;
    // a running destination (as a loaded or the persistent scene): its activation changes reach
    // the scripts; a scene whose scripts never started observes none (script lifecycle)
    ScriptRuntime::awakeScene(&other);
    const std::vector<Entity> moved = ObjectManager::moveToScene(scene.get(), entity, &other);
    ASSERT_FALSE(moved.empty());
    clearLog();
    frames(1);
    EXPECT_EQ(log(), "timer co");
    EXPECT_EQ(ScriptScheduler::coroutineCountOf(owner), 1u);
    // the owner index follows: deactivating it in the new scene stops the coroutine
    other.setEntityActive(moved[0], false);
    EXPECT_EQ(eval("return Kept.status"), "stopped");
    LuaBinding::cleanupLuaScripts(&other);
    EXPECT_EQ(ScriptScheduler::timerCountOf(owner), 0u);
}

// ================================================================== one Lua Timer: oe-port's and upstream's scripts

// The global Timer is oe-port's (the C++ Scheduler: clock, catchUp, frame and fixed-step timers,
// Timer.run) with upstream's coroutine-era API on the same calls and handles (unscaled, delay,
// the callback's timer and its false, id / remaining / interval / unscaled / isDone, waitFor)
TEST_F(CoroutineTest, TheGlobalTimerRunsOePortAndUpstreamScripts) {
    Engine::setTimeScale(0.0f);
    ASSERT_EQ(run(R"(
        -- oe-port's scripts
        Old = Timer.after(0.05, function() Log("clock") end, {clock = "unscaled"})
        Timer.afterFrames(2, function() Log("frames" .. F()) end)
        Timer.nextFrame(function() Log("next" .. F()) end)
        Run = Timer.run(function() Timer.waitFrames(2) Log("run" .. F()) end)
        -- upstream's scripts
        New = Timer.after(0.05, function(t) Log("unscaled" .. tostring(t == New)) end, {unscaled = true})
        Scaled = Timer.after(0.05, function() Log("scaled") end)
    )"), "");
    EXPECT_EQ(eval("return tostring(Old:isActive()) .. tostring(New:isDone()) .. tostring(New.unscaled) .. tostring(Old.unscaled)"),
              "truefalsetruetrue");
    frames(6);
    EXPECT_NE(log().find("next1"), std::string::npos) << log();
    EXPECT_NE(log().find("frames2"), std::string::npos) << log();
    EXPECT_NE(log().find("run2"), std::string::npos) << log();
    EXPECT_NE(log().find("clock"), std::string::npos) << log();
    EXPECT_NE(log().find("unscaledtrue"), std::string::npos) << log();
    EXPECT_EQ(log().find("scaled "), std::string::npos) << log();
    EXPECT_EQ(eval("return tostring(Old:isActive()) .. tostring(Old:isDone()) .. tostring(Run:isActive()) .. tostring(Scaled:isActive())"),
              "falsetruefalsetrue");
    // a cancelled timer: isDone, cancel() says it stopped it once
    EXPECT_EQ(eval("return tostring(Scaled:cancel()) .. tostring(Scaled:cancel()) .. tostring(Scaled:isDone())"), "truefalsetrue");
    EXPECT_NE(eval("return tostring(Scaled)").find("cancelled"), std::string::npos);
    // either family's option errors
    EXPECT_NE(eval("return select(2, pcall(Timer.after, 1, function() end, {clock = 'moon'}))").find("clock must be"), std::string::npos);
    EXPECT_NE(eval("return select(2, pcall(Timer.after, 1, function() end, {delay = 1}))").find("opts.delay is for Timer.every"),
              std::string::npos);
    run("Old = nil New = nil Scaled = nil Run = nil");
}

// a coroutine waits for either family's timer handle; an owned timer ends with its script
TEST_F(CoroutineTest, WaitForAGlobalTimerAndOwnedTimersEndWithTheScript) {
    addScript("Owner", R"(
        local M = {}
        function M:init()
            Timer.every(0.0001, function() Log("tick") end, {owner = self})
            self:startCoroutine(function()
                local ok = Coroutine.waitFor(Timer.after(0.02, function() end, {clock = "game"}))
                Log("waited" .. tostring(ok))
            end)
        end
        return M
    )");
    start();
    frames(3);
    EXPECT_NE(log().find("waitedtrue"), std::string::npos) << log();
    LuaBinding::cleanupLuaScripts(scene.get());
    clearLog();
    frames(2);
    EXPECT_EQ(log(), "");
    EXPECT_EQ(luaTimersPending(), 0u);
}
