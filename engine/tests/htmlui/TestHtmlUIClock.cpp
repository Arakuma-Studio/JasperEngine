// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// HtmlUIClock (phase 1 wave 0): advance, fixed time and its release, RmlSystem's elapsed
// time, Engine::systemDraw advancing it with the unscaled (fixed) delta whatever the time
// scale, not while the simulation is held, and RmlUi animations following it.

#include "TestDocumentCommon.h"

#include "Engine.h"
#include "HtmlUIClock.h"
#include "RmlSystem.h"
#include "component/UIDocumentComponent.h"
#include "htmlui/anim/Anime.h"
#include "object/ui/UIDocument.h"
#include "subsystem/UIDocumentSystem.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::test;

namespace {

    // the engine's frame timing, restored after each test
    class HtmlUIClockTest : public ::testing::Test {
    protected:
        void SetUp() override{
            TestSystem::get().clearLogs();
            HtmlUIClock::reset();
        }

        void TearDown() override{
            Engine::setTimeScale(1.0f);
            Engine::setFixedDeltatime(0.0);
            Engine::setSimulationHeld(false);
            HtmlUIClock::reset();
        }
    };

    Rml::Element* documentElement(Scene& scene, const UIDocument& document, const char* id){
        Rml::ElementDocument* rml = documentSystem(scene).getDocument(document.getEntity());
        return rml ? rml->GetElementById(id) : nullptr;
    }

    float opacityOf(Scene& scene, const char* id){
        Rml::Context* context = rmlContext(scene);
        if (!context || context->GetNumDocuments() == 0)
            return -1.0f;
        Rml::Element* element = context->GetDocument(0)->GetElementById(id);
        return element ? element->GetComputedValues().opacity() : -1.0f;
    }

}

TEST_F(HtmlUIClockTest, AdvanceAccumulates){
    EXPECT_EQ(HtmlUIClock::now(), 0.0);
    EXPECT_FALSE(HtmlUIClock::getFixedTime().has_value());

    HtmlUIClock::advance(0.25);
    HtmlUIClock::advance(0.5);
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(), 0.75);

    // a frame without time, or a broken delta, leaves it
    HtmlUIClock::advance(0.0);
    HtmlUIClock::advance(-1.0);
    HtmlUIClock::advance(std::numeric_limits<double>::quiet_NaN());
    HtmlUIClock::advance(std::numeric_limits<double>::infinity());
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(), 0.75);

    HtmlUIClock::reset();
    EXPECT_EQ(HtmlUIClock::now(), 0.0);
}

TEST_F(HtmlUIClockTest, FixedTimePinsAndReleasesWithoutJump){
    HtmlUIClock::advance(1.0);

    HtmlUIClock::setFixedTime(5.0);
    ASSERT_TRUE(HtmlUIClock::getFixedTime().has_value());
    EXPECT_DOUBLE_EQ(*HtmlUIClock::getFixedTime(), 5.0);
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(), 5.0);

    // frames do not move a pinned clock
    HtmlUIClock::advance(0.5);
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(), 5.0);

    // a pinned clock can go anywhere, backwards too (golden frames)
    HtmlUIClock::setFixedTime(0.5);
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(), 0.5);

    // released: running again from the pinned value
    HtmlUIClock::setFixedTime(std::nullopt);
    EXPECT_FALSE(HtmlUIClock::getFixedTime().has_value());
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(), 0.5);
    HtmlUIClock::advance(0.25);
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(), 0.75);

    // releasing a clock that is not pinned changes nothing
    HtmlUIClock::setFixedTime(std::nullopt);
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(), 0.75);

    HtmlUIClock::setFixedTime(2.0);
    HtmlUIClock::reset();
    EXPECT_FALSE(HtmlUIClock::getFixedTime().has_value());
    EXPECT_EQ(HtmlUIClock::now(), 0.0);
}

// The recorder and render_ui_frames pin the Game Time clock beside the UI clock: a fixed UI time
// alone holds it (S93), so a recorded Game Time document would not move.
TEST_F(HtmlUIClockTest, FixedGameTimePinsTheGameClockAndReleasesWithoutJump){
    HtmlUIClock::advanceGame(1.0);
    HtmlUIClock::setFixedTime(5.0);
    HtmlUIClock::setFixedGameTime(2.0);
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(UIDocumentUpdateMode::GAME_TIME), 2.0);
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(UIDocumentUpdateMode::UNSCALED_TIME), 5.0);
    HtmlUIClock::setFixedGameTime(2.5);
    HtmlUIClock::advanceGame(1.0);
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(UIDocumentUpdateMode::GAME_TIME), 2.5) << "frames do not move a pinned clock";

    // released: the game clock runs on from the pinned value
    HtmlUIClock::setFixedGameTime(std::nullopt);
    HtmlUIClock::setFixedTime(std::nullopt);
    EXPECT_FALSE(HtmlUIClock::getFixedGameTime().has_value());
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(UIDocumentUpdateMode::GAME_TIME), 2.5);
    HtmlUIClock::advanceGame(0.25);
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(UIDocumentUpdateMode::GAME_TIME), 2.75);

    HtmlUIClock::setFixedGameTime(-1.0);
    EXPECT_FALSE(HtmlUIClock::getFixedGameTime().has_value());
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] HtmlUIClock::setFixedGameTime"), 1u);
    HtmlUIClock::setFixedGameTime(3.0);
    HtmlUIClock::reset();
    EXPECT_FALSE(HtmlUIClock::getFixedGameTime().has_value());
    EXPECT_EQ(HtmlUIClock::now(UIDocumentUpdateMode::GAME_TIME), 0.0);
}

