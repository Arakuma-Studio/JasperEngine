// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// HtmlUIClock (phase 1 wave 0): advance, fixed time and its release, RmlSystem's elapsed
// time, Engine::systemDraw advancing it with the unscaled (fixed) delta whatever the time
// scale, not while the simulation is held, and RmlUi animations following it.

#include "TestDocumentCommon.h"

#include "Engine.h"
#include "HtmlUIClock.h"
#include "RmlSystem.h"

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

    // 10 s linear fade. RmlUi 6.3 advances an animation by at most 0.1 s per update
    // (ElementAnimation::UpdateAndGetProperty, removed by phase 3), so the pinned clock steps
    // in 50 ms updates like frames would
    auto stepTo = [&scene](double from, double to){
        const int steps = (int)std::lround((to - from) / 0.05);
        for (int i = 1; i <= steps; i++){
            HtmlUIClock::setFixedTime(from + 0.05 * i);
            updateDocuments(scene);
        }
    };
    stepTo(10.0, 12.5);
    EXPECT_NEAR(opacityOf(scene, "box"), 0.25f, 2e-3f);
    stepTo(12.5, 15.0);
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
