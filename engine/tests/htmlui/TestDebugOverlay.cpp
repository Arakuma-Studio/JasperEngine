// (c) JasperDev and contributors
// SPDX-License-Identifier: MIT

// The on-device debug overlay's document (feature debug-overlay, spec 10, tests 7-11, 13, 14):
// nothing exists while it is off; the backquote key and the three-finger hold toggle the
// console, loaded from engine:// with no project file; filters, search, collapse and row
// expansion; copy and save; the stats panel; the overlay drawn above the game and offered
// input first, consuming only taps on its own panels (also above a loading scene); the Canvas
// Scaler and the DPI scale; the row window and its throttle; the popup, the four-finger tap,
// Open On Error; the keys while the game types; gestures reset on focus loss, pause and
// cancel; the safe area. The log and commands are tested by
// editor/tests/LogListenerTest.cpp.

#include "InspectorTestCommon.h"

#include "Engine.h"
#include "Input.h"
#include "Log.h"
#include "log/Logger.h"
#include "debug/DebugOverlay.h"
#include "debug/DebugStats.h"
#include "harness/Readback.h"
#include "Screen.h"
#include "manager/SceneManager.h"
#include "subsystem/RenderSystem.h"
#include "util/CanvasScaler.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // The log is S09's Logger (docs/specs/S09-core-utilities.md "Upstream absorption"): a call
    // site past 200 lines a second meets the rate limit, so floods log distinct texts through
    // "%s" (keyed by text); and a flood past the Async sinks' queue would drop their lines and
    // log a drop notice the console lists, so the Async sinks sit out the floods (the console's
    // sink is Inline)
    void printDistinct(const char* prefix, int i){
        char text[96];
        std::snprintf(text, sizeof(text), "%s %d", prefix, i);
        Log::print("%s", text);
    }

    class QuietAsyncSinks {
    public:
        QuietAsyncSinks(){
            for (const Logger::SinkInfo& info : Logger::sinkInfo()){
                if (info.delivery != LogSink::Delivery::Async) continue;
                levels.push_back({info.id, info.level});
                Logger::setSinkLevel(info.id, LogLevel::Off);
            }
        }
        ~QuietAsyncSinks(){
            for (const auto& [id, level] : levels)
                Logger::setSinkLevel(id, level);
        }
    private:
        std::vector<std::pair<Logger::SinkId, LogLevel>> levels;
    };


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
            TestSystem::get().touchDevice = false;
            TestSystem::get().screenDpi = 0.0f;
            Engine::setDefaultCanvasScaler(CanvasScalerSettings());
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

        // the rows the filters let through (the list makes elements for those in view only)
        int rows(){
            Rml::Element* list = byId("list");
            if (!list)
                return -1;
            return list->GetAttribute<int>("data-rows", rows_unset);
        }
        static constexpr int rows_unset = -2;

        // the k-th row with an element (the window's first is the oldest in view); -1 = the last
        Rml::Element* shownRow(int k){
            Rml::Element* box = byId("list-rows");
            if (!box || box->GetNumChildren() == 0)
                return nullptr;
            return box->GetChild(k < 0 ? box->GetNumChildren() - 1 : k);
        }
        std::string shownRowText(int k){
            Rml::Element* row = shownRow(k);
            return row ? row->GetInnerRML() : std::string("(no row)");
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

        void fingers(int count, int phase, float y = 600.0f){
            for (int i = 0; i < count; i++){
                const float x = 100.0f + 100.0f * i;
                if (phase == 0) Engine::systemTouchStart(i, x, y);
                else Engine::systemTouchEnd(i, x, y);
            }
        }

        // an element's height on screen in canvas px (its layout px times the canvas px of one)
        float renderedHeight(const char* id){
            Rml::Element* element = byId(id);
            Scene* scene = DebugOverlay::getScene();
            if (!element || !scene)
                return -1.0f;
            const float canvasPerLayout = scene->isDocumentScaled() ? (float)Engine::getCanvasWidth() / scene->getCanvasScale().uiWidth : 1.0f;
            return element->GetOffsetHeight() * canvasPerLayout;
        }

        float bodyFontSize(){
            Rml::Element* body = byId("overlay-body");
            return body ? body->GetComputedValues().font_size() : -1.0f;
        }

        std::string text(const char* id){
            Rml::Element* element = byId(id);
            return element ? element->GetInnerRML() : std::string();
        }

        // the centre of an element, in view px (the canvas is the view here)
        void centre(const char* id, float& x, float& y){
            Rml::Element* element = byId(id);
            ASSERT_NE(element, nullptr) << id;
            const Rml::Vector2f at = element->GetAbsoluteOffset(Rml::BoxArea::Border);
            x = at.x + element->GetOffsetWidth() * 0.5f;
            y = at.y + element->GetOffsetHeight() * 0.5f;
        }

        void search(const std::string& text){
            auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(byId("search"));
            ASSERT_NE(control, nullptr);
            control->SetValue(text);
            control->DispatchEvent(Rml::EventId::Change, Rml::Dictionary());
            // applied once typing pauses (0.15 s)
            const double fixed = Engine::getFixedDeltatime();
            Engine::setFixedDeltatime(0.05);
            GpuEnvironment::drawFrames(5);
            Engine::setFixedDeltatime(fixed);
        }

        // the element's rect in layout px (left, top, right, bottom)
        std::array<float, 4> rect(const char* id){
            Rml::Element* element = byId(id);
            if (!element)
                return {0, 0, 0, 0};
            const Rml::Vector2f at = element->GetAbsoluteOffset(Rml::BoxArea::Border);
            return {at.x, at.y, at.x + element->GetOffsetWidth(), at.y + element->GetOffsetHeight()};
        }

        // the first row whose top is in the list's view
        std::string firstVisibleRowText(){
            Rml::Element* list = byId("list");
            Rml::Element* box = byId("list-rows");
            if (!list || !box)
                return "(no list)";
            const float top = list->GetAbsoluteOffset(Rml::BoxArea::Content).y;
            for (int i = 0; i < box->GetNumChildren(); i++){
                Rml::Element* row = box->GetChild(i);
                if (row->GetAbsoluteOffset(Rml::BoxArea::Border).y + 0.5f >= top)
                    return row->GetInnerRML();
            }
            return "(none in view)";
        }

        // after a clear: the lines the fixture's loads logged are not in the list
        void logKnownSet(){
            Log::clearEntries();
            Log::print("alpha info");
            Log::warn("beta warning");
            Log::error("gamma error\nstack traceback:\n\tgame.lua:12: in function 'hit'");
            Log::error("gamma error\nstack traceback:\n\tgame.lua:12: in function 'hit'");
            Log::print("delta info");
            // these errors are the test's own
            TestSystem::get().clearLogs();
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
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(2);
    logKnownSet();
    GpuEnvironment::drawFrames(2);
    ASSERT_NE(document(), nullptr);
    EXPECT_EQ(rows(), 5);

    click("filter-info");
    EXPECT_EQ(rows(), 3);
    click("filter-warn");
    EXPECT_EQ(rows(), 2);
    click("collapse");
    EXPECT_EQ(rows(), 1);
    Rml::Element* row = shownRow(0);
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
    Rml::Element* errorRow = shownRow(2);
    errorRow->Click();
    GpuEnvironment::drawFrames(1);
    errorRow = shownRow(2);
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
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(2);
    logKnownSet();
    GpuEnvironment::drawFrames(2);
    click("copy");
    const std::string copied = System::instance().getClipboardText();
    EXPECT_NE(copied.find("] INFO alpha info"), std::string::npos) << copied;
    EXPECT_NE(copied.find("] WARN beta warning"), std::string::npos) << copied;
    EXPECT_NE(copied.find("] ERROR gamma error\nstack traceback:\n\tgame.lua:12"), std::string::npos) << copied;

    // only the expanded row when one is
    shownRow(1)->Click();
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
    GpuEnvironment::drawFrames(2);
    const std::array<int, 4> game = pixel(640, 600);
    ASSERT_GT(game[0], 200) << "the game's page: " << game[0] << "," << game[1] << "," << game[2];
    enable();
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(3);
    ASSERT_NE(document(), nullptr);

    // the console (top half, #0b0d12ee) over the game's red page; the red below it
    const std::array<int, 4> console = pixel(640, 300);
    const std::array<int, 4> below = pixel(640, 600);
    EXPECT_NEAR(console[0], 28, 6) << console[0] << "," << console[1] << "," << console[2];
    EXPECT_NEAR(console[1], 12, 6);
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

// the project's Canvas Scaler scales the game's UI, not the overlay: its own FollowGameCanvas
TEST_F(DebugOverlayTest, CanvasScalerDoesNotScaleTheOverlay){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    DebugOverlaySettings settings;
    settings.stats = DebugOverlaySettings::Stats::Fps;
    enable(settings);
    GpuEnvironment::drawFrames(3);
    ASSERT_NE(byId("stats-line"), nullptr);
    const float height = renderedHeight("stats-line");
    const float fontSize = bodyFontSize();
    ASSERT_GT(height, 10.0f);
    DebugOverlay::shutdown();

    // the game being ported: 1920x1080, matching the height
    CanvasScalerSettings scaler;
    scaler.mode = UIScaleMode::ScaleWithScreenSize;
    scaler.referenceResolution = Vector2(1920, 1080);
    scaler.matchWidthOrHeight = 1.0f;
    Engine::setDefaultCanvasScaler(scaler);
    GpuEnvironment::drawFrames(1);
    EXPECT_TRUE(fixture->getScene().isDocumentScaled()) << "the game's documents follow the scaler";
    enable(settings);
    GpuEnvironment::drawFrames(3);
    ASSERT_NE(DebugOverlay::getScene(), nullptr);
    EXPECT_FALSE(DebugOverlay::getScene()->isDocumentScaled());
    EXPECT_NEAR(renderedHeight("stats-line"), height, 0.5f);
    EXPECT_NEAR(bodyFontSize(), fontSize, 0.01f);

    // changed while it shows
    scaler.referenceResolution = Vector2(640, 360);
    Engine::setDefaultCanvasScaler(scaler);
    GpuEnvironment::drawFrames(3);
    EXPECT_FALSE(DebugOverlay::getScene()->isDocumentScaled());
    EXPECT_NEAR(renderedHeight("stats-line"), height, 0.5f);
}

// touch screens: 1/160 inch from the DPI, at most the shorter side / 400 (also without a DPI)
TEST_F(DebugOverlayTest, TouchScaleFollowsTheDpi){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    DebugOverlaySettings settings;
    settings.stats = DebugOverlaySettings::Stats::Fps;
    enable(settings);
    GpuEnvironment::drawFrames(3);
    const Rect view = Engine::getViewRect();
    ASSERT_GT(view.getWidth(), 0.0f);
    const float canvasPerView = (float)Engine::getCanvasWidth() / view.getWidth();
    const float shorter = std::min(view.getWidth(), view.getHeight());
    EXPECT_NEAR(bodyFontSize(), 13.0f * std::max(1.0f, shorter / 720.0f) * canvasPerView, 0.01f) << "desktop";

    TestSystem::get().touchDevice = true;
    TestSystem::get().screenDpi = 264.0f;   // an iPad
    GpuEnvironment::drawFrames(2);
    EXPECT_NEAR(bodyFontSize(), 13.0f * std::max(1.0f, std::min(264.0f / 160.0f, shorter / 400.0f)) * canvasPerView, 0.01f) << "by the DPI";
    TestSystem::get().screenDpi = 640.0f;   // a dense phone: capped
    GpuEnvironment::drawFrames(2);
    EXPECT_NEAR(bodyFontSize(), 13.0f * std::max(1.0f, shorter / 400.0f) * canvasPerView, 0.01f) << "capped";
    TestSystem::get().screenDpi = 0.0f;     // unknown
    GpuEnvironment::drawFrames(2);
    EXPECT_NEAR(bodyFontSize(), 13.0f * std::max(1.0f, shorter / 400.0f) * canvasPerView, 0.01f) << "no DPI";
}

// every row scrolls, only those in view have elements (a few dozen whatever the buffer); a
// burst of lines updates the list at 10 Hz; a rebuild is quick; collapsed counts drop as lines
// leave the buffer
TEST_F(DebugOverlayTest, RowWindowThrottleAndTheBuffer){
    QuietAsyncSinks quiet;
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Engine::setFixedDeltatime(0.05);
    Log::setBufferSize(20000);
    enable();
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(2);
    Log::clearEntries();
    for (int i = 0; i < 12000; i++)
        printDistinct("line", i);
    GpuEnvironment::drawFrames(4);
    ASSERT_EQ(rows(), 12000);
    Rml::Element* box = byId("list-rows");
    ASSERT_NE(box, nullptr);
    EXPECT_GT(box->GetNumChildren(), 5);
    EXPECT_LT(box->GetNumChildren(), 60) << "the rows in view and a margin";
    EXPECT_NE(shownRowText(-1).find("line 11999"), std::string::npos) << "follows the newest";
    Rml::Element* list = byId("list");
    EXPECT_GT(list->GetScrollHeight(), 12000.0f * 15.0f) << "every row scrolls";

    // scrolled to the top: the oldest rows get the elements
    list->SetScrollTop(0.0f);
    GpuEnvironment::drawFrames(2);
    EXPECT_NE(shownRowText(0).find("line 0"), std::string::npos) << shownRowText(0);
    // in the middle
    list->SetScrollTop(list->GetScrollHeight() / 2.0f);
    GpuEnvironment::drawFrames(2);
    const std::string middle = shownRowText(10);
    const size_t at = middle.find("line ");
    ASSERT_NE(at, std::string::npos) << middle;
    const int number = std::atoi(middle.c_str() + at + 5);
    EXPECT_GT(number, 5000) << middle;
    EXPECT_LT(number, 7000) << middle;
    // a new line does not pull the list down while reading
    Log::print("while reading");
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(shownRowText(10), middle);
    // back at the bottom it follows again
    list->SetScrollTop(list->GetScrollHeight());
    GpuEnvironment::drawFrames(3);
    EXPECT_NE(shownRowText(-1).find("while reading"), std::string::npos) << shownRowText(-1);

    // a burst: the list takes it now, the next one 0.1 s later
    for (int i = 0; i < 50; i++)
        Log::print("burst a %d", i);
    GpuEnvironment::drawFrames(1);
    EXPECT_NE(shownRowText(-1).find("burst a 49"), std::string::npos) << shownRowText(-1);
    for (int i = 0; i < 50; i++)
        Log::print("burst b %d", i);
    GpuEnvironment::drawFrames(1);
    EXPECT_NE(shownRowText(-1).find("burst a 49"), std::string::npos) << "throttled";
    GpuEnvironment::drawFrames(3);
    EXPECT_NE(shownRowText(-1).find("burst b 49"), std::string::npos);
    // a few lines come at once
    Log::print("single");
    GpuEnvironment::drawFrames(1);
    EXPECT_NE(shownRowText(-1).find("single"), std::string::npos);

    // collapse of 12102 rows
    click("collapse");
    EXPECT_EQ(rows(), 12102);
    click("collapse");

    // the spec's budget: a full rebuild of 500 rows < 5 ms (here with its layout), a line added too
    Log::clearEntries();
    Log::setBufferSize(500);
    for (int i = 0; i < 500; i++)
        printDistinct("row", i);
    GpuEnvironment::drawFrames(3);
    ASSERT_EQ(rows(), 500);
    byId("filter-warn")->Click();
    auto start = std::chrono::steady_clock::now();
    DebugOverlay::update(0.0);
    const double rebuildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(rows(), 500);
    GpuEnvironment::drawFrames(1);
    Log::print("one more");
    start = std::chrono::steady_clock::now();
    DebugOverlay::update(0.0);
    const double addMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    printf("[debug overlay] rebuild of 500 rows with its layout: %.2f ms, a line added: %.2f ms\n", rebuildMs, addMs);
    EXPECT_LT(rebuildMs, 5.0);
    EXPECT_LT(addMs, 5.0);
    click("filter-warn");
    click("collapse");

    // collapsed: a row's count is its buffered lines
    Log::clearEntries();
    Log::setBufferSize(10);
    for (int i = 0; i < 6; i++)
        Log::warn("same warning");
    for (int i = 0; i < 4; i++)
        Log::print("other %d", i);
    GpuEnvironment::drawFrames(1);
    ASSERT_EQ(rows(), 5);
    EXPECT_NE(shownRowText(0).find(">6<"), std::string::npos) << shownRowText(0);
    for (int i = 0; i < 3; i++)
        Log::print("new %d", i);
    GpuEnvironment::drawFrames(1);
    ASSERT_EQ(rows(), 8);
    EXPECT_NE(shownRowText(0).find(">3<"), std::string::npos) << shownRowText(0);
    for (int i = 0; i < 3; i++)
        Log::print("more %d", i);
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(rows(), 10);
    EXPECT_EQ(shownRowText(0).find("same warning"), std::string::npos) << "gone with its last line";

    // an expanded row is measured: the rows after it keep their place
    shownRow(0)->Click();
    GpuEnvironment::drawFrames(3);
    EXPECT_TRUE(shownRow(0)->IsClassSet("expanded"));
    EXPECT_EQ(byId("list-rows")->GetNumChildren(), 10);
}

// the popup: the counts since the console was last open, a tap opens it, a drag moves it (also
// one that ends off it does not eat the next tap)
TEST_F(DebugOverlayTest, PopupCountsTapAndDrag){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    DebugOverlaySettings settings;
    settings.popup = DebugOverlaySettings::Popup::AllDevices;
    enable(settings);
    GpuEnvironment::drawFrames(2);
    ASSERT_TRUE(shown("popup"));
    Log::warn("w1");
    Log::error("e1");
    Log::error("e2");
    TestSystem::get().clearLogs();
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(text("popup-warn"), "1");
    EXPECT_EQ(text("popup-error"), "2");

    // dragged down past its lowest place: the pointer ends off it, no click
    float x = 0.0f, y = 0.0f;
    centre("popup", x, y);
    const float top = byId("popup")->GetAbsoluteOffset(Rml::BoxArea::Border).y;
    Engine::systemMouseMove(x, y, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_1, x, y, 0);
    GpuEnvironment::drawFrames(1);
    for (float at = y + 20.0f; at < 719.0f; at += 20.0f){
        Engine::systemMouseMove(x, at, 0);
        GpuEnvironment::drawFrames(1);
    }
    Engine::systemMouseMove(x, 719.0f, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_1, x, 719.0f, 0);
    GpuEnvironment::drawFrames(2);
    EXPECT_GT(byId("popup")->GetAbsoluteOffset(Rml::BoxArea::Border).y, top + 100.0f) << "moved";
    EXPECT_FALSE(DebugOverlay::isConsoleVisible()) << "a drag is no tap";

    // the next tap opens the console
    centre("popup", x, y);
    Engine::systemMouseMove(x, y, 0);
    Engine::systemMouseDown(D_MOUSE_BUTTON_1, x, y, 0);
    Engine::systemMouseUp(D_MOUSE_BUTTON_1, x, y, 0);
    GpuEnvironment::drawFrames(2);
    EXPECT_TRUE(DebugOverlay::isConsoleVisible());
    EXPECT_FALSE(shown("popup")) << "the console replaces it";

    // closed: the counts start again from there
    DebugOverlay::hideConsole();
    Log::warn("w2");
    GpuEnvironment::drawFrames(2);
    EXPECT_EQ(text("popup-warn"), "1");
    EXPECT_EQ(text("popup-error"), "0");
}

// four fingers down and up within 0.5 s toggle the console; slower, or three, do not
TEST_F(DebugOverlayTest, FourFingerTap){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    DebugOverlaySettings settings;
    settings.gesture = DebugOverlaySettings::Gesture::FourFingerTap;
    enable(settings);
    Engine::setFixedDeltatime(0.1);
    GpuEnvironment::drawFrames(1);

    fingers(4, 0);
    GpuEnvironment::drawFrames(1);
    fingers(4, 2);
    EXPECT_TRUE(DebugOverlay::isConsoleVisible());
    GpuEnvironment::drawFrames(1);
    fingers(4, 0);
    GpuEnvironment::drawFrames(1);
    fingers(4, 2);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());

    // held too long
    fingers(4, 0);
    GpuEnvironment::drawFrames(7);
    fingers(4, 2);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
    // three fingers: neither a tap nor (with this gesture) a hold
    fingers(3, 0);
    GpuEnvironment::drawFrames(10);
    fingers(3, 2);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
}

// Open On Error: the first error after the overlay is on opens the console, once
TEST_F(DebugOverlayTest, OpenOnError){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Log::error("before the overlay");
    TestSystem::get().clearLogs();
    DebugOverlaySettings settings;
    settings.openOnError = true;
    enable(settings);
    GpuEnvironment::drawFrames(2);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible()) << "an error from before";
    Log::warn("a warning");
    GpuEnvironment::drawFrames(1);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
    Log::error("the first error");
    TestSystem::get().clearLogs();
    GpuEnvironment::drawFrames(2);
    EXPECT_TRUE(DebugOverlay::isConsoleVisible());
    EXPECT_TRUE(shown("console"));

    DebugOverlay::hideConsole();
    Log::error("another error");
    TestSystem::get().clearLogs();
    GpuEnvironment::drawFrames(2);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible()) << "once";
}