TEST_F(HtmlUIClockTest, InvalidFixedTimeIsIgnoredWithWarning){
    HtmlUIClock::advance(1.0);
    HtmlUIClock::setFixedTime(-1.0);
    HtmlUIClock::setFixedTime(std::numeric_limits<double>::quiet_NaN());
    HtmlUIClock::setFixedTime(std::numeric_limits<double>::infinity());

    EXPECT_FALSE(HtmlUIClock::getFixedTime().has_value());
    EXPECT_DOUBLE_EQ(HtmlUIClock::now(), 1.0);
    EXPECT_EQ(TestSystem::get().countLogs(D_LOG_WARN, "[HTML UI] HtmlUIClock::setFixedTime"), 3u);
}

TEST_F(HtmlUIClockTest, RmlSystemElapsedTimeIsTheClock){
    RmlSystem system;
    EXPECT_EQ(system.GetElapsedTime(), 0.0);

    HtmlUIClock::advance(1.5);
    EXPECT_DOUBLE_EQ(system.GetElapsedTime(), 1.5);

    HtmlUIClock::setFixedTime(42.0);
    EXPECT_DOUBLE_EQ(system.GetElapsedTime(), 42.0);
}

TEST_F(HtmlUIClockTest, SystemDrawAdvancesUnscaledFixedDelta){
    JASPER_REQUIRE_GPU();

    Engine::setFixedDeltatime(0.02);
    Engine::setTimeScale(0.0f);

    const double start = HtmlUIClock::now();
    GpuEnvironment::drawFrames(5);
    // the game is frozen, the UI is not
    EXPECT_EQ(Engine::getDeltatime(), 0.0f);
    EXPECT_NEAR(HtmlUIClock::now() - start, 0.1, 1e-9);

    // time scale 2: the game runs twice as fast, the UI clock does not
    Engine::setTimeScale(2.0f);
    const double scaled = HtmlUIClock::now();
    GpuEnvironment::drawFrames(5);
    EXPECT_NEAR(Engine::getDeltatime(), 0.04f, 1e-6);
    EXPECT_NEAR(HtmlUIClock::now() - scaled, 0.1, 1e-9);
}

TEST_F(HtmlUIClockTest, SystemDrawHeldSimulationStopsTheClock){
    JASPER_REQUIRE_GPU();

    Engine::setFixedDeltatime(0.02);

    Engine::setSimulationHeld(true);
    const double held = HtmlUIClock::now();
    GpuEnvironment::drawFrames(3);
    EXPECT_EQ(HtmlUIClock::now(), held);

    Engine::setSimulationHeld(false);
    GpuEnvironment::drawFrames(2);
    EXPECT_NEAR(HtmlUIClock::now() - held, 0.04, 1e-9);
}

TEST_F(HtmlUIClockTest, SystemDrawMeasuredDeltaIsCapped){
    JASPER_REQUIRE_GPU();

    // a long stall counts as maxDeltatime at most, like the game's delta
    const float maxDelta = Engine::getMaxDeltatime();
    Engine::setMaxDeltatime(0.01f);
    GpuEnvironment::drawFrames(1);
    const double before = HtmlUIClock::now();
    GpuEnvironment::drawFrames(4);
    Engine::setMaxDeltatime(maxDelta);

    const double advanced = HtmlUIClock::now() - before;
    EXPECT_GT(advanced, 0.0);
    EXPECT_LE(advanced, 4 * 0.01 + 1e-6);
}

