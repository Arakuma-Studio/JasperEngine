// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Animator's hot paths of docs/specs/S77-clip-quality.md: a layer of weight 0 is not sampled
// (R14, AC6) and an interruption still blends from the right pose, and a first use through the
// thread pool starts like one through the calling thread (R16, AC7).

#include <gtest/gtest.h>

#include "AnimatorTestUtil.h"

#include "Engine.h"
#include "pool/AnimationClipPoolTesting.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

using namespace jasperengine;
using namespace jasperengine::anim;
using namespace animatortest;

namespace {

    constexpr float kDt = 1.0f / 60.0f;

    // a turn of `degrees` about x of `path` over 1 s, in steps of 0.1 s
    ClipData turn(const std::string& path, float degrees){
        ClipData data;
        data.wrap = ClipWrapMode::Loop;
        ClipTrackData t;
        t.path = path;
        t.channel = ClipChannel::Rotation;
        for (int k = 0; k <= 10; k++){
            const Quaternion q = aboutX(degrees * (float)k * 0.1f);
            t.keys.push_back(key((float)k * 0.1f, {q.x, q.y, q.z, q.w}));
        }
        data.tracks = {t};
        return data;
    }

    class AnimatorHotPath : public ::testing::Test, public AnimatorFixture {
    protected:
        Entity twin = NULL_ENTITY;

        void TearDown() override{
            setAnimationClipPoolTestHooks(AnimationClipPoolTestHooks());
        }

        // A second character with the same hierarchy
        Entity addTwin(){
            twin = node(NULL_ENTITY, "Twin", Vector3(0, 0, 0));
            const Entity twinHips = node(twin, "Hips", Vector3(0, 1, 0));
            const Entity twinSpine = node(twinHips, "Spine", Vector3(0, 0.3f, 0));
            node(twinSpine, "Head", Vector3(0, 0.4f, 0));
            return twin;
        }

        // a base layer and an upper layer of `blending` with states A, B, C (each its own turn of
        // the spine); any state -> C when the parameter "Go" is set
        std::string layered(AnimatorLayerBlending blending = AnimatorLayerBlending::Override){
            const std::string base = clip("base", turn("Hips", 30.0f));
            const std::string a = clip("a", turn("Hips/Spine", 40.0f));
            const std::string b = clip("b", turn("Hips/Spine", -50.0f));
            const std::string c = clip("c", turn("Hips/Spine", 70.0f));
            ControllerDef def = controller({parameter("Go", AnimatorParameterType::Bool)}, {state("Base", base)});
            LayerDef upper;
            upper.name = "Upper";
            upper.weight = 0.0f;
            upper.blending = blending;
            StateDef sa = state("A", a), sb = state("B", b), sc = state("C", c);
            upper.machine.states = {sa, sb, sc};
            upper.machine.anyState.push_back(transition("C", {condition("Go", AnimatorConditionMode::If)}, 0.4f));
            def.layers.push_back(upper);
            return controllerFile("layered", def);
        }

        static size_t slotOf(const AnimatorInstance& instance, const std::string& path){
            for (size_t i = 0; i < instance.clips.size(); i++){
                if (instance.clips[i].path.find(path) != std::string::npos) return i;
            }
            ADD_FAILURE() << "no clip slot " << path;
            return 0;
        }

        static bool sampledSinceBind(const AnimatorClipSlot& slot){
            for (const RigClipCursors::Location& location : slot.cursors.locations){
                if (location.at.index != 0 || location.at.interpolation != 0.0f) return true;
            }
            return false;
        }

        // both characters in state B of the upper layer, away from any key: skipped on the root,
        // sampled on the twin
        void startInterruption(){
            const std::string path = layered();
            animator(path);
            animator(path, addTwin());
            step(0.0);
            ASSERT_TRUE(instance(root).bound);
            instance(twin).setLayerWeight(1, 1e-6f);
            for (Entity e : {root, twin}) instance(e).play("B", 1, 0.0f);
            step(kDt, 17);
            EXPECT_TRUE(instance(root).layers[1].outputSkipped);
            EXPECT_FALSE(instance(twin).layers[1].outputSkipped);
        }

        void expectSameAfterInterruption(){
            instance(twin).setLayerWeight(1, 1.0f);
            for (int frame = 0; frame < 12; frame++){
                step(kDt);
                expectSamePose("after the interruption", frame);
            }
        }

        void expectSamePose(const char* when, int frame = -1){
            const Pose& a = instance(root).pose();
            const Pose& b = instance(twin).pose();
            ASSERT_EQ(a.floatCount(), b.floatCount());
            EXPECT_EQ(std::memcmp(a.raw(), b.raw(), a.floatCount() * sizeof(float)), 0) << when << ", frame " << frame;
        }
    };

}

