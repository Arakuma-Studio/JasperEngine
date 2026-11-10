// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S48 R20-R21 (docs/specs/S48-pixel-fx.md AC16-AC17): Hit-Stop on the engine clock. The
// countdown runs on unscaled time and scales the frame's game delta for the part of the frame it
// covers; overlapping calls keep the longer time and the lower scale; Hit Stop Exempt GameObjects
// (and their children) and a hit's exclude list get the unfrozen delta; an `only` hit freezes
// just its GameObjects; a script inside an exempt GameObject reads the unfrozen delta.
// (test_hit_stop.py checks the same in Play: Time.deltaTime, physics, timers, audio.)

#include <gtest/gtest.h>

#include "Engine.h"
#include "Scene.h"
#include "component/HitStopExemptComponent.h"
#include "component/Transform.h"
#include "pixelart/HitStop.h"

#include <cmath>

using namespace jasperengine;

namespace {

    constexpr double kDt = 1.0 / 60.0;

    class HitStop : public ::testing::Test {
    protected:
        void SetUp() override { pixelart::HitStop::reset(); }
        void TearDown() override { pixelart::HitStop::reset(); }

        // one engine frame of `dt` unscaled seconds at time scale 1: the world's factor
        double frame(double dt = kDt){ return pixelart::HitStop::beginFrame(dt, dt); }

        Entity node(Entity parent = NULL_ENTITY){
            Entity entity = scene.createEntity();
            Transform transform;
            transform.parent = parent;
            scene.addComponent<Transform>(entity, transform);
            return entity;
        }

        Scene scene;
    };

}

TEST_F(HitStop, IdleIsOneAndFree){
    for (int i = 0; i < 10; i++) EXPECT_EQ(frame(), 1.0);
    EXPECT_FALSE(pixelart::HitStop::isActive());
    EXPECT_FALSE(pixelart::HitStop::isFrameActive());
    EXPECT_EQ(pixelart::HitStop::getFactor(), 1.0f);
    const Entity e = node();
    EXPECT_EQ(pixelart::HitStop::deltaFor(&scene, e, kDt), kDt);
}

TEST_F(HitStop, FreezesForItsUnscaledSecondsThenResumes){
    pixelart::HitStop::play(0.1f);
    EXPECT_TRUE(pixelart::HitStop::isActive());
    EXPECT_FLOAT_EQ(pixelart::HitStop::getRemaining(), 0.1f);
    // 0.1 s at 60 fps: 6 frozen frames, then the game runs again
    for (int i = 0; i < 6; i++) EXPECT_EQ(frame(), 0.0) << "frame " << i;
    EXPECT_FALSE(pixelart::HitStop::isActive());
    EXPECT_EQ(frame(), 1.0);
    EXPECT_FALSE(pixelart::HitStop::isFrameActive());
}

TEST_F(HitStop, APartlyCoveredFrameRunsTheRest){
    pixelart::HitStop::play(0.025f);
    EXPECT_EQ(frame(), 0.0);
    // 0.025 - 1/60 = 1/120 s of the second frame is frozen: half of it runs
    EXPECT_NEAR(frame(), 0.5, 1e-6);
    EXPECT_EQ(frame(), 1.0);
}

TEST_F(HitStop, TimeScaleSlowsInsteadOfFreezing){
    pixelart::HitStop::Options options;
    options.timeScale = 0.25f;
    pixelart::HitStop::play(0.1f, options);
    EXPECT_NEAR(frame(), 0.25, 1e-9);
    EXPECT_FLOAT_EQ(pixelart::HitStop::getFactor(), 0.25f);
    // clamped to 0..1
    pixelart::HitStop::stop();
    options.timeScale = 3.0f;
    pixelart::HitStop::play(0.1f, options);
    EXPECT_EQ(frame(), 1.0);
}

TEST_F(HitStop, OverlapsKeepTheLongerTimeAndTheLowerScale){
    pixelart::HitStop::Options slow;
    slow.timeScale = 0.5f;
    pixelart::HitStop::play(0.2f, slow);
    frame();
    pixelart::HitStop::Options frozen;
    frozen.timeScale = 0.0f;
    pixelart::HitStop::play(0.05f, frozen);
    EXPECT_NEAR(pixelart::HitStop::getRemaining(), 0.2 - kDt, 1e-6) << "the longer remaining time";
    EXPECT_EQ(frame(), 0.0) << "the lower scale";
    pixelart::HitStop::play(0.5f, slow);
    EXPECT_FLOAT_EQ(pixelart::HitStop::getRemaining(), 0.5f);
    EXPECT_EQ(frame(), 0.0) << "still the lower scale";
    pixelart::HitStop::stop();
    EXPECT_FALSE(pixelart::HitStop::isActive());
    EXPECT_EQ(frame(), 1.0);
}