// the toggle and stats keys while a text field of the game has the keyboard: typed, not toggled
TEST_F(DebugOverlayTest, KeysIgnoredWhileTheGameTypes){
    page("form", "", "<input type=\"text\" id=\"name\"/>");
    UIDocument form = fixture->addDocument("ui/form.html", 0);
    ASSERT_TRUE(fixture->waitUntilLoaded());
    enable();
    GpuEnvironment::drawFrames(1);
    Rml::ElementDocument* game = fixture->getScene().getSystem<UIDocumentSystem>()->getDocument(form.getEntity());
    ASSERT_NE(game, nullptr);
    Rml::Element* field = game->GetElementById("name");
    ASSERT_NE(field, nullptr);
    field->Focus();
    ASSERT_TRUE(Engine::isTextInputFocused());
    key(D_KEY_GRAVE_ACCENT);
    key(D_KEY_F3);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
    EXPECT_EQ(DebugOverlay::getStatsMode(), DebugOverlaySettings::Stats::Off);

    field->Blur();
    key(D_KEY_GRAVE_ACCENT);
    EXPECT_TRUE(DebugOverlay::isConsoleVisible());
    GpuEnvironment::drawFrames(2);
    // the console's own command field: the key still closes it
    ASSERT_NE(byId("command"), nullptr);
    byId("command")->Focus();
    ASSERT_TRUE(DebugOverlay::hasTextFocus());
    key(D_KEY_GRAVE_ACCENT);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
}