// AC6: a layer of weight 0 costs no sampling, and its time still runs: when its weight comes up
// the pose is the one of a layer that was sampled all along
TEST_F(AnimatorHotPath, WeightZeroLayerIsNotSampled){
    const std::string path = layered();
    animator(path);
    animator(path, addTwin());
    step(0.0);
    ASSERT_TRUE(instance(root).bound);
    instance(twin).setLayerWeight(1, 1e-6f);
    step(kDt, 30);
    EXPECT_TRUE(sampledSinceBind(instance(root).clips[slotOf(instance(root), "base.anim")])) << "the base layer samples";
    EXPECT_TRUE(sampledSinceBind(instance(twin).clips[slotOf(instance(twin), "a.anim")])) << "any weight samples";
    EXPECT_FALSE(sampledSinceBind(instance(root).clips[slotOf(instance(root), "a.anim")])) << "weight 0 does not";

    // the same frame's weight, set before the update
    instance(root).setLayerWeight(1, 1.0f);
    instance(twin).setLayerWeight(1, 1.0f);
    step(kDt);
    EXPECT_TRUE(sampledSinceBind(instance(root).clips[slotOf(instance(root), "a.anim")]));
    expectSamePose("the weight came up");
    // a fade: this update's step already counts
    instance(root).setLayerWeight(1, 0.0f);
    step(kDt, 10);
    EXPECT_TRUE(instance(root).layers[1].outputSkipped);
    instance(root).setLayerWeight(1, 1.0f, 0.1f);
    step(kDt);
    EXPECT_NEAR(instance(root).getLayerWeight(1), kDt / 0.1f, 1e-5f);
    EXPECT_FALSE(instance(root).layers[1].outputSkipped);
}

// An additive layer's reference is not taken either
TEST_F(AnimatorHotPath, WeightZeroAdditiveLayerTakesNoReference){
    animator(layered(AnimatorLayerBlending::Additive));
    step(0.0);
    step(kDt, 10);
    const AnimatorClipSlot& slot = instance().clips[slotOf(instance(), "a.anim")];
    EXPECT_FALSE(slot.hasReference);
    instance().setLayerWeight(1, 1.0f);
    step(kDt);
    EXPECT_TRUE(instance().clips[slotOf(instance(), "a.anim")].hasReference);
}

// AC6: the interruption snapshot of a layer of weight 0, with the weight coming up in the same
// frame. The twin's layer has a weight too small to show, so it is sampled every frame: what
// the snapshot must hold is what it holds there
TEST_F(AnimatorHotPath, SnapshotOfTwoCrossFadesInOneFrame){
    startInterruption();
    for (Entity e : {root, twin}){
        // the first leaves the state, the second interrupts it before any evaluation
        ASSERT_TRUE(instance(e).crossFade("A", 0.3f, true, 1, 0.0f));
        ASSERT_TRUE(instance(e).crossFade("C", 0.3f, true, 1, 0.0f));
    }
    instance(root).setLayerWeight(1, 1.0f);
    expectSameAfterInterruption();
}

TEST_F(AnimatorHotPath, SnapshotOfACrossFadeAndAnAnyStateTransition){
    startInterruption();
    for (Entity e : {root, twin}){
        // the update's any-state transition interrupts the script's crossfade
        ASSERT_TRUE(instance(e).crossFade("A", 0.3f, true, 1, 0.0f));
        instance(e).setBool("Go", true);
    }
    instance(root).setLayerWeight(1, 1.0f);
    expectSameAfterInterruption();
}

TEST_F(AnimatorHotPath, SnapshotOfARunningTransition){
    startInterruption();
    // a transition runs for some frames at weight 0 (its pose is kept), then is interrupted
    for (Entity e : {root, twin}) ASSERT_TRUE(instance(e).crossFade("A", 0.4f, true, 1, 0.0f));
    step(kDt, 6);
    for (Entity e : {root, twin}) ASSERT_TRUE(instance(e).crossFade("C", 0.3f, true, 1, 0.0f));
    instance(root).setLayerWeight(1, 1.0f);
    expectSameAfterInterruption();
}

TEST_F(AnimatorHotPath, SnapshotWithAWeightFadeInTheSameFrame){
    startInterruption();
    for (Entity e : {root, twin}){
        ASSERT_TRUE(instance(e).crossFade("A", 0.3f, true, 1, 0.0f));
        ASSERT_TRUE(instance(e).crossFade("C", 0.3f, true, 1, 0.0f));
    }
    // this update's fade step reaches the full weight
    instance(root).setLayerWeight(1, 1.0f, 1e-3f);
    expectSameAfterInterruption();
}