TEST_F(HitStop, ExemptGameObjectsAndTheirChildrenRun){
    const Entity hero = node();
    const Entity sword = node(hero);
    const Entity enemy = node();
    scene.addComponent<HitStopExemptComponent>(hero, {});
    pixelart::HitStop::play(0.1f);
    frame();
    EXPECT_TRUE(pixelart::HitStop::isExempt(&scene, hero));
    EXPECT_TRUE(pixelart::HitStop::isExempt(&scene, sword)) << "includeChildren";
    EXPECT_FALSE(pixelart::HitStop::isExempt(&scene, enemy));
    // the systems' per-entity delta: the world's (frozen) one becomes the unfrozen one
    EXPECT_NEAR(pixelart::HitStop::deltaFor(&scene, hero, 0.0), kDt, 1e-12);
    EXPECT_NEAR(pixelart::HitStop::deltaFor(&scene, sword, 0.0), kDt, 1e-12);
    EXPECT_EQ(pixelart::HitStop::deltaFor(&scene, enemy, 0.0), 0.0);
    EXPECT_NEAR(pixelart::HitStop::getUnfrozenDeltaTime(), kDt, 1e-12);

    // includeChildren off: the child freezes with the world (resolved again next frame)
    scene.getComponent<HitStopExemptComponent>(hero).includeChildren = false;
    frame();
    EXPECT_TRUE(pixelart::HitStop::isExempt(&scene, hero));
    EXPECT_FALSE(pixelart::HitStop::isExempt(&scene, sword));
    // disabled: nothing is exempt
    scene.getComponent<HitStopExemptComponent>(hero).enabled = false;
    frame();
    EXPECT_FALSE(pixelart::HitStop::isExempt(&scene, hero));
}

TEST_F(HitStop, ExcludeExemptsForThatHitOnly){
    const Entity hero = node();
    const Entity cape = node(hero);
    const Entity enemy = node();
    pixelart::HitStop::Options options;
    options.exclude = {{&scene, hero}};
    pixelart::HitStop::play(0.05f, options);
    frame();
    EXPECT_TRUE(pixelart::HitStop::isExempt(&scene, hero));
    EXPECT_TRUE(pixelart::HitStop::isExempt(&scene, cape));
    EXPECT_FALSE(pixelart::HitStop::isExempt(&scene, enemy));
    for (int i = 0; i < 5; i++) frame();
    // the next hit has no exclude list
    pixelart::HitStop::play(0.05f);
    frame();
    EXPECT_FALSE(pixelart::HitStop::isExempt(&scene, hero));
}

TEST_F(HitStop, OnlyFreezesTheListedAndTheWorldRuns){
    const Entity fighter = node();
    const Entity fist = node(fighter);
    const Entity bystander = node();
    pixelart::HitStop::Options options;
    options.only = {{&scene, fighter}};
    pixelart::HitStop::play(0.1f, options);
    EXPECT_EQ(frame(), 1.0) << "the world's delta is not scaled";
    EXPECT_EQ(pixelart::HitStop::getWorldFactor(), 1.0);
    EXPECT_EQ(pixelart::HitStop::deltaFor(&scene, fighter, kDt), 0.0);
    EXPECT_EQ(pixelart::HitStop::deltaFor(&scene, fist, kDt), 0.0) << "and its descendants";
    EXPECT_EQ(pixelart::HitStop::deltaFor(&scene, bystander, kDt), kDt);
    // an exempt GameObject inside an only target still runs
    scene.addComponent<HitStopExemptComponent>(fist, {});
    frame();
    EXPECT_EQ(pixelart::HitStop::deltaFor(&scene, fist, kDt), kDt);
}

TEST_F(HitStop, ScriptsOfExemptGameObjectsReadTheUnfrozenDelta){
    const Entity hero = node();
    const Entity enemy = node();
    scene.addComponent<HitStopExemptComponent>(hero, {});
    pixelart::HitStop::play(0.1f);
    frame();
    // (Engine::getDeltatime() is the world's frozen 0 here)
    {
        pixelart::HitStop::ScriptScope scope(&scene, hero);
        EXPECT_NEAR(pixelart::HitStop::getScriptDeltaTime(), kDt, 1e-12);
        {
            pixelart::HitStop::ScriptScope inner(&scene, enemy);
            EXPECT_EQ(pixelart::HitStop::getScriptDeltaTime(), (double)Engine::getDeltatime());
        }
        EXPECT_NEAR(pixelart::HitStop::getScriptDeltaTime(), kDt, 1e-12) << "the scope nests";
    }
    EXPECT_EQ(pixelart::HitStop::getScriptDeltaTime(), (double)Engine::getDeltatime()) << "outside any script";
}

TEST_F(HitStop, StatsRecordWhatWasAskedFor){
    pixelart::HitStop::resetStats();
    pixelart::HitStop::play(0.08f);
    pixelart::HitStop::play(0.4f);
    const pixelart::HitStopStats stats = pixelart::HitStop::getStats();
    EXPECT_EQ(stats.requests, 2u);
    EXPECT_NEAR(stats.totalSeconds, 0.48, 1e-6);
    EXPECT_NEAR(stats.longestSeconds, 0.4, 1e-6);
    pixelart::HitStop::resetStats();
    EXPECT_EQ(pixelart::HitStop::getStats().requests, 0u);
}

TEST_F(HitStop, BadSecondsAreIgnored){
    pixelart::HitStop::play(0.0f);
    pixelart::HitStop::play(-1.0f);
    pixelart::HitStop::play(std::nanf(""));
    EXPECT_FALSE(pixelart::HitStop::isActive());
    EXPECT_EQ(frame(), 1.0);
}