// no finger stays held for the gestures across a focus loss, a pause or a cancel of all touches
// (also fingers outside the canvas, which Input never had)
TEST_F(DebugOverlayTest, GesturesResetOnFocusLossPauseAndCancel){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    enable();
    Engine::setFixedDeltatime(0.1);
    GpuEnvironment::drawFrames(1);

    fingers(3, 0);
    GpuEnvironment::drawFrames(3);
    Engine::systemFocus(false);
    GpuEnvironment::drawFrames(10);
    Engine::systemFocus(true);
    GpuEnvironment::drawFrames(5);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible()) << "focus loss";
    fingers(3, 2);

    fingers(3, 0);
    GpuEnvironment::drawFrames(3);
    Engine::systemPause();
    Engine::systemResume();
    Engine::systemFocus(true);
    GpuEnvironment::drawFrames(10);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible()) << "pause";
    fingers(3, 2);

    // outside the canvas: the overlay saw them, Input did not
    DebugOverlay::handleTouch(7, -50.0f, 100.0f, 0);
    DebugOverlay::handleTouch(8, -50.0f, 200.0f, 0);
    DebugOverlay::handleTouch(9, -50.0f, 300.0f, 0);
    GpuEnvironment::drawFrames(3);
    Engine::systemTouchCancel();
    GpuEnvironment::drawFrames(10);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible()) << "cancel all";
    // one finger cancelled: the hold needs three again
    DebugOverlay::handleTouch(7, -50.0f, 100.0f, 0);
    DebugOverlay::handleTouch(8, -50.0f, 200.0f, 0);
    DebugOverlay::handleTouch(9, -50.0f, 300.0f, 0);
    Engine::systemTouchCancel(9);
    GpuEnvironment::drawFrames(10);
    EXPECT_FALSE(DebugOverlay::isConsoleVisible()) << "one cancelled";
    DebugOverlay::cancelGestures();

    // and a hold still works after all that
    fingers(3, 0);
    GpuEnvironment::drawFrames(9);
    EXPECT_TRUE(DebugOverlay::isConsoleVisible());
    fingers(3, 2);
}