TEST_F(HtmlUIClockTest, RmlUiAnimationsFollowTheClock){
    JASPER_REQUIRE_GPU();

    SceneFixture fixture;
    Scene& scene = fixture.getScene();

    // the animation starts when the document is first updated: at the pinned time
    HtmlUIClock::setFixedTime(10.0);
    fixture.addDocument("p1/clock_animation.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());

    // A 10-second linear fade must follow the clock even when a frame jumps forward.
    HtmlUIClock::setFixedTime(12.5);
    updateDocuments(scene);
    EXPECT_NEAR(opacityOf(scene, "box"), 0.25f, 2e-3f);
    HtmlUIClock::setFixedTime(15.0);
    updateDocuments(scene);
    EXPECT_NEAR(opacityOf(scene, "box"), 0.5f, 2e-3f);

    // a pinned time does not move the animation
    updateDocuments(scene);
    updateDocuments(scene);
    EXPECT_NEAR(opacityOf(scene, "box"), 0.5f, 2e-3f);

    // released, at time scale 0: frames still animate the document, by the fixed delta
    HtmlUIClock::setFixedTime(std::nullopt);
    Engine::setFixedDeltatime(0.05);
    Engine::setTimeScale(0.0f);
    GpuEnvironment::drawFrames(40);
    EXPECT_NEAR(HtmlUIClock::now(), 17.0, 1e-9);
    EXPECT_NEAR(opacityOf(scene, "box"), 0.7f, 2e-3f);

    // held: frozen
    Engine::setSimulationHeld(true);
    GpuEnvironment::drawFrames(3);
    EXPECT_NEAR(HtmlUIClock::now(), 17.0, 1e-9);
    EXPECT_NEAR(opacityOf(scene, "box"), 0.7f, 2e-3f);
}

// S93 R35 / AC39: a Game Time document's CSS transitions, @keyframes and anime follow (its text
// effects: TextEffectDocumentTest.AutoTimeFollowsTheDocumentClock)
// Engine.timeScale and stop at 0; an Unscaled Time document's do not
TEST_F(HtmlUIClockTest, GameTimeDocumentFollowsTimeScale){
    JASPER_REQUIRE_GPU();

    // every frame 50 ms; the game at half speed (from the documents' first frame)
    Engine::setFixedDeltatime(0.05);
    Engine::setTimeScale(0.5f);

    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument unscaled = fixture.addDocument("p1/clock_update_mode.rml");
    UIDocument game = fixture.addDocument("p1/clock_update_mode.rml");
    game.getComponent<UIDocumentComponent>().updateMode = UIDocumentUpdateMode::GAME_TIME;
    ASSERT_TRUE(fixture.waitUntilLoaded());
    EXPECT_EQ(unscaled.getUpdateMode(), UIDocumentUpdateMode::UNSCALED_TIME);
    EXPECT_EQ(game.getUpdateMode(), UIDocumentUpdateMode::GAME_TIME);

    Rml::Element* unscaledSlide = documentElement(scene, unscaled, "slide");
    Rml::Element* gameSlide = documentElement(scene, game, "slide");
    ASSERT_NE(unscaledSlide, nullptr);
    ASSERT_NE(gameSlide, nullptr);

    // anime without a clock takes its targets' document's
    auto unscaledMove = Anime::animate({AnimTargets::fromElement(unscaled.getElementById("move"))},
                                       AnimParam::object({{"left", 100}, {"duration", 1000}, {"ease", "linear"}}));
    auto gameMove = Anime::animate({AnimTargets::fromElement(game.getElementById("move"))},
                                   AnimParam::object({{"left", 100}, {"duration", 1000}, {"ease", "linear"}}));
    EXPECT_EQ(unscaledMove->getClock(), AnimClock::UI);
    EXPECT_EQ(gameMove->getClock(), AnimClock::GAME);
    // a timeline takes its first animated targets' document's; a given clock wins
    auto timeline = Anime::createTimeline();
    timeline->add({AnimTargets::fromElement(game.getElementById("slide"))}, AnimParam::object({{"opacity", 0.5}, {"duration", 100}}));
    EXPECT_EQ(timeline->getClock(), AnimClock::GAME);
    auto given = Anime::animate({AnimTargets::fromElement(game.getElementById("keys"))},
                                AnimParam::object({{"opacity", 0.5}, {"duration", 100}, {"clock", "ui"}}));
    EXPECT_EQ(given->getClock(), AnimClock::UI);

    // a 1 s transition, started in both documents in the same frame
    unscaledSlide->SetProperty("left", "100px");
    gameSlide->SetProperty("left", "100px");
    GpuEnvironment::drawFrames(11);
    const float unscaledHalf = unscaledSlide->GetOffsetLeft();
    EXPECT_NEAR(unscaledHalf, 50.0f, 6.0f);
    EXPECT_NEAR(gameSlide->GetOffsetLeft(), unscaledHalf * 0.5f, 1.5f) << "half the time scale, half the way";
    EXPECT_NEAR(documentElement(scene, game, "move")->GetOffsetLeft(), documentElement(scene, unscaled, "move")->GetOffsetLeft() * 0.5f, 1.5f);

    // after 1 s the unscaled one is done, the game one half way; after 2 s both are done
    GpuEnvironment::drawFrames(10);
    EXPECT_FLOAT_EQ(unscaledSlide->GetOffsetLeft(), 100.0f);
    EXPECT_NEAR(gameSlide->GetOffsetLeft(), 50.0f, 3.5f);
    GpuEnvironment::drawFrames(21);
    EXPECT_FLOAT_EQ(gameSlide->GetOffsetLeft(), 100.0f);
    EXPECT_FLOAT_EQ(documentElement(scene, game, "move")->GetOffsetLeft(), 100.0f);

    // the @keyframes and the text effect ran at half the time too
    const float unscaledKeys = documentElement(scene, unscaled, "keys")->GetOffsetLeft();
    const float gameKeys = documentElement(scene, game, "keys")->GetOffsetLeft();
    EXPECT_GT(unscaledKeys, 0.0f);
    EXPECT_NEAR(gameKeys, unscaledKeys * 0.5f, 8.0f);

    // time scale 0: the game document holds, the other runs on
    Engine::setTimeScale(0.0f);
    gameSlide->SetProperty("left", "0px");
    unscaledSlide->SetProperty("left", "0px");
    GpuEnvironment::drawFrames(10);
    EXPECT_FLOAT_EQ(gameSlide->GetOffsetLeft(), 100.0f) << "a Game Time transition does not start running at time scale 0";
    EXPECT_FLOAT_EQ(documentElement(scene, game, "keys")->GetOffsetLeft(), gameKeys);
    EXPECT_LT(unscaledSlide->GetOffsetLeft(), 60.0f);
    EXPECT_GT(documentElement(scene, unscaled, "keys")->GetOffsetLeft(), unscaledKeys);
}

// S93 R35: switching a document's Update Mode keeps its running animations' progress
TEST_F(HtmlUIClockTest, UpdateModeChangeKeepsProgress){
    JASPER_REQUIRE_GPU();

    Engine::setFixedDeltatime(0.05);
    Engine::setTimeScale(1.0f);
    SceneFixture fixture;
    Scene& scene = fixture.getScene();
    UIDocument document = fixture.addDocument("p1/clock_update_mode.rml");
    ASSERT_TRUE(fixture.waitUntilLoaded());
    // the clocks far apart: the UI clock ran while the game one was held
    Engine::setTimeScale(0.0f);
    GpuEnvironment::drawFrames(20);
    Engine::setTimeScale(1.0f);

    Rml::Element* slide = documentElement(scene, document, "slide");
    ASSERT_NE(slide, nullptr);
    slide->SetProperty("left", "100px");
    GpuEnvironment::drawFrames(5);
    const float before = slide->GetOffsetLeft();
    EXPECT_GT(before, 0.0f);
    EXPECT_LT(before, 100.0f);

    document.setUpdateMode(UIDocumentUpdateMode::GAME_TIME);
    EXPECT_EQ(document.getComponent<UIDocumentComponent>().appliedUpdateMode, UIDocumentUpdateMode::GAME_TIME);
    updateDocuments(scene);
    EXPECT_NEAR(slide->GetOffsetLeft(), before, 0.5f) << "no jump onto the other clock";
    GpuEnvironment::drawFrames(2);
    EXPECT_NEAR(slide->GetOffsetLeft(), before + 10.0f, 1.0f) << "and it runs on it";
}

// S93 R35: the game clock stops while the game events are paused (the editor outside Play and
// in its Pause) and while a fixed UI time pins the UI
TEST_F(HtmlUIClockTest, GameClockStopsWithTheGameEvents){
    JASPER_REQUIRE_GPU();

    Engine::setFixedDeltatime(0.02);
    Engine::setTimeScale(1.0f);
    const double start = HtmlUIClock::now(UIDocumentUpdateMode::GAME_TIME);
    GpuEnvironment::drawFrames(5);
    EXPECT_NEAR(HtmlUIClock::now(UIDocumentUpdateMode::GAME_TIME) - start, 0.1, 1e-9);

    Engine::pauseGameEvents(true);
    const double paused = HtmlUIClock::now(UIDocumentUpdateMode::GAME_TIME);
    const double unscaled = HtmlUIClock::now();
    GpuEnvironment::drawFrames(3);
    Engine::pauseGameEvents(false);
    EXPECT_EQ(HtmlUIClock::now(UIDocumentUpdateMode::GAME_TIME), paused);
    EXPECT_NEAR(HtmlUIClock::now() - unscaled, 0.06, 1e-9) << "the UI clock runs on (documents animate in edit mode)";

    HtmlUIClock::setFixedTime(3.0);
    GpuEnvironment::drawFrames(3);
    EXPECT_EQ(HtmlUIClock::now(UIDocumentUpdateMode::GAME_TIME), paused) << "a fixed UI time holds the game clock too";
    HtmlUIClock::setFixedTime(std::nullopt);
}
