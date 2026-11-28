// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// S95 REQ-14: the canvas a script's init reads. A platform that knows its screen before the view
// exists (a desktop window's size, the web canvas, the iOS view) sets it first, and init reads the
// first frame's layout; without it (Android before its window) init reads the preferred size, and
// onCanvasChanged comes when the view loads, before the first onUpdate.

#include "ScriptTestHarness.h"
#include "EngineTestAccess.h"

namespace EngineCanvasTest_cpp {

using namespace jasperengine;
using namespace jasperengine::scripttest;

namespace {

    class ScreenSystem : public LuaTestSystem {
    public:
        int width = 0;
        int height = 0;
        int getScreenWidth() override { return width; }
        int getScreenHeight() override { return height; }
    };

    class EngineCanvasTest : public ScriptTest {
    protected:
        bool previousViewLoaded = false;
        Scaling previousScaling = Scaling::FITWIDTH;
        int previousWidth = 0;
        int previousHeight = 0;

        void SetUp() override {
            ScriptTest::SetUp();
            previousViewLoaded = EngineTestAccess::setViewLoaded(false);
            previousScaling = Engine::getScalingMode();
            previousWidth = Engine::getPreferredCanvasWidth();
            previousHeight = Engine::getPreferredCanvasHeight();
            writeScript("CanvasProbe",
                "local M = {}\n"
                "function M:init() log[#log + 1] = 'init' .. Engine.canvasWidth .. 'x' .. Engine.canvasHeight; "
                "RegisterEngineEvent(self, 'onUpdate') end\n"
                "function M:onUpdate() if not self.updated then self.updated = true; log[#log + 1] = 'update' end end\n"
                "return M\n");
        }

        void TearDown() override {
            Engine::setStartScreenSize(0, 0);
            EngineTestAccess::setViewLoaded(previousViewLoaded);
            Engine::setScalingMode(previousScaling);
            Engine::setCanvasSize(previousWidth, previousHeight);
            ScriptTest::TearDown();
        }

        // a game's start: its main sets the canvas, then the start scene's scripts run
        void startGame() {
            Engine::setScalingMode(Scaling::FITHEIGHT);
            Engine::setCanvasSize(1920, 1080);
            Scene* scene = newScene();
            addScript(scene, entity(scene, "Probe"), "CanvasProbe");
            LuaBinding::initializeLuaScripts(scene);
            Engine::setScene(scene);
        }
    };

}

TEST_F(EngineCanvasTest, InitSeesFirstLayout) {
    Engine::setStartScreenSize(2400, 1080);
    startGame();
    EXPECT_EQ(takeLog(), "init2400x1080") << "FITHEIGHT on a 2400 x 1080 screen, before the view exists";
    EXPECT_EQ(Engine::getPreferredCanvasWidth(), 1920);
}

TEST_F(EngineCanvasTest, WithoutAScreenInitSeesThePreferredSizeAndTheChangeComesFirst) {
    startGame();
    EXPECT_EQ(takeLog(), "init1920x1080");
    EXPECT_NE(Engine::getCanvasWidth(), 0);

    // the window comes (Android): the view loads, then the frames
    ScreenSystem screen;
    screen.width = 2400;
    screen.height = 1080;
    screen.luaPath = system.luaPath;
    System* previous = SystemTestAccess::swap(&screen);
    Engine::onViewChanged.add("canvastest", std::function<void()>([this]() {
        lua("log[#log + 1] = 'canvas' .. Engine.canvasWidth .. 'x' .. Engine.canvasHeight");
    }));
    EngineTestAccess::setViewLoaded(true);
    Engine::systemViewChanged();
    frame();
    Engine::onViewChanged.remove("canvastest");
    SystemTestAccess::swap(previous);
    EXPECT_EQ(takeLog(), "canvas2400x1080 update");
}

}  // namespace EngineCanvasTest_cpp