// 14 with a loading scene in the stack: the overlay is drawn above it and offered a tap first;
// a tap outside its panels reaches the loading scene's document (the game below it is covered)
TEST_F(DebugOverlayTest, AboveTheLoadingScene){
    page("game", "html, body { margin: 0; }\n#full { position: absolute; left: 0; top: 0; width: 1280px; height: 720px; background: #ff0000; }\n",
         "<div id=\"full\"></div>");
    page("loading", "html, body { margin: 0; }\n#full { position: absolute; left: 0; top: 0; width: 1280px; height: 720px; background: #0000ff; }\n",
         "<div id=\"full\"></div>");
    fixture->addDocument("ui/game.html", 0);
    ASSERT_TRUE(fixture->waitUntilLoaded());

    std::unique_ptr<Scene> loading(new Scene());
    std::unique_ptr<Camera> loadingCamera(new Camera(loading.get()));
    loadingCamera->setType(CameraType::CAMERA_UI);
    loadingCamera->activate();
    std::unique_ptr<UIDocument> loadingDocument(new UIDocument(loading.get()));
    loadingDocument->setFilename("ui/loading.html");

    Scene* game = &fixture->getScene();
    SceneManager::registerScene(1, "Game", [game](){ Engine::setScene(game); });
    SceneManager::registerScene(2, "Loading", [](){}, [&loading](){ SceneManager::setScenePtr(2, loading.get()); }, {2});
    struct Cleanup {
        Scene* game;
        Scene* loading;
        ~Cleanup(){
            SceneManager::releaseLoading();
            SceneManager::clearAll();
            Engine::removeScene(loading);
            Engine::setScene(game);
        }
    } cleanup{game, loading.get()};
    ASSERT_TRUE(SceneManager::setLoadingScene("Loading"));
    ASSERT_TRUE(SceneManager::loadScene("Game"));
    SceneManager::holdLoading();
    ASSERT_TRUE(GpuEnvironment::runFrames([&loading, game](){
        return SceneManager::isCoveredByLoading(game) && loading->getSystem<RenderSystem>()->isAllLoaded() &&
               loading->getSystem<UIDocumentSystem>()->isAllLoaded();
    }, 300));
    GpuEnvironment::drawFrames(3);
    ASSERT_TRUE(SceneManager::isLoading());
    ASSERT_TRUE(SceneManager::isCoveredByLoading(game)) << "the loading scene is up";
    const std::array<int, 4> covered = pixel(640, 600);
    ASSERT_GT(covered[2], 200) << "the loading page: " << covered[0] << "," << covered[1] << "," << covered[2];

    enable();
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(3);
    ASSERT_NE(document(), nullptr);
    const std::vector<Scene*> layers = Engine::getSceneLayers();
    ASSERT_FALSE(layers.empty());
    EXPECT_EQ(layers.back(), loading.get()) << "the loading scene is the top layer of the game";
    const std::array<int, 4> console = pixel(640, 300);
    const std::array<int, 4> below = pixel(640, 600);
    // the console's #0b0d12ee over the blue page
    EXPECT_LT(console[0], 30) << console[0] << "," << console[1] << "," << console[2];
    EXPECT_LT(console[2], 60) << "drawn over the loading page";
    EXPECT_GT(below[2], 200) << below[2];

    UIDocumentSystem& loadingDocuments = *loading->getSystem<UIDocumentSystem>();
    UIDocumentSystem& gameDocuments = *game->getSystem<UIDocumentSystem>();
    tap(640, 300);
    EXPECT_FALSE(loadingDocuments.hasPointerCapture(0, D_MOUSE_BUTTON_1, true)) << "the overlay's tap";
    Engine::systemTouchStart(0, 640, 600);
    EXPECT_TRUE(loadingDocuments.hasPointerCapture(0, D_MOUSE_BUTTON_1, true)) << "below the console: the loading scene's";
    EXPECT_FALSE(gameDocuments.hasPointerCapture(0, D_MOUSE_BUTTON_1, true)) << "the covered game gets nothing";
    Engine::systemTouchEnd(0, 640, 600);

    // the overlay's buttons work over a loading scene
    click("close");
    EXPECT_FALSE(DebugOverlay::isConsoleVisible());
}

