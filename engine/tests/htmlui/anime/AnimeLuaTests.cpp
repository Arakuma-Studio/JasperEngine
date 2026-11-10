// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Lua `anime` table (HtmlUIAnimeLua.cpp) in the engine's Lua state: plain table targets,
// callbacks and their order, playback control, timelines, stagger, eases, utils, the clock
// parameter, errors, and AnimEngine::reset() freeing every Lua reference.

#include "TestDocumentCommon.h"

#include "HtmlUIClock.h"
#include "LuaBinding.h"
#include "htmlui/anim/Anime.h"
#include "htmlui/anim/AnimLua.h"
#include "object/ui/UIElementInternal.h"

#include "lua.hpp"
#include "LuaBridge.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Property.h>

#include <gtest/gtest.h>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    class AnimeLuaTest : public ::testing::Test {
    protected:
        void SetUp() override{
            L = LuaBinding::getLuaState();
            ASSERT_NE(L, nullptr);
            AnimEngine::instance().reset();
            now = 1000;
            AnimEngine::instance().tickMilliseconds(now, 0);
            TestSystem::get().clearLogs();
        }

        void TearDown() override{
            run("t, a, tl, log = nil, nil, nil, nil");
            AnimEngine::instance().reset();
            lua_gc(L, LUA_GCCOLLECT, 0);
            EXPECT_EQ(AnimLua::getLiveLuaRefs(), 0u);
        }

        // "" when the chunk ran, else the error
        std::string run(const std::string& code){
            if (luaL_loadstring(L, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(L, 0, 0) != LUA_OK){
                std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
                lua_pop(L, 1);
                return error;
            }
            return "";
        }

        double number(const std::string& expression){
            std::string code = "return " + expression;
            if (luaL_loadstring(L, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(L, 0, 1) != LUA_OK){
                ADD_FAILURE() << expression << ": " << (lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
                lua_pop(L, 1);
                return std::nan("");
            }
            double v = lua_isnumber(L, -1) ? (double)lua_tonumber(L, -1) : std::nan("");
            lua_pop(L, 1);
            return v;
        }

        std::string text(const std::string& expression){
            std::string code = "return tostring(" + expression + ")";
            if (luaL_loadstring(L, code.c_str()) != LUA_OK || LuaBinding::pcallWithTraceback(L, 0, 1) != LUA_OK){
                std::string error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
                lua_pop(L, 1);
                return "error: " + error;
            }
            std::string v = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
            lua_pop(L, 1);
            return v;
        }

        void advance(double ms){
            now += ms;
            AnimEngine::instance().tickMilliseconds(now, 0);
        }

        lua_State* L = nullptr;
        double now = 0;
    };

}

TEST_F(AnimeLuaTest, PlainTableTargets){
    ASSERT_EQ(run(R"(
        t = { x = 0, y = 10, color = "#ff0000", label = "keep" }
        a = anime.animate(t, { x = 100, y = "+=10", color = "#0000ff", duration = 1000, ease = "linear" })
    )"), "");
    advance(500);
    EXPECT_NEAR(number("t.x"), 50, 1e-9);
    EXPECT_NEAR(number("t.y"), 15, 1e-9);
    // colours in tables come back as #rrggbbaa
    EXPECT_EQ(text("t.color"), "#b400b4ff");
    EXPECT_EQ(text("t.label"), "keep");
    EXPECT_NEAR(number("a.progress"), 0.5, 1e-9);
    EXPECT_NEAR(number("a.currentTime"), 500, 1e-9);
    EXPECT_EQ(text("a.paused"), "false");
    advance(600);
    EXPECT_EQ(number("t.x"), 100);
    EXPECT_EQ(text("t.color"), "#0000ffff");
    EXPECT_EQ(text("a.completed"), "true");
    // a list of tables
    ASSERT_EQ(run("t = { {v = 0}, {v = 0} }; anime.animate(t, { v = function(target, i, total) return (i + 1) * 10 * total end, duration = 0 })"), "");
    EXPECT_EQ(number("t[1].v"), 20);
    EXPECT_EQ(number("t[2].v"), 40);
}

TEST_F(AnimeLuaTest, DetachedPreviewReleasesLuaCallbacksAtPinnedTime){
    AnimEngine& engine = AnimEngine::instance();
    auto unrelated = Anime::createTimer();
    const size_t initialLive = AnimEngine::getLiveTimerCount();
    const size_t initialLuaRefs = AnimLua::getLiveLuaRefs();
    std::vector<std::shared_ptr<AnimTimer>> roots;
    {
        AnimEngine::ScopedTickPause pause(&roots);
        ASSERT_EQ(run("t = {x = 0}; a = anime.animate(t, {x = 100, duration = 1000, onUpdate = function() t.seen = true end})"), "");
    }
    EXPECT_EQ(roots.size(), 1u);
    EXPECT_GT(AnimLua::getLiveLuaRefs(), initialLuaRefs);
    for (const auto& root : roots){
        root->paused = true;
        root->cancel();
        engine.detachRoot(*root);
    }
    roots.clear();
    ASSERT_EQ(run("a, t = nil, nil; collectgarbage('collect')"), "");
    EXPECT_EQ(AnimLua::getLiveLuaRefs(), initialLuaRefs);
    EXPECT_EQ(AnimEngine::getLiveTimerCount(), initialLive);
    EXPECT_EQ(engine.getActiveCount(), 1u);
    EXPECT_TRUE(unrelated->running);
    EXPECT_FALSE(unrelated->isCancelled());
    EXPECT_DOUBLE_EQ(engine.now(AnimClock::UI), now);
    EXPECT_DOUBLE_EQ(engine.now(AnimClock::GAME), 0);
}

TEST_F(AnimeLuaTest, PreviewReferenceCyclesReleaseOnlyTheirOwnContext){
    AnimEngine& engine = AnimEngine::instance();
    ASSERT_EQ(run("t = {x = 0}; a = anime.animate(t, {x = 100, duration = 1000, onUpdate = function() t.seen = true end})"), "");
    const size_t outsideRefs = AnimLua::getLiveLuaRefs();
    const size_t outsideTimers = AnimEngine::getLiveTimerCount();
    const int firstCache = LuaBinding::createPreviewModuleCache();
    const int secondCache = LuaBinding::createPreviewModuleCache();
    ASSERT_GE(firstCache, 0);
    ASSERT_GE(secondCache, 0);
    std::vector<std::shared_ptr<AnimTimer>> roots;
    {
        LuaBinding::ScopedModuleCache modules(firstCache);
        AnimEngine::ScopedTickPause pause(&roots);
        ASSERT_EQ(run(R"(
            local m = {x = 0}
            m.a = anime.animate(m, {x = 1, duration = 1000, onUpdate = function() m.seen = true end})
            log = setmetatable({m}, {__mode = 'v'})
        )"), "");
    }
    const size_t firstRefs = AnimLua::getLiveLuaRefs() - outsideRefs;
    EXPECT_GT(firstRefs, 0u);
    ASSERT_EQ(roots.size(), 1u);
    const auto disposedTarget = std::static_pointer_cast<AnimAnimation>(roots[0])->head->target;
    {
        LuaBinding::ScopedModuleCache modules(secondCache);
        AnimEngine::ScopedTickPause pause(&roots);
        ASSERT_EQ(run(R"(
            local m = {x = 0}
            m.a = anime.animate(m, {x = 1, duration = 1000, onUpdate = function() m.seen = true end})
            tl = setmetatable({m}, {__mode = 'v'})
        )"), "");
    }
    const size_t secondRefs = AnimLua::getLiveLuaRefs() - outsideRefs - firstRefs;
    EXPECT_GT(secondRefs, 0u);
    for (const auto& root : roots){
        root->paused = true;
        root->cancel();
        engine.detachRoot(*root);
    }
    roots.clear();
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    EXPECT_EQ(AnimLua::getLiveLuaRefs(), outsideRefs + firstRefs + secondRefs);
    EXPECT_EQ(text("log[1] == nil"), "false");
    EXPECT_EQ(text("tl[1] == nil"), "false");

    AnimLua::releasePreviewRefs(firstCache);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    EXPECT_EQ(text("log[1] == nil"), "true");
    EXPECT_EQ(text("tl[1] == nil"), "false");
    EXPECT_FALSE(disposedTarget->isValid());
    EXPECT_EQ(AnimLua::getLiveLuaRefs(), outsideRefs + secondRefs);
    AnimLua::releasePreviewRefs(firstCache);
    AnimLua::releasePreviewRefs(-1);
    EXPECT_EQ(AnimLua::getLiveLuaRefs(), outsideRefs + secondRefs);

    AnimLua::releasePreviewRefs(secondCache);
    LuaBinding::releaseLuaRef(firstCache);
    LuaBinding::releaseLuaRef(secondCache);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_gc(L, LUA_GCCOLLECT, 0);
    EXPECT_EQ(text("tl[1] == nil"), "true");
    EXPECT_EQ(AnimLua::getLiveLuaRefs(), outsideRefs);
    EXPECT_EQ(AnimEngine::getLiveTimerCount(), outsideTimers);
    EXPECT_EQ(engine.getActiveCount(), 1u);
    ASSERT_EQ(run("a:seek(500); assert(t.seen and t.x == 50)"), "");
    EXPECT_DOUBLE_EQ(engine.now(AnimClock::UI), now);
    EXPECT_DOUBLE_EQ(engine.now(AnimClock::GAME), 0);
}

TEST_F(AnimeLuaTest, CallbacksInOrderWithLoopsAndAlternate){
    ASSERT_EQ(run(R"(
        log = {}
        local function note(name) return function(anim) log[#log + 1] = name .. ":" .. anim.currentIteration end end
        t = { x = 0 }
        a = anime.animate(t, { x = 100, duration = 100, loop = 2, alternate = true, ease = "linear",
            onBegin = note("begin"), onLoop = note("loop"), onComplete = note("complete") })
        a:andThen(function(anim) log[#log + 1] = "then" end)
    )"), "");
    for (int i = 0; i < 20; i++)
        advance(20);
    EXPECT_EQ(text("table.concat(log, ' ')"), "begin:0 loop:1 loop:2 complete:2 then");
    // forwards, backwards, forwards
    EXPECT_EQ(number("t.x"), 100);
    // already completed: at once
    ASSERT_EQ(run("log = {}; a:andThen(function() log[1] = 'again' end)"), "");
    EXPECT_EQ(text("log[1]"), "again");
}

TEST_F(AnimeLuaTest, PlaybackControl){
    ASSERT_EQ(run("t = { x = 0 }; a = anime.animate(t, { x = 100, duration = 1000, ease = 'linear', autoplay = false })"), "");
    advance(100);
    EXPECT_EQ(number("t.x"), 0);
    EXPECT_EQ(number("a:seek(250).currentTime"), 250);
    EXPECT_NEAR(number("t.x"), 25, 1e-9);
    ASSERT_EQ(run("a:play()"), "");
    advance(250);
    EXPECT_NEAR(number("t.x"), 50, 1e-9);
    ASSERT_EQ(run("a:pause()"), "");
    advance(250);
    EXPECT_NEAR(number("t.x"), 50, 1e-9);
    ASSERT_EQ(run("a:reverse()"), "");
    advance(100);
    EXPECT_NEAR(number("t.x"), 40, 1e-9);
    ASSERT_EQ(run("a.speed = 2; a:play()"), "");
    advance(100);
    EXPECT_NEAR(number("t.x"), 60, 1e-9);
    ASSERT_EQ(run("a.progress = 0.9"), "");
    EXPECT_NEAR(number("t.x"), 90, 1e-9);
    ASSERT_EQ(run("a:restart()"), "");
    advance(100);
    EXPECT_NEAR(number("t.x"), 20, 1e-9);
    ASSERT_EQ(run("a:complete()"), "");
    EXPECT_EQ(number("t.x"), 100);
    EXPECT_EQ(text("a.cancelled"), "true");
    ASSERT_EQ(run("a:revert()"), "");
    EXPECT_EQ(number("t.x"), 0);
    // chaining keeps the class: timeline methods after a timer method
    ASSERT_EQ(run("tl = anime.createTimeline({ autoplay = false }); tl:pause():add(t, { x = 5, duration = 10 }):label('end')"), "");
    EXPECT_EQ(number("tl.labels['end']"), 10);
}

TEST_F(AnimeLuaTest, TimelineWithLabelsAndPositions){
    ASSERT_EQ(run(R"(
        log = {}
        t = { x = 0, y = 0 }
        tl = anime.createTimeline({ defaults = { duration = 100, ease = "linear" }, autoplay = false })
        tl:add(t, { x = 100 })
          :label("half")
          :add(t, { y = 50 }, "-=50")
          :call(function(timeline) log[#log + 1] = "call" end, "half+=25")
          :set(t, { x = 7 }, "<<")
          :add({ duration = 30, onComplete = function() log[#log + 1] = "timer" end }, "+=10")
    )"), "");
    EXPECT_EQ(number("tl.labels.half"), 100);
    EXPECT_EQ(number("tl.duration"), 190);
    ASSERT_EQ(run("tl:seek(75)"), "");
    EXPECT_NEAR(number("t.x"), 75, 1e-9);
    EXPECT_NEAR(number("t.y"), 12.5, 1e-9);
    ASSERT_EQ(run("tl:seek(130)"), "");
    EXPECT_EQ(number("t.x"), 7);
    EXPECT_EQ(text("log[1]"), "call");
    ASSERT_EQ(run("tl:play()"), "");
    advance(100);
    EXPECT_EQ(text("tl.completed"), "true");
    EXPECT_EQ(text("table.concat(log, ' ')"), "call timer");
    // a stagger as the position: one child per target
    ASSERT_EQ(run(R"(
        t = { {v = 0}, {v = 0}, {v = 0} }
        tl = anime.createTimeline({ autoplay = false })
        tl:add(t, { v = 1, duration = 100 }, anime.stagger(50))
    )"), "");
    EXPECT_EQ(number("tl.duration"), 200);
}

TEST_F(AnimeLuaTest, StaggerEasesAndUtils){
    // stagger values (index 0-based, as Anime)
    EXPECT_EQ(number("anime.stagger(100)(nil, 2, 5)"), 200);
    EXPECT_EQ(number("anime.stagger(100, { from = 'center' })(nil, 0, 5)"), 200);
    EXPECT_EQ(text("anime.stagger('10px')(nil, 3, 4)"), "30px");
    EXPECT_EQ(number("anime.stagger({0, 360})(nil, 4, 5)"), 360);
    // eases
    EXPECT_DOUBLE_EQ(number("anime.eases.inQuad(0.5)"), 0.25);
    EXPECT_EQ(text("anime.eases.outElastic(1, .6).name"), "outElastic(...)");
    EXPECT_NEAR(number("anime.eases.outElastic(1, .6)(0.25)"), AnimEasing::parse("outElastic(1, .6)")(0.25), 1e-12);
    EXPECT_NEAR(number("anime.cubicBezier(.25, .1, .25, 1)(0.5)"), AnimEasing::cubicBezier(.25, .1, .25, 1)(0.5), 1e-12);
    EXPECT_EQ(number("anime.steps(4)(0.6)"), 0.5);
    EXPECT_EQ(number("anime.linear(0, '0.8 25%', 1)(0.25)"), 0.8);
    EXPECT_GT(number("anime.createSpring({ bounce = 0.5, duration = 400 }).settlingDuration"), 400);
    EXPECT_EQ(number("anime.spring({ stiffness = 200 }).stiffness"), 200);
    // a Lua function as ease and modifier, an ease object, a spring
    ASSERT_EQ(run(R"(
        t = { a = 0, b = 0, c = 0, d = 0 }
        anime.animate(t, { a = { to = 100, ease = function(p) return p * p end },
                           b = { to = 10, modifier = anime.utils.round(0) },
                           c = { to = 100, ease = anime.eases.inCubic },
                           d = { to = 100, ease = anime.createSpring({ stiffness = 300, damping = 20 }) },
                           duration = 1000, ease = "linear" })
    )"), "");
    advance(500);
    EXPECT_NEAR(number("t.a"), 25, 1e-9);
    EXPECT_EQ(number("t.b"), 5);
    EXPECT_NEAR(number("t.c"), 12.5, 1e-9);
    EXPECT_GT(number("t.d"), 0);
    // utils
    EXPECT_EQ(number("anime.utils.clamp(5, 0, 3)"), 3);
    EXPECT_EQ(number("anime.utils.clamp(0, 3)(-1)"), 0);
    EXPECT_EQ(number("anime.utils.round(2.567, 2)"), 2.57);
    EXPECT_EQ(number("anime.utils.round(0)(2.6)"), 3);
    EXPECT_EQ(number("anime.utils.snap(17, 5)"), 15);
    EXPECT_EQ(number("anime.utils.snap(17, {0, 10, 20})"), 20);
    EXPECT_EQ(number("anime.utils.wrap(370, 0, 360)"), 10);
    EXPECT_EQ(number("anime.utils.mapRange(5, 0, 10, 0, 100)"), 50);
    EXPECT_EQ(number("anime.utils.lerp(0, 100, 0.25)"), 25);
    EXPECT_EQ(number("anime.utils.lerp(0, 100)(0.5)"), 50);
    EXPECT_NEAR(number("anime.utils.degToRad(180)"), 3.14159265358979, 1e-12);
    EXPECT_NEAR(number("anime.utils.radToDeg(math.pi)"), 180, 1e-12);
    EXPECT_EQ(text("anime.utils.roundPad(1.5, 3)"), "1.500");
    EXPECT_EQ(text("anime.utils.padStart(5, 3, '0')"), "005");
    EXPECT_EQ(text("anime.utils.padEnd('ab', 5, 'xy')"), "abxyx");
    EXPECT_NEAR(number("anime.utils.damp(0, 100, 16, 0.5)"), AnimUtils::damp(0, 100, 16, 0.5), 1e-12);
    const double r = number("anime.utils.random(5, 10)");
    EXPECT_GE(r, 5);
    EXPECT_LE(r, 10);
    EXPECT_EQ(number("anime.utils.createSeededRandom(1234)(0, 100)"), AnimUtils::createSeededRandom(1234)(0, 100, 0));
    EXPECT_EQ(text("({a=1})[1] == nil and anime.utils.randomPick({'only'})"), "only");
    EXPECT_EQ(number("#anime.utils.shuffle({1, 2, 3, 4})"), 4);
    ASSERT_EQ(run("t = { x = 3 }; anime.utils.set(t, { x = 42 })"), "");
    EXPECT_EQ(number("t.x"), 42);
    EXPECT_EQ(number("anime.utils.get(t, 'x')"), 42);
    ASSERT_EQ(run("t = { x = 0 }; a = anime.animate(t, { x = 100, duration = 1000 }); anime.utils.remove(t)"), "");
    EXPECT_EQ(text("a.cancelled"), "true");
}

TEST_F(AnimeLuaTest, ClockParameterAndErrors){
    ASSERT_EQ(run("a = anime.createTimer({ duration = 100, clock = 'game' })"), "");
    EXPECT_EQ(number("a.duration"), 100);
    EXPECT_NE(run("anime.animate({ x = 0 }, { x = 1, clock = 'wall' })").find("clock must be \"ui\""), std::string::npos);
    const std::string error = run("anime.createTimeline({ clock = 5 })");
    EXPECT_NE(error.find("\"ui\""), std::string::npos) << error;
    EXPECT_NE(error.find("\"game\""), std::string::npos) << error;
    EXPECT_NE(run("anime.animate({ x = 0 })").find("params must be a table"), std::string::npos);
    EXPECT_NE(run("anime.createTimeline():add(5)").find("AnimeTimeline:add"), std::string::npos);
    // a no-target animation warns, never fails silently
    TestSystem::get().clearLogs();
    ASSERT_EQ(run("anime.animate('#nothing-matches', { opacity = 0 })"), "");
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "no target found"), 1u);
    // an error in a callback is logged with the traceback, the animation goes on
    ASSERT_EQ(run("t = { x = 0 }; a = anime.animate(t, { x = 10, duration = 100, ease = 'linear', onUpdate = function() error('boom') end })"), "");
    advance(50);
    EXPECT_GE(TestSystem::get().countLogs(D_LOG_ERROR, "boom"), 1u);
    EXPECT_NEAR(number("t.x"), 5, 1e-9);
}

TEST_F(AnimeLuaTest, ResetFreesEveryLuaReference){
    ASSERT_EQ(run(R"(
        t = { x = 0 }
        a = anime.animate(t, { x = function() return 100 end, duration = 1000, onUpdate = function() end,
                               ease = function(p) return p end })
        a:andThen(function() end)
        tl = anime.createTimeline({ onComplete = function() end })
        tl:add(t, { y = 5 }):call(function() end)
    )"), "");
    advance(100);
    EXPECT_GT(AnimLua::getLiveLuaRefs(), 4u);
    EXPECT_EQ(AnimEngine::instance().getActiveCount(), 2u);
    AnimEngine::instance().reset();
    EXPECT_EQ(AnimLua::getLiveLuaRefs(), 0u);
    EXPECT_EQ(AnimEngine::instance().getActiveCount(), 0u);
    // handles scripts keep are cancelled and harmless
    EXPECT_EQ(text("a.cancelled"), "true");
    ASSERT_EQ(run("a:play(); a:seek(500)"), "");
    advance(100);
    ASSERT_EQ(run("a = nil; tl = nil; t = nil"), "");
    lua_gc(L, LUA_GCCOLLECT, 0);
    advance(100);
    EXPECT_EQ(AnimEngine::getLiveTimerCount(), 0u);
}

TEST_F(AnimeLuaTest, SelectorsOnDocuments){
    ASSERT_TRUE(GpuEnvironment::ensure());
    SceneFixture fixture;
    UIDocument& document = fixture.add<UIDocument>();
    ASSERT_TRUE(document.load("p1/anime.rml"));
    HtmlUIClock::setFixedTime(1.0);
    AnimEngine::instance().reset();
    AnimEngine::instance().tick(HtmlUIClock::now());
    ASSERT_EQ(run("a = anime.animate('.card', { opacity = 0.25, translateX = 20, duration = 100, delay = anime.stagger(10) })"), "");
    EXPECT_EQ(number("#a.targets"), 3);
    EXPECT_EQ(text("a.targets[1].className"), "card");
    HtmlUIClock::setFixedTime(1.5);
    AnimEngine::instance().tick(HtmlUIClock::now());
    Rml::Element* first = UIElementAccess::get(document.getElementById("box"));
    ASSERT_NE(first, nullptr);
    Rml::ElementList list;
    first->GetOwnerDocument()->QuerySelectorAll(list, ".card");
    ASSERT_EQ(list.size(), 3u);
    for (Rml::Element* card : list){
        const Rml::Property* opacity = card->GetLocalProperty(Rml::PropertyId::Opacity);
        ASSERT_NE(opacity, nullptr);
        EXPECT_FLOAT_EQ(opacity->Get<float>(), 0.25f);
        EXPECT_NE(card->GetLocalProperty(Rml::PropertyId::Transform), nullptr);
    }
    // utils.get on a selector
    EXPECT_EQ(text("anime.utils.get('#box', 'width')"), "100px");
    AnimEngine::instance().reset();
    HtmlUIClock::reset();
}

TEST_F(AnimeLuaTest, UIElementHandlesFromLua){
    ASSERT_TRUE(GpuEnvironment::ensure());
    SceneFixture fixture;
    UIDocument& document = fixture.add<UIDocument>();
    ASSERT_TRUE(document.load("p1/anime.rml"));
    HtmlUIClock::setFixedTime(1.0);
    AnimEngine::instance().reset();
    AnimEngine::instance().tick(HtmlUIClock::now());
    updateDocuments(fixture.getScene());
    ASSERT_TRUE(luabridge::push(L, document));
    lua_setglobal(L, "doc");

    // a UIElement handle, a list from querySelectorAll, function values receiving the elements
    ASSERT_EQ(run(R"(
        box = doc:getElementById('box')
        a = anime.animate(box, { translateX = 100, duration = 100, ease = 'linear' })
        cards = anime.animate(doc:querySelectorAll('.card'), {
            translateX = function(el, i, total) return el.className == 'card' and (i + 1) * 10 or -1 end, duration = 0 })
    )"), "");
    EXPECT_EQ(number("#a.targets"), 1);
    EXPECT_EQ(text("a.targets[1].id"), "box");
    EXPECT_EQ(text("a.targets[1] == box"), "true");
    EXPECT_EQ(number("#cards.targets"), 3);

    // the selector root: a UIDocument, an element, an element without matches (a warning)
    EXPECT_EQ(number("#anime.animate('.card', { opacity = 0.5, duration = 0, root = doc }).targets"), 3);
    EXPECT_EQ(number("#anime.animate('.card', { opacity = 0.5, duration = 0, root = doc:getElementById('holder') }).targets"), 3);
    TestSystem::get().clearLogs();
    EXPECT_EQ(number("#anime.animate('.card', { opacity = 0.5, duration = 0, root = box }).targets"), 0);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "no target found"), 1u);

    HtmlUIClock::setFixedTime(1.2);
    AnimEngine::instance().tick(HtmlUIClock::now());
    // RmlUi computes transform states when it renders: a frame (the pinned clock does not move)
    GpuEnvironment::drawFrames(1);
    // the element API sees the transforms anime wrote
    EXPECT_NEAR(number("box:getBoundingClientRect().left"), 110, 1e-3);
    EXPECT_NEAR(number("box:getBoundingClientRect().width"), 100, 1e-3);
    EXPECT_NEAR(number("doc:querySelectorAll('.card')[2]:getBoundingClientRect().left - doc:getElementById('holder'):getBoundingClientRect().left"), 2 + 20, 1e-3);
    EXPECT_EQ(text("anime.utils.get(box, 'translateX')"), "100px");

    // a gone element: the handle is invalid, the animation on it cancelled silently
    ASSERT_EQ(run("gone = anime.animate(doc:getElementById('holder'):querySelector('.card'), { x = 50, duration = 1000 })"), "");
    ASSERT_EQ(run("doc:getElementById('holder').innerHTML = ''"), "");
    HtmlUIClock::setFixedTime(1.3);
    AnimEngine::instance().tick(HtmlUIClock::now());
    EXPECT_EQ(text("gone.cancelled"), "true");

    ASSERT_EQ(run("doc, box, a, cards, gone = nil, nil, nil, nil, nil"), "");
    lua_gc(L, LUA_GCCOLLECT, 0);
    AnimEngine::instance().reset();
    HtmlUIClock::reset();
}