// R16: a preview shows its pose in the call, as without async loading (the editor's mode)
TEST_F(AnimatorHotPath, PreviewLoadsAnAnimInTheCall){
    // large enough for the thread pool otherwise
    animtest::writeAsset(prefix + "base.anim", anim::writeClip(turn("Hips", 30.0f)) + std::string(70000, ' '));
    animator(controllerFile("preview", controller({}, {state("Base", prefix + "base.anim")})));
    const bool wasAsync = Engine::isAsyncLoading();
    Engine::setAsyncLoading(true);
    std::string error;
    const bool shown = system().preview(root, 0.5f, &error);
    Engine::setAsyncLoading(wasAsync);
    ASSERT_TRUE(shown) << error;
    EXPECT_TRUE(instance().bound);
    EXPECT_NEAR(std::fabs(rotation(hips).dot(aboutX(15.0f))), 1.0f, 1e-5f);
}

// AC7 (R16): a first use through the thread pool starts where one through the calling thread
// does, even when one of its clips arrives later than another: time does not run without them
TEST_F(AnimatorHotPath, ThreadPoolFirstUseMatchesTheCallingThread){
    const std::string base = clip("base", turn("Hips", 30.0f));
    ClipData late = turn("Hips/Spine", 40.0f);
    late.events = {event(0.05f, "early")};
    // large enough to go through the thread pool
    animtest::writeAsset(prefix + "late.anim", anim::writeClip(late) + std::string(70000, ' '));
    const std::string lateClip = prefix + "late.anim";
    ControllerDef def = controller({}, {state("Base", base)});
    LayerDef upper;
    upper.name = "Upper";
    upper.machine.states = {state("Late", lateClip)};
    def.layers.push_back(upper);
    const std::string path = controllerFile("first", def);

    // the calling thread
    animator(path);
    size_t syncEvents = 0;
    scene.getComponent<AnimatorComponent>(root).onEvent.add("sync", [&](const AnimationEvent&){ syncEvents++; });
    step(0.0);
    ASSERT_TRUE(instance(root).bound);
    AnimationClipPool::clear();

    // the thread pool, the second clip held back while the frames run
    auto release = std::make_shared<std::atomic<bool>>(false);
    struct Release{
        std::shared_ptr<std::atomic<bool>> flag;
        ~Release(){ *flag = true; }
    } releaseAtEnd{release};
    AnimationClipPoolTestHooks hooks;
    hooks.beforeInsert = [release](const std::string& key){
        if (key.find("late.anim") == std::string::npos) return;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!*release && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    setAnimationClipPoolTestHooks(hooks);
    const bool wasAsync = Engine::isAsyncLoading();
    struct AsyncOff{
        bool was;
        ~AsyncOff(){ Engine::setAsyncLoading(was); }
    } asyncOff{wasAsync};
    Engine::setAsyncLoading(true);
    animator(path, addTwin());
    size_t asyncEvents = 0;
    scene.getComponent<AnimatorComponent>(twin).onEvent.add("async", [&](const AnimationEvent&){ asyncEvents++; });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    // the frames pass until the first clip is there, and some more
    int after = 0;
    while (after < 10 && std::chrono::steady_clock::now() < until){
        // only the twin moves while it waits
        scene.getComponent<AnimatorComponent>(root).enabled = false;
        step(kDt);
        if (AnimationClipPool::get(base)) after++;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_GE(after, 10) << "the first clip never arrived";
    AnimatorInstance* waiting = scene.getComponent<AnimatorComponent>(twin).runtime.get();
    ASSERT_TRUE(waiting);
    EXPECT_FALSE(waiting->bound) << "no bind until every clip is there";
    *release = true;
    const auto bindUntil = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!instance(twin).bound && std::chrono::steady_clock::now() < bindUntil){
        step(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Engine::setAsyncLoading(wasAsync);
    ASSERT_TRUE(instance(twin).bound);
    EXPECT_FLOAT_EQ(instance(twin).currentState(1).normalizedTime, 0.0f) << "no time passed before the clips";

    scene.getComponent<AnimatorComponent>(root).enabled = true;
    for (int frame = 0; frame < 30; frame++){
        step(kDt);
        expectSamePose("after the first use", frame);
        EXPECT_FLOAT_EQ(instance(twin).currentState(1).normalizedTime, instance(root).currentState(1).normalizedTime) << frame;
    }
    EXPECT_EQ(asyncEvents, syncEvents) << "the clip's events from its start";
    EXPECT_GT(syncEvents, 0u);
}