// a notch (the platform's insets, as a phone in landscape and the Game view's device simulation
// give them): the console and the stats panel keep clear of the safe area
TEST_F(DebugOverlayTest, PanelsKeepClearOfTheSafeArea){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    struct Restore { ~Restore(){ Screen::setPlatformSafeArea(EdgeInsets()); } } restore;
    DebugOverlaySettings settings;
    settings.stats = DebugOverlaySettings::Stats::Fps;
    enable(settings);
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(3);
    ASSERT_NE(document(), nullptr);
    const float consoleLeft = byId("console")->GetAbsoluteOffset(Rml::BoxArea::Border).x;
    const float consoleTop = byId("console")->GetAbsoluteOffset(Rml::BoxArea::Border).y;
    EXPECT_NEAR(consoleLeft, 0.0f, 0.5f);
    EXPECT_NEAR(consoleTop, 0.0f, 0.5f);

    // view pixels (the view is the canvas here): a notch at the left, the status bar's corners at
    // the top, the home indicator at the bottom
    const Rect view = Engine::getViewRect();
    const float canvasPerView = (float)Engine::getCanvasWidth() / view.getWidth();
    Screen::setPlatformSafeArea(EdgeInsets(88.0f, 24.0f, 40.0f, 21.0f));
    GpuEnvironment::drawFrames(3);
    const EdgeInsets insets = Screen::getSafeAreaInsets();
    EXPECT_NEAR(insets.left, 88.0f * canvasPerView, 0.5f);
    Rml::Element* console = byId("console");
    EXPECT_NEAR(console->GetAbsoluteOffset(Rml::BoxArea::Border).x, insets.left, 0.5f);
    EXPECT_NEAR(console->GetAbsoluteOffset(Rml::BoxArea::Border).y, insets.top, 0.5f);
    EXPECT_NEAR(console->GetAbsoluteOffset(Rml::BoxArea::Border).x + console->GetOffsetWidth(),
                (float)Engine::getCanvasWidth() - insets.right, 0.5f);
    // full height: down to the bottom inset
    click("size");
    GpuEnvironment::drawFrames(2);
    EXPECT_NEAR(console->GetAbsoluteOffset(Rml::BoxArea::Border).y + console->GetOffsetHeight(),
                (float)Engine::getCanvasHeight() - insets.bottom, 0.5f);

    // the stats panel (top-left) too, under the console's top when both show
    DebugOverlay::hideConsole();
    GpuEnvironment::drawFrames(2);
    Rml::Element* stats = byId("stats");
    EXPECT_GE(stats->GetAbsoluteOffset(Rml::BoxArea::Border).x, insets.left);
    EXPECT_GE(stats->GetAbsoluteOffset(Rml::BoxArea::Border).y, insets.top);
    // the pixels in the notch stay the game's
    const std::array<int, 4> notch = pixel(20, 10);
    EXPECT_NEAR(notch[0], 0x1E, 3) << notch[0] << "," << notch[1] << "," << notch[2];

    // gone: the panels back at the edges
    Screen::setPlatformSafeArea(EdgeInsets());
    GpuEnvironment::drawFrames(3);
    EXPECT_NEAR(stats->GetAbsoluteOffset(Rml::BoxArea::Border).x, 4.0f, 1.0f);
}

