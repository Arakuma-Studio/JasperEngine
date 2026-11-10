// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "htmlui/anim/Anime.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(AnimeClock, ScopedPreviewTicksLeaveBothClocksAndCallbacksAlone){
    AnimEngine& engine = AnimEngine::instance();
    engine.reset();
    engine.tickMilliseconds(1000, 500);
    auto uiTarget = std::make_shared<AnimObjectTarget>(std::initializer_list<std::pair<const std::string, AnimRawValue>>{{"v", AnimRawValue(0.0)}});
    auto gameTarget = std::make_shared<AnimObjectTarget>(std::initializer_list<std::pair<const std::string, AnimRawValue>>{{"v", AnimRawValue(0.0)}});
    int updates = 0;
    AnimParam params = AnimParam::object({{"v", 1000}, {"duration", 1000}, {"ease", "linear"}});
    params.set("onUpdate", AnimParam::callback([&](AnimTimer&){ ++updates; }));
    auto ui = Anime::animate({uiTarget}, params);
    params.set("clock", "game");
    auto game = Anime::animate({gameTarget}, params);
    {
        AnimEngine::ScopedTickPause outer;
        engine.tickMilliseconds(10000, 10000);
        {
            AnimEngine::ScopedTickPause inner;
            engine.tickMilliseconds(20000, 20000);
        }
        engine.tickMilliseconds(30000, 30000);
        EXPECT_DOUBLE_EQ(engine.now(AnimClock::UI), 1000);
        EXPECT_DOUBLE_EQ(engine.now(AnimClock::GAME), 0);
        EXPECT_DOUBLE_EQ(uiTarget->getNumber("v"), 0);
        EXPECT_DOUBLE_EQ(gameTarget->getNumber("v"), 0);
        EXPECT_EQ(updates, 0);
    }
    engine.tickMilliseconds(1100, 600);
    EXPECT_NEAR(uiTarget->getNumber("v"), 100, 1e-9);
    EXPECT_NEAR(gameTarget->getNumber("v"), 100, 1e-9);
    EXPECT_EQ(updates, 2);
    engine.reset();
}

TEST(AnimeClock, PreviewCollectsOnlyTimersCreatedInItsScope){
    AnimEngine& engine = AnimEngine::instance();
    engine.reset();
    engine.tickMilliseconds(0, 0);
    auto outside = Anime::createTimer();
    std::vector<std::shared_ptr<AnimTimer>> outerTimers;
    std::vector<std::shared_ptr<AnimTimer>> innerTimers;
    std::shared_ptr<AnimTimer> first, second, inner;
    {
        AnimEngine::ScopedTickPause outer(&outerTimers);
        first = Anime::createTimer();
        {
            AnimEngine::ScopedTickPause collectingInner(&innerTimers);
            inner = Anime::createTimer();
        }
        {
            AnimEngine::ScopedTickPause nonCollectingInner;
            second = Anime::createTimer();
        }
    }
    auto after = Anime::createTimer();
    ASSERT_EQ(outerTimers.size(), 2u);
    EXPECT_EQ(outerTimers[0], first);
    EXPECT_EQ(outerTimers[1], second);
    ASSERT_EQ(innerTimers.size(), 1u);
    EXPECT_EQ(innerTimers[0], inner);
    engine.reset();
}
