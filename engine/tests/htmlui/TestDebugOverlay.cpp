// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The on-device debug overlay's document (feature debug-overlay, spec 10, tests 7-11, 13, 14):
// nothing exists while it is off; the backquote key and the three-finger hold toggle the
// console, loaded from engine:// with no project file; filters, search, collapse and row
// expansion; copy and save; the stats panel; the overlay drawn above the game and offered
// input first, consuming only taps on its own panels. The log and commands are tested by
// editor/tests/LogListenerTest.cpp.

#include "InspectorTestCommon.h"

#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "debug/DebugOverlay.h"
#include "debug/DebugStats.h"
#include "harness/Readback.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    const char* callbackTag = "htmlui-tests-debug-overlay";

    class DebugOverlayTest : public InspectorTestBase {
    protected:
        std::unique_ptr<SceneFixture> fixture;
        int gameTouches = 0;
        int gameKeys = 0;
        size_t bufferSize = 500;

        void SetUp() override{
            InspectorTestBase::SetUp();
            if (HasFatalFailure())
                return;
            fixture.reset(new SceneFixture());
            Engine::onTouchStart.add(callbackTag, std::function<void(int, float, float)>([this](int, float, float){ gameTouches++; }));
            Engine::onKeyDown.add(callbackTag, std::function<void(int, bool, int)>([this](int, bool, int){ gameKeys++; }));
            bufferSize = Log::getBufferSize();
            Log::clearEntries();
        }

        void TearDown() override{
            Engine::onTouchStart.remove(callbackTag);
            Engine::onKeyDown.remove(callbackTag);
            Engine::systemTouchCancel();
            Engine::setFixedDeltatime(0.0);
            DebugOverlay::shutdown();
            Log::setBufferSize(bufferSize);
            Log::clearEntries();
            fixture.reset();
            EXPECT_EQ(TestSystem::get().countLogs(D_LOG_ERROR), 0u) << logText(D_LOG_ERROR);
            InspectorTestBase::TearDown();
        }

        void enable(DebugOverlaySettings settings = DebugOverlaySettings()){
            settings.enabled = true;
            DebugOverlay::configure(settings);
        }

        Rml::ElementDocument* document(){
            Scene* scene = DebugOverlay::getScene();
            return scene ? scene->getSystem<UIDocumentSystem>()->getDocument(DebugOverlay::getDocumentEntity()) : nullptr;
        }

        Rml::Element* byId(const char* id){
            Rml::ElementDocument* doc = document();
            return doc ? doc->GetElementById(id) : nullptr;
        }

        // laid out (not display: none), what the class alone does not tell
        bool shown(const char* id){
            Rml::Element* element = byId(id);
            return element && element->GetComputedValues().display() != Rml::Style::Display::None;
        }

        int rows(){
            Rml::Element* list = byId("list");
            return list ? list->GetNumChildren() : -1;
        }

        void key(int code){
            Engine::systemKeyDown(code, false, 0);
            Engine::systemKeyUp(code, false, 0);
        }

        void click(const char* id){
            Rml::Element* element = byId(id);
            ASSERT_NE(element, nullptr) << id;
            element->Click();
            GpuEnvironment::drawFrames(1);
        }

        void tap(float x, float y, int pointer = 0){
            Engine::systemTouchStart(pointer, x, y);
            Engine::systemTouchEnd(pointer, x, y);
        }

        void search(const std::string& text){
            auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(byId("search"));
            ASSERT_NE(control, nullptr);
            control->SetValue(text);
            control->DispatchEvent(Rml::EventId::Change, Rml::Dictionary());
            GpuEnvironment::drawFrames(1);
        }

        void logKnownSet(){
            Log::print("alpha info");
            Log::warn("beta warning");
            Log::error("gamma error\nstack traceback:\n\tgame.lua:12: in function 'hit'");
            Log::error("gamma error\nstack traceback:\n\tgame.lua:12: in function 'hit'");
            Log::print("delta info");
        }

        // the RGBA of the target at a pixel (rows top first)
        std::array<int, 4> pixel(int x, int y){
            std::vector<uint8_t> rgba;
            Framebuffer& target = GpuEnvironment::target();
            const int width = (int)target.getWidth();
            const int height = (int)target.getHeight();
            EXPECT_TRUE(readTexture(target.getRender().getColorTexture(), width, height, rgba));
            if (rgba.size() < (size_t)width * height * 4)
                return {-1, -1, -1, -1};
            const size_t at = ((size_t)y * width + x) * 4;
            return {rgba[at], rgba[at + 1], rgba[at + 2], rgba[at + 3]};
        }
    };

}