// the stats panel in a top corner goes under the open console, in portrait and in landscape
// (`top: 50%` is a share of the height), and stays on the screen
TEST_F(DebugOverlayTest, StatsBelowTheConsoleInPortraitAndLandscape){
    struct View { int width, height; };
    for (const View view : {View{720, 1280}, View{2400, 1080}}){
        SCOPED_TRACE(std::to_string(view.width) + "x" + std::to_string(view.height));
        GpuEnvironment::setView(view.width, view.height, view.width, view.height, Scaling::FITWIDTH);
        fixture.reset(new SceneFixture());
        ASSERT_TRUE(fixture->waitUntilLoaded());
        for (DebugOverlaySettings::Corner corner : {DebugOverlaySettings::Corner::TopLeft, DebugOverlaySettings::Corner::TopRight}){
            DebugOverlaySettings settings;
            settings.stats = DebugOverlaySettings::Stats::Full;
            settings.statsCorner = corner;
            enable(settings);
            DebugOverlay::showConsole();
            GpuEnvironment::drawFrames(4);
            ASSERT_NE(document(), nullptr);
            const std::array<float, 4> console = rect("console");
            const std::array<float, 4> stats = rect("stats");
            EXPECT_GT(stats[3] - stats[1], 20.0f);
            EXPECT_GE(stats[1], console[3]) << "below the console: stats top " << stats[1] << ", console bottom " << console[3];
            EXPECT_GE(stats[0], 0.0f);
            EXPECT_LE(stats[2], (float)Engine::getCanvasWidth());
            EXPECT_LE(stats[3], (float)Engine::getCanvasHeight()) << "on the screen";
            // closed: back at the top
            DebugOverlay::hideConsole();
            GpuEnvironment::drawFrames(2);
            EXPECT_LT(rect("stats")[1], 20.0f);
            DebugOverlay::shutdown();
        }
        fixture.reset();
    }
    GpuEnvironment::setView(1280, 720, 1280, 720, Scaling::FITWIDTH);
    fixture.reset(new SceneFixture());
}

