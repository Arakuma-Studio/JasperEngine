// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S74 R6, R7: the Animator's and the Animation Players' time lands in the profiler's Animation
// band and leaves Scripts and Physics, and the monitors read the Animators' stats.

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"

#include "Engine.h"
#include "component/AnimationPlayerComponent.h"
#include "object/AnimationPlayer.h"
#include "profiler/EngineProfiling.h"
#include "profiler/FrameTimes.h"
#include "profiler/Profile.h"
#include "subsystem/ActionSystem.h"

using namespace animatortest;

namespace {

    class ProfilerAnimationTest : public ::testing::Test, public AnimatorFixture {
    protected:
        void SetUp() override{
            profiler::setEnabled(false);
            nextFrame();
            profiler::clearHistory();
            profiler::setEnabled(true);
            nextFrame();
        }
        void TearDown() override{
            profiler::setEnabled(false);
            nextFrame();
            profiler::clearHistory();
        }
        static void nextFrame(){
            FrameTimes::beginFrame();
            profiler::frameMark();
        }
        static double categoryMs(profiler::Category category){
            double total = 0.0;
            for (size_t i = 0; i < profiler::frameCount(); i++){
                total += profiler::frameAt(i)->categoryMs[(size_t)category];
            }
            return total;
        }
        // enough bones and keys that the update's own time is what the band shows
        void playASlide(AnimatorUpdateMode mode = AnimatorUpdateMode::Normal){
            AnimatorComponent& component = animator(controllerFile("c", controller({}, {state("Play", clip("slide", slide("Hips", 1.0f)))})));
            component.updateMode = mode;
        }
    };

}

TEST_F(ProfilerAnimationTest, TheAnimatorUpdateLeavesScripts){
    playASlide();
    for (int frame = 0; frame < 10; frame++){
        {
            JE_PROFILE_SCOPE("Update");
            system().update(1.0 / 60.0);
        }
        nextFrame();
    }
    ASSERT_TRUE(instance().bound);
    EXPECT_GT(categoryMs(profiler::Category::Animation), 0.0);
}

TEST_F(ProfilerAnimationTest, AnAnimatePhysicsAnimatorLeavesPhysics){
    playASlide(AnimatorUpdateMode::AnimatePhysics);
    for (int frame = 0; frame < 10; frame++){
        {
            JE_PROFILE_SCOPE("FixedUpdate");
            system().fixedUpdate(1.0 / 60.0);
        }
        nextFrame();
    }
    ASSERT_TRUE(instance().bound);
    EXPECT_GT(categoryMs(profiler::Category::Animation), 0.0);
}

TEST_F(ProfilerAnimationTest, TheClipPlayersPassLeavesScripts){
    AnimationPlayer object(&scene, root);
    AnimationPlayerComponent& component = scene.getComponent<AnimationPlayerComponent>(root);
    AnimationPlayerClip entry;
    entry.clip = clip("slide", slide("Hips", 1.0f));
    entry.name = "slide";
    component.clips.push_back(entry);
    component.playAutomatically = false;
    component.needsLoad = true;
    scene.getSystem<ActionSystem>()->update(0.0);
    object.play("slide");
    for (int frame = 0; frame < 10; frame++){
        {
            JE_PROFILE_SCOPE("Update");
            scene.getSystem<ActionSystem>()->update(1.0 / 60.0);
        }
        nextFrame();
    }
    EXPECT_GT(position(hips).x, 0.0f) << "the player played";
    EXPECT_GT(categoryMs(profiler::Category::Animation), 0.0);
}

TEST_F(ProfilerAnimationTest, NothingToAnimateLeavesTheBandEmpty){
    for (int frame = 0; frame < 10; frame++){
        {
            JE_PROFILE_SCOPE("Update");
            system().update(1.0 / 60.0);
            scene.getSystem<ActionSystem>()->update(1.0 / 60.0);
        }
        nextFrame();
    }
    EXPECT_EQ(categoryMs(profiler::Category::Animation), 0.0);
}

TEST_F(ProfilerAnimationTest, FrameStatsCountAFrameOnce){
    playASlide(AnimatorUpdateMode::AnimatePhysics);
    system().takeFrameStats();
    system().fixedUpdate(1.0 / 60.0);
    system().fixedUpdate(1.0 / 60.0);
    system().update(1.0 / 60.0);
    AnimatorSystem::Stats frame = system().takeFrameStats();
    EXPECT_EQ(frame.animators, 1u) << "two fixed steps, one Animator; the update's run does not hide it";
    EXPECT_EQ(frame.sampled, 1u);
    frame = system().takeFrameStats();
    EXPECT_EQ(frame.animators, 0u) << "taken";
    EXPECT_EQ(frame.waveMs, 0.0);
}

TEST_F(ProfilerAnimationTest, TheMonitorsReadTheStats){
    playASlide();
    Engine::addSceneLayer(&scene);
    for (int frame = 0; frame < 3; frame++){
        system().update(1.0 / 60.0);
        profiler::engineFrameEnd(false);
        nextFrame();
    }
    Engine::removeScene(&scene);
    ASSERT_TRUE(instance().bound);

    profiler::MonitorInfo info;
    ASSERT_TRUE(profiler::monitorInfo("Animators", info));
    EXPECT_EQ(info.latest, 1.0);
    ASSERT_TRUE(profiler::monitorInfo("Animators sampled", info));
    EXPECT_EQ(info.latest, 1.0);
    ASSERT_TRUE(profiler::monitorInfo("Animators culled", info));
    EXPECT_EQ(info.latest, 0.0);
    for (const char* name : {"Anim Prepare ms", "Anim Wave ms", "Anim Finalize ms"}){
        ASSERT_TRUE(profiler::monitorInfo(name, info)) << name;
        EXPECT_EQ(info.unit, profiler::MonitorUnit::TimeMs) << name;
    }
    ASSERT_TRUE(profiler::monitorInfo("Anim Wave ms", info));
    EXPECT_GT(info.maximum, 0.0);
    ASSERT_TRUE(profiler::monitorInfo("Clip memory", info));
    EXPECT_EQ(info.unit, profiler::MonitorUnit::Memory);
    EXPECT_GT(info.latest, 0.0);
    EXPECT_EQ(info.latest, (double)AnimationClipPool::loadedBytes());

    // the last Animator gone: the monitors say so instead of keeping its last value
    Engine::addSceneLayer(&scene);
    scene.removeComponent<AnimatorComponent>(root);
    system().update(1.0 / 60.0);
    profiler::engineFrameEnd(false);
    nextFrame();
    Engine::removeScene(&scene);
    ASSERT_TRUE(profiler::monitorInfo("Animators", info));
    EXPECT_EQ(info.latest, 0.0);
}