// 7. off: no Scene, whatever keys and touches come
TEST_F(DebugOverlayTest, NothingExistsWhileOff){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    DebugOverlay::configure(DebugOverlaySettings());
    for (int frame = 0; frame < 10; frame++){
        key(D_KEY_GRAVE_ACCENT);
        key(D_KEY_F3);
        Engine::systemTouchStart(0, 100, 100);
        Engine::systemTouchStart(1, 200, 100);
        Engine::systemTouchStart(2, 300, 100);
        GpuEnvironment::drawFrames(1);
        Engine::systemTouchEnd(0, 100, 100);
        Engine::systemTouchEnd(1, 200, 100);
        Engine::systemTouchEnd(2, 300, 100);
    }
    EXPECT_EQ(DebugOverlay::getScene(), nullptr);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
    EXPECT_EQ(DebugOverlay::getStatsMode(), DebugOverlaySettings::Stats::Off);
    // the game got every key
    EXPECT_EQ(gameKeys, 20);
}

// 8. the backquote key opens the console (the document from engine://), a second press closes it
TEST_F(DebugOverlayTest, BackquoteTogglesTheConsole){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    enable();
    GpuEnvironment::drawFrames(2);
    // nothing shown yet: no Scene
    EXPECT_EQ(DebugOverlay::getScene(), nullptr);

    key(D_KEY_GRAVE_ACCENT);
    EXPECT_TRUE(DebugOverlay::isConsoleVisible());
    GpuEnvironment::drawFrames(2);
    ASSERT_NE(document(), nullptr);
    EXPECT_EQ(document()->GetSourceURL().rfind("engine", 0), 0u) << document()->GetSourceURL();
    EXPECT_TRUE(shown("console"));
    // the game still got the key
    EXPECT_EQ(gameKeys, 1);

    key(D_KEY_GRAVE_ACCENT);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(shown("console"));

    // F3 cycles the stats
    key(D_KEY_F3);
    EXPECT_EQ(DebugOverlay::getStatsMode(), DebugOverlaySettings::Stats::Fps);
    GpuEnvironment::drawFrames(3);
    EXPECT_TRUE(shown("stats"));
    EXPECT_FALSE(shown("stats-table"));
    key(D_KEY_F3);
    GpuEnvironment::drawFrames(3);
    EXPECT_TRUE(shown("stats-table"));
    key(D_KEY_F3);
    EXPECT_EQ(DebugOverlay::getStatsMode(), DebugOverlaySettings::Stats::Off);

    // disabled at runtime: the Scene goes at the next frame
    DebugOverlay::setEnabled(false);
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(DebugOverlay::getScene(), nullptr);
}