// reading older lines while the buffer is full and a line comes every frame: the rows in view
// stay (the trimmed rows above them move the scroll up by as much)
TEST_F(DebugOverlayTest, ListStaysWhileReadingAFullBuffer){
    QuietAsyncSinks quiet;
    ASSERT_TRUE(fixture->waitUntilLoaded());
    // the overlay's clock: a burst of lines waits 0.1 s of it
    Engine::setFixedDeltatime(0.05);
    Log::setBufferSize(500);
    enable();
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(2);
    Log::clearEntries();
    for (int i = 0; i < 500; i++)
        printDistinct("line", i);
    GpuEnvironment::drawFrames(4);
    ASSERT_EQ(rows(), 500);
    Rml::Element* list = byId("list");
    ASSERT_NE(list, nullptr);
    list->SetScrollTop(list->GetScrollHeight() / 2.0f);
    GpuEnvironment::drawFrames(2);
    const std::string reading = firstVisibleRowText();
    ASSERT_NE(reading.find("line 2"), std::string::npos) << reading;
    for (int frame = 0; frame < 60; frame++){
        Log::print("new %d", frame);
        GpuEnvironment::drawFrames(1);
    }
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(firstVisibleRowText(), reading) << "the row read stays in view";
    EXPECT_EQ(rows(), 500);

    // an expanded row above the view does not move it either
    list->SetScrollTop(0.0f);
    GpuEnvironment::drawFrames(2);
    ASSERT_NE(shownRow(0), nullptr);
    shownRow(0)->Click();
    GpuEnvironment::drawFrames(1);
    list->SetScrollTop(list->GetScrollHeight() / 2.0f);
    GpuEnvironment::drawFrames(3);
    const std::string middle = firstVisibleRowText();
    for (int frame = 0; frame < 10; frame++){
        Log::print("more %d", frame);
        GpuEnvironment::drawFrames(1);
    }
    GpuEnvironment::drawFrames(1);
    EXPECT_EQ(firstVisibleRowText(), middle);
}