// 9. three fingers held 0.75 s toggle the console; two do not
TEST_F(DebugOverlayTest, ThreeFingerHold){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    enable();
    Engine::setFixedDeltatime(0.1);

    Engine::systemTouchStart(0, 100, 600);
    Engine::systemTouchStart(1, 200, 600);
    GpuEnvironment::drawFrames(12);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
    Engine::systemTouchEnd(0, 100, 600);
    Engine::systemTouchEnd(1, 200, 600);

    Engine::systemTouchStart(0, 100, 600);
    Engine::systemTouchStart(1, 200, 600);
    Engine::systemTouchStart(2, 300, 600);
    GpuEnvironment::drawFrames(5);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
    GpuEnvironment::drawFrames(4);
    EXPECT_TRUE(DebugOverlay::isConsoleVisible());
    // once per hold
    GpuEnvironment::drawFrames(10);
    EXPECT_TRUE(DebugOverlay::isConsoleVisible());
    Engine::systemTouchEnd(0, 100, 600);
    Engine::systemTouchEnd(1, 200, 600);
    Engine::systemTouchEnd(2, 300, 600);

    // a hold that slides is no hold
    DebugOverlay::hideConsole();
    Engine::systemTouchStart(0, 100, 600);
    Engine::systemTouchStart(1, 200, 600);
    Engine::systemTouchStart(2, 300, 600);
    Engine::systemTouchMove(2, 600, 600);
    GpuEnvironment::drawFrames(10);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
    Engine::systemTouchEnd(0, 100, 600);
    Engine::systemTouchEnd(1, 200, 600);
    Engine::systemTouchEnd(2, 600, 600);
    // the game got every finger
    EXPECT_EQ(gameTouches, 8);
}

// 10. filters, search and collapse change the rows; a tap on a row shows its trace
TEST_F(DebugOverlayTest, FiltersSearchCollapseAndTraces){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    enable();
    logKnownSet();
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(2);
    ASSERT_NE(document(), nullptr);
    EXPECT_EQ(rows(), 5);

    click("filter-info");
    EXPECT_EQ(rows(), 3);
    click("filter-warn");
    EXPECT_EQ(rows(), 2);
    click("collapse");
    EXPECT_EQ(rows(), 1);
    Rml::Element* row = byId("list")->GetChild(0);
    EXPECT_NE(row->GetInnerRML().find("gamma error"), std::string::npos);
    EXPECT_NE(row->GetInnerRML().find(">2<"), std::string::npos) << row->GetInnerRML();
    click("collapse");
    click("filter-info");
    click("filter-warn");
    EXPECT_EQ(rows(), 5);

    search("INFO");
    EXPECT_EQ(rows(), 2);
    search("hit");    // in the trace only
    EXPECT_EQ(rows(), 2);
    search("");
    EXPECT_EQ(rows(), 5);

    // new lines are appended
    Log::print("epsilon");
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(rows(), 6);

    // a row expanded shows the trace
    Rml::Element* errorRow = byId("list")->GetChild(2);
    errorRow->Click();
    GpuEnvironment::drawFrames(1);
    errorRow = byId("list")->GetChild(2);
    EXPECT_TRUE(errorRow->IsClassSet("expanded"));
    EXPECT_NE(errorRow->GetInnerRML().find("game.lua:12"), std::string::npos) << errorRow->GetInnerRML();

    // the buffer bounds the rows; a clear empties them
    Log::setBufferSize(3);
    Log::print("zeta");
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(rows(), 3);
    click("clear");
    EXPECT_EQ(rows(), 0);
    EXPECT_TRUE(shown("empty"));
}

// 11. copy puts the rows on the clipboard; save writes data://logs/log-*.txt
TEST_F(DebugOverlayTest, CopyAndSave){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    enable();
    logKnownSet();
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(2);
    click("copy");
    const std::string copied = System::instance().getClipboardText();
    EXPECT_NE(copied.find("] INFO alpha info"), std::string::npos) << copied;
    EXPECT_NE(copied.find("] WARN beta warning"), std::string::npos) << copied;
    EXPECT_NE(copied.find("] ERROR gamma error\nstack traceback:\n\tgame.lua:12"), std::string::npos) << copied;

    // only the expanded row when one is
    byId("list")->GetChild(1)->Click();
    GpuEnvironment::drawFrames(1);
    click("copy");
    EXPECT_EQ(System::instance().getClipboardText().find("alpha"), std::string::npos);
    EXPECT_NE(System::instance().getClipboardText().find("beta warning"), std::string::npos);

    const std::filesystem::path logs = std::filesystem::path(System::instance().getUserDataPath()) / "logs";
    std::filesystem::remove_all(logs);
    click("save");
    std::string saved;
    for (const auto& entry : Log::getEntries()){
        if (entry.message.rfind("log saved to ", 0) == 0)
            saved = entry.message.substr(13);
    }
    ASSERT_FALSE(saved.empty());
    ASSERT_TRUE(std::filesystem::exists(saved)) << saved;
    EXPECT_EQ(std::filesystem::path(saved).parent_path().filename(), "logs");
    std::ifstream file(saved);
    std::stringstream text;
    text << file.rdbuf();
    EXPECT_NE(text.str().find("gamma error"), std::string::npos);
    std::filesystem::remove_all(logs);
}

// 13. the stats panel: FPS text and the full table
TEST_F(DebugOverlayTest, StatsPanel){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    DebugOverlaySettings settings;
    settings.stats = DebugOverlaySettings::Stats::Full;
    settings.statsCorner = DebugOverlaySettings::Corner::BottomRight;
    enable(settings);
    GpuEnvironment::drawFrames(10);
    ASSERT_NE(document(), nullptr);
    EXPECT_TRUE(shown("stats"));
    EXPECT_TRUE(byId("stats")->IsClassSet("corner-br"));
    EXPECT_NE(byId("stats-line")->GetInnerRML().find("FPS"), std::string::npos);
    EXPECT_NE(byId("stats-table")->GetInnerRML().find("Draw calls"), std::string::npos);
    EXPECT_GE(DebugStats::frameHistory().size(), 10u);
    // the overlay's own draws are not the game's
    const uint32_t drawCalls = Engine::getFrameStats().drawCalls;
    DebugOverlay::setStatsMode(DebugOverlaySettings::Stats::Off);
    GpuEnvironment::drawFrames(3);
    EXPECT_EQ(Engine::getFrameStats().drawCalls, drawCalls);
}

// 14. above the game: drawn last, offered a tap first; a tap outside its panels reaches the game
TEST_F(DebugOverlayTest, AboveTheGameAndOfferedInputFirst){
    page("game", "html, body { margin: 0; }\n#full { position: absolute; left: 0; top: 0; width: 1280px; height: 720px; background: #ff0000; }\n",
         "<div id=\"full\"></div>");
    fixture->addDocument("ui/game.html", 0);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    enable();
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(3);
    ASSERT_NE(document(), nullptr);

    // the console (top half, dark) over the game's red page; the red below it
    const std::array<int, 4> console = pixel(640, 300);
    const std::array<int, 4> below = pixel(640, 600);
    EXPECT_LT(console[0], 60) << console[0] << "," << console[1] << "," << console[2];
    EXPECT_GT(below[0], 200) << below[0];

    // a tap on the console's list is the overlay's; one below it is the game document's
    UIDocumentSystem& gameDocuments = *fixture->getScene().getSystem<UIDocumentSystem>();
    tap(640, 300);
    EXPECT_FALSE(gameDocuments.hasPointerCapture(0, D_MOUSE_BUTTON_1, true));
    EXPECT_EQ(gameTouches, 0);
    Engine::systemTouchStart(0, 640, 600);
    EXPECT_TRUE(gameDocuments.hasPointerCapture(0, D_MOUSE_BUTTON_1, true));
    Engine::systemTouchEnd(0, 640, 600);

    // closed: the whole screen is the game's again
    click("close");
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
    Engine::systemTouchStart(0, 640, 300);
    EXPECT_TRUE(gameDocuments.hasPointerCapture(0, D_MOUSE_BUTTON_1, true));
    Engine::systemTouchEnd(0, 640, 300);
}