// collapse on, a full 100,000-line buffer of distinct lines: a line added costs little (the
// rows that go are found at the front, no index rebuilt)
TEST_F(DebugOverlayTest, CollapsedFullBufferLineCost){
    QuietAsyncSinks quiet;
    ASSERT_TRUE(fixture->waitUntilLoaded());
    Engine::setFixedDeltatime(0.05);
    Log::setBufferSize(100000);
    enable();
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(2);
    click("collapse");
    Log::clearEntries();
    for (int i = 0; i < 100000; i++)
        printDistinct("distinct", i);
    GpuEnvironment::drawFrames(4);
    ASSERT_EQ(rows(), 100000);
    double worst = 0.0, total = 0.0;
    const int lines = 50;
    for (int i = 0; i < lines; i++){
        Log::print("more %d", i);
        const auto start = std::chrono::steady_clock::now();
        DebugOverlay::update(0.0);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        worst = std::max(worst, ms);
        total += ms;
    }
    printf("[debug overlay] collapsed, 100,000 rows: a line %.3f ms (worst %.3f ms) with its layout\n", total / lines, worst);
    EXPECT_LT(total / lines, 0.5);
    EXPECT_EQ(rows(), 100000);
    EXPECT_NE(shownRowText(-1).find("more 49"), std::string::npos) << shownRowText(-1);
}

// the safe area with the game's UI on ScaleWithScreenSize: the overlay (its own scaler) still
// keeps clear of the insets, in its own px
TEST_F(DebugOverlayTest, SafeAreaWithTheGameScaled){
    ASSERT_TRUE(fixture->waitUntilLoaded());
    struct Restore { ~Restore(){ Screen::setPlatformSafeArea(EdgeInsets()); } } restore;
    CanvasScalerSettings scaler;
    scaler.mode = UIScaleMode::ScaleWithScreenSize;
    scaler.referenceResolution = Vector2(1920, 1080);
    scaler.matchWidthOrHeight = 1.0f;
    Engine::setDefaultCanvasScaler(scaler);
    Screen::setPlatformSafeArea(EdgeInsets(88.0f, 24.0f, 40.0f, 21.0f));
    DebugOverlaySettings settings;
    settings.stats = DebugOverlaySettings::Stats::Fps;
    enable(settings);
    DebugOverlay::showConsole();
    GpuEnvironment::drawFrames(3);
    ASSERT_NE(document(), nullptr);
    ASSERT_TRUE(fixture->getScene().isDocumentScaled());
    ASSERT_FALSE(DebugOverlay::getScene()->isDocumentScaled());
    const EdgeInsets insets = Screen::getSafeAreaInsets();
    const std::array<float, 4> console = rect("console");
    EXPECT_NEAR(console[0], insets.left, 0.5f);
    EXPECT_NEAR(console[1], insets.top, 0.5f);
    EXPECT_NEAR(console[2], (float)Engine::getCanvasWidth() - insets.right, 0.5f);
    const std::array<float, 4> stats = rect("stats");
    EXPECT_GE(stats[0], insets.left);
    EXPECT_GE(stats[1], console[3]);
}
