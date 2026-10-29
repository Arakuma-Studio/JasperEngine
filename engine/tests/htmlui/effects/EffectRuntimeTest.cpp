// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The effects' per-element runtime (phase-5 spec 3.4, 3.6, 5.1): the effect clock and the
// integrated clocks, state keying across re-instancing and its LRU and purge, the pointer mapped
// through transforms, smoothing and presence, the trail, ripples and the seed. The instances are
// acquired and prepared directly on the elements of a document (no shader is drawn), with the
// runtime's frames advanced by the test and the UI clock driven through HtmlUIClock.

#include "EffectTestCommon.h"

#include "htmlui/HtmlUIClock.h"
#include "htmlui/effects/EffectInstancers.h"

using namespace jasperengine;
using namespace jasperengine::test;
using namespace jasperengine::test::effects;
using namespace jasperengine::htmlui;

namespace {

    const char* kDocument = R"(<!DOCTYPE html>
<html><head><style>
html, body { margin: 0; width: 320px; height: 200px; }
div { position: absolute; }
#a { left: 10px; top: 10px; width: 100px; height: 50px; }
#b { left: 10px; top: 120px; width: 100px; height: 50px; }
#t { left: 10px; top: 70px; width: 100px; height: 40px; transform: translate(30px, 5px); }
#s { left: 130px; top: 10px; width: 50px; height: 20px; transform-origin: 0 0; transform: scale(2, 1.5); }
#r { left: 280px; top: 60px; width: 60px; height: 30px; transform-origin: 0 0; transform: rotate(90deg); }
</style></head>
<body><div id="a"></div><div id="b"></div><div id="t"></div><div id="s"></div><div id="r"></div></body></html>)";

    constexpr int kBorder = (int)Rml::BoxArea::Border;

    std::shared_ptr<const EffectParamsSnapshot> snapshot(const std::string& arguments){
        EffectSnapshotResult result = snapshotFromCall(arguments, false);
        EXPECT_TRUE(result.snapshot) << arguments << ": " << (result.problems.empty() ? "" : result.problems.front());
        return result.snapshot;
    }

    struct EffectRuntimeTest : ::testing::Test {
        std::unique_ptr<EffectScene> scene;
        std::vector<EffectInstance*> instances;

        void SetUp() override{
            TestSystem::get().clearLogs();
            scene = std::make_unique<EffectScene>(320, 200, 1);
            if (!scene->valid())
                GTEST_SKIP() << "no GPU";
            std::string error;
            ASSERT_TRUE(scene->loadHtml(kDocument, &error)) << error;
            TestImage image;
            ASSERT_TRUE(scene->capture(image));   // layout and transforms
            EffectRuntime::get().clearTestInput();
        }

        void TearDown() override{
            for (EffectInstance* instance : instances)
                EffectRuntime::get().release(instance);
            instances.clear();
            EffectClock::setFixedTime(std::nullopt);
            EffectRuntime::get().clearTestInput();
            HtmlUIClock::setFixedTime(0.0);
            EffectRegistry::get().clearProjectEffects();
            scene.reset();
        }

        Rml::Element* element(const std::string& id){
            Rml::Element* e = scene->element(id);
            EXPECT_NE(e, nullptr) << id;
            return e;
        }

        EffectInstance* acquire(const std::string& id, const std::string& arguments){
            std::shared_ptr<const EffectParamsSnapshot> snap = snapshot(arguments);
            if (!snap)
                return nullptr;
            EffectInstance* instance = EffectRuntime::get().acquire(element(id), *snap->manifest, false, snap, kBorder);
            instances.push_back(instance);
            return instance;
        }

        void release(EffectInstance* instance){
            instances.erase(std::remove(instances.begin(), instances.end(), instance), instances.end());
            EffectRuntime::get().release(instance);
        }

        // the next runtime frame at UI time t (the effect clock follows the UI clock)
        static void frameAt(double t){
            HtmlUIClock::setFixedTime(t);
            EffectRuntime::get().advanceFrameForTests();
        }

        Rml::Context* context(){ return scene->context(); }
    };

    float pointerX(const EffectInstance& i){ return i.frame.pointer[0]; }
    float pointerY(const EffectInstance& i){ return i.frame.pointer[1]; }

}

TEST_F(EffectRuntimeTest, FixedClock){
    EffectClock::setFixedTime(2.5);
    EXPECT_TRUE(EffectClock::isFixed());
    EXPECT_DOUBLE_EQ(EffectClock::now(), 2.5);
    EXPECT_EQ(EffectClock::getFixedTime(), std::optional<double>(2.5));
    // a non-finite time is ignored
    EffectClock::setFixedTime(std::numeric_limits<double>::quiet_NaN());
    EXPECT_DOUBLE_EQ(EffectClock::now(), 2.5);
    EffectClock::setFixedTime(std::nullopt);
    EXPECT_FALSE(EffectClock::isFixed());
    HtmlUIClock::setFixedTime(7.25);
    EXPECT_DOUBLE_EQ(EffectClock::now(), 7.25) << "without a fixed time the effects run on the UI clock";

    // the frame block: time = t * time-scale + time-offset, dt 0 and the fixed flag
    EffectClock::setFixedTime(3.0);
    EffectRuntime::get().advanceFrameForTests();
    EffectInstance* fx = acquire("a", "stripes, time-scale 0.5, time-offset 1s");
    ASSERT_NE(fx, nullptr);
    EffectRuntime::get().prepare(*fx);
    EXPECT_FLOAT_EQ(fx->frame.time[0], 2.5f);
    EXPECT_FLOAT_EQ(fx->frame.time[1], 0.f);
    EXPECT_FLOAT_EQ(fx->frame.time[3], 1.f);
    EXPECT_FLOAT_EQ(fx->frame.size[0], 100.f);
    EXPECT_FLOAT_EQ(fx->frame.size[1], 50.f);
}

TEST_F(EffectRuntimeTest, IntegratedClockClosedFormWhenFixed){
    // anim = (t * time-scale + time-offset) * rate
    EffectClock::setFixedTime(3.0);
    EffectRuntime::get().advanceFrameForTests();
    EffectInstance* fx = acquire("a", "stripes, speed 0.8, time-scale 0.5, time-offset 1s");
    ASSERT_NE(fx, nullptr);
    EffectRuntime::get().prepare(*fx);
    EXPECT_FLOAT_EQ(fx->frame.anim[0], 2.f);
    EffectClock::setFixedTime(4.0);
    EffectRuntime::get().advanceFrameForTests();
    EffectRuntime::get().prepare(*fx);
    EXPECT_FLOAT_EQ(fx->frame.anim[0], 2.4f);
}

TEST_F(EffectRuntimeTest, IntegratedClockLiveAndNoJump){
    // live: anim starts at time-offset * rate and grows by dt * rate * time-scale
    frameAt(10.0);
    EffectInstance* fx = acquire("a", "stripes, speed 0.4, time-scale 0.5, time-offset 1s");
    ASSERT_NE(fx, nullptr);
    EffectRuntime::get().prepare(*fx);
    EXPECT_FLOAT_EQ(fx->frame.anim[0], 0.4f);
    const double dt = 1.0 / 60.0;
    double t = 10.0;
    for (int i = 0; i < 30; i++){
        t += dt;
        frameAt(t);
        EffectRuntime::get().prepare(*fx);
    }
    EXPECT_NEAR(fx->frame.anim[0], 0.4 + 30 * dt * 0.4 * 0.5, 1e-4);
    EXPECT_NEAR(fx->frame.time[1], dt, 1e-6) << "dt in the frame block";

    // speed changes live (re-instanced, as a transition or var() does): no jump
    const float before = fx->frame.anim[0];
    t += dt;
    frameAt(t);
    EffectInstance* faster = acquire("a", "stripes, speed 1, time-scale 0.5, time-offset 1s");
    ASSERT_NE(faster, nullptr);
    release(fx);
    EffectRuntime::get().prepare(*faster);
    EXPECT_LE(std::abs(faster->frame.anim[0] - before), (float)(dt * 1.0 * 0.5) + 1e-4f) << "anim jumped when the rate changed";
    EXPECT_GT(faster->frame.anim[0], before);

    // a long frame counts 0.25 s at most
    t += 5.0;
    const float beforeGap = faster->frame.anim[0];
    frameAt(t);
    EffectRuntime::get().prepare(*faster);
    EXPECT_NEAR(faster->frame.anim[0] - beforeGap, 0.25f * 1.f * 0.5f, 1e-4);
}

TEST_F(EffectRuntimeTest, PointerUnderTransforms){
    EffectClock::setFixedTime(1.0);
    struct Case {
        const char* id;
        Rml::Vector2f local;
        Rml::Vector2f context;
    };
    const Case cases[] = {
        {"a", {25.f, 10.f}, {10.f + 25.f, 10.f + 10.f}},
        {"t", {12.f, 30.f}, {10.f + 30.f + 12.f, 70.f + 5.f + 30.f}},
        {"s", {20.f, 8.f}, {130.f + 2.f * 20.f, 10.f + 1.5f * 8.f}},
        {"r", {40.f, 10.f}, {280.f - 10.f, 60.f + 40.f}},   // rotate(90deg): (x, y) -> (-y, x)
    };
    for (const Case& c : cases){
        SCOPED_TRACE(c.id);
        EffectRuntime::get().advanceFrameForTests();
        EffectRuntime::get().setTestPointer(context(), c.context, false);
        EffectInstance* fx = acquire(c.id, "spotlight");
        ASSERT_NE(fx, nullptr);
        EffectRuntime::get().prepare(*fx);
        EXPECT_NEAR(pointerX(*fx), c.local.x, 0.5f);
        EXPECT_NEAR(pointerY(*fx), c.local.y, 0.5f);
        EXPECT_FLOAT_EQ(fx->frame.pointer[2], 1.f) << "inside";
        // the fixed clock snaps the smoothing and the presence
        EXPECT_NEAR(fx->frame.pointerSmooth[0], c.local.x, 0.5f);
        EXPECT_NEAR(fx->frame.pointerSmooth[1], c.local.y, 0.5f);
        EXPECT_FLOAT_EQ(fx->frame.pointerSmooth[2], 1.f);
    }
    // outside, and no pointer at all
    EffectRuntime::get().advanceFrameForTests();
    EffectRuntime::get().setTestPointer(context(), Rml::Vector2f(300.f, 190.f), false);
    EffectInstance* fx = acquire("a", "spotlight");
    EffectRuntime::get().prepare(*fx);
    EXPECT_FLOAT_EQ(fx->frame.pointer[2], 0.f);
    EXPECT_FLOAT_EQ(fx->frame.pointerSmooth[2], 0.f);
    EffectRuntime::get().advanceFrameForTests();
    EffectRuntime::get().setTestPointer(context(), std::nullopt, false);
    EffectRuntime::get().prepare(*fx);
    EXPECT_FLOAT_EQ(fx->frame.pointer[2], 0.f);
    EXPECT_GT(std::abs(fx->frame.pointer[0]), 1e5f) << "no pointer reads as far away";
}

TEST_F(EffectRuntimeTest, SmoothingAndPresence){
    // live clock, 20 ms frames: the smoothed pointer covers half the way in 80 ms, presence
    // takes 150 ms
    double t = 2.0;
    frameAt(t);
    EffectRuntime::get().setTestPointer(context(), Rml::Vector2f(300.f, 190.f), false);   // outside #a
    EffectInstance* fx = acquire("a", "spotlight");
    ASSERT_NE(fx, nullptr);
    EffectRuntime::get().prepare(*fx);
    EXPECT_FLOAT_EQ(fx->frame.pointerSmooth[2], 0.f);

    // enters at (20, 20) local
    EffectRuntime::get().setTestPointer(context(), Rml::Vector2f(30.f, 30.f), false);
    const Rml::Vector2f from(fx->frame.pointerSmooth[0], fx->frame.pointerSmooth[1]);
    const Rml::Vector2f to(20.f, 20.f);
    for (int i = 0; i < 4; i++){
        t += 0.02;
        frameAt(t);
        EffectRuntime::get().prepare(*fx);
        EXPECT_NEAR(fx->frame.pointerSmooth[2], std::min(1.f, 0.02f * (i + 1) / 0.15f), 1e-4) << "presence after " << (i + 1) << " frames";
    }
    // 80 ms: half the way
    EXPECT_NEAR(fx->frame.pointerSmooth[0], from.x + (to.x - from.x) * 0.5f, 0.05f);
    EXPECT_NEAR(fx->frame.pointerSmooth[1], from.y + (to.y - from.y) * 0.5f, 0.05f);
    EXPECT_FLOAT_EQ(fx->frame.pointer[0], 20.f) << "the raw pointer does not lag";
    // since-enter counts from the first frame inside
    EXPECT_NEAR(fx->frame.pointerEx[3], 0.06f, 1e-4);
    for (int i = 0; i < 4; i++){
        t += 0.02;
        frameAt(t);
        EffectRuntime::get().prepare(*fx);
    }
    EXPECT_FLOAT_EQ(fx->frame.pointerSmooth[2], 1.f) << "presence is full after 150 ms";
    // leaving: presence falls over 150 ms
    EffectRuntime::get().setTestPointer(context(), std::nullopt, false);
    t += 0.075;
    frameAt(t);
    EffectRuntime::get().prepare(*fx);
    EXPECT_NEAR(fx->frame.pointerSmooth[2], 0.5f, 1e-4);
    EXPECT_GT(fx->frame.pointerEx[3], 1e5f) << "since-enter is far away outside";
}

TEST_F(EffectRuntimeTest, TrailOrderingAgeingAndExpiry){
    double t = 3.0;
    frameAt(t);
    EffectInstance* fx = acquire("a", "cursor-trail");
    ASSERT_NE(fx, nullptr);
    // one sample a frame while the pointer moves (context (10 + 5k, 30) = local (5k, 20))
    for (int k = 0; k < 10; k++){
        t += 0.05;
        frameAt(t);
        EffectRuntime::get().onPointerMove(context(), Rml::Vector2f(10.f + 5.f * k, 30.f));
        EffectRuntime::get().prepare(*fx);
    }
    // newest first, in local px, aged by the frames since
    for (int i = 0; i < 10; i++){
        SCOPED_TRACE(i);
        EXPECT_NEAR(fx->trail[i][0], 5.f * (9 - i), 0.5f);
        EXPECT_NEAR(fx->trail[i][1], 20.f, 0.5f);
        EXPECT_NEAR(fx->trail[i][2], 0.05f * i, 1e-3f);
        EXPECT_FLOAT_EQ(fx->trail[i][3], 1.f);
    }
    EXPECT_FLOAT_EQ(fx->trail[10][3], 0.f);
    // a pointer that does not move (less than 1 px) adds nothing
    t += 0.05;
    frameAt(t);
    EffectRuntime::get().onPointerMove(context(), Rml::Vector2f(10.f + 45.f + 0.5f, 30.f));
    EffectRuntime::get().prepare(*fx);
    EXPECT_FLOAT_EQ(fx->trail[10][3], 0.f);
    EXPECT_NEAR(fx->trail[0][2], 0.05f, 1e-3f);
    // 32 samples at most
    for (int k = 0; k < 40; k++){
        t += 0.01;
        frameAt(t);
        EffectRuntime::get().onPointerMove(context(), Rml::Vector2f(10.f + 2.f * k, 40.f));
        EffectRuntime::get().prepare(*fx);
    }
    EXPECT_FLOAT_EQ(fx->trail[31][3], 1.f);
    EXPECT_NEAR(fx->trail[0][0], 78.f, 0.5f);
    // 4 s after the last move nothing is left
    for (int k = 0; k < 18; k++){
        t += 0.25;
        frameAt(t);
        EffectRuntime::get().prepare(*fx);
    }
    for (int i = 0; i < 32; i++)
        EXPECT_FLOAT_EQ(fx->trail[i][3], 0.f) << "sample " << i << " outlived 4 s";
    EffectRuntime::get().onPointerLeave(context());
}

TEST_F(EffectRuntimeTest, RipplesByPressAndMovement){
    std::vector<EffectDiagnostic> diags;
    ASSERT_TRUE(EffectRegistry::get().declareProjectEffect("ripple-test", "fixtures/effects/fx/ripple-test.frag", "test", 1, diags))
        << (diags.empty() ? "" : diags.front().message);
    double t = 5.0;
    frameAt(t);
    EffectInstance* press = acquire("a", "ripple-test");
    EffectInstance* move = acquire("b", "ripple-test, trigger move");
    ASSERT_NE(press, nullptr);
    ASSERT_NE(move, nullptr);
    EffectRuntime::get().prepare(*press);
    EffectRuntime::get().prepare(*move);

    // a press inside #a: one ripple at the press, strength 1, ageing with the frames
    t += 0.1;
    frameAt(t);
    EffectRuntime::get().onPointerButton(context(), Rml::Vector2f(40.f, 30.f), true);
    EffectRuntime::get().onPointerButton(context(), false);
    EffectRuntime::get().prepare(*press);
    EXPECT_NEAR(press->ripples[0][0], 30.f, 0.5f);
    EXPECT_NEAR(press->ripples[0][1], 20.f, 0.5f);
    EXPECT_NEAR(press->ripples[0][2], 0.f, 1e-3f);
    EXPECT_FLOAT_EQ(press->ripples[0][3], 1.f);
    EXPECT_FLOAT_EQ(press->ripples[1][3], 0.f);
    EXPECT_NEAR(press->frame.pointerEx[2], 0.f, 1e-3f) << "since-press";
    for (int k = 0; k < 5; k++){
        t += 0.1;
        frameAt(t);
        EffectRuntime::get().prepare(*press);
    }
    EXPECT_NEAR(press->ripples[0][2], 0.5f, 1e-3f);
    EXPECT_NEAR(press->frame.pointerEx[2], 0.5f, 1e-3f);
    // the press was outside #b: no ripple there
    EffectRuntime::get().prepare(*move);
    EXPECT_FLOAT_EQ(move->ripples[0][3], 0.f);

    // trigger move: a ripple (strength 0.6) every 40 px of movement inside #b
    for (int k = 0; k <= 10; k++){
        t += 0.02;
        frameAt(t);
        EffectRuntime::get().onPointerMove(context(), Rml::Vector2f(15.f + 9.f * k, 140.f));
        EffectRuntime::get().prepare(*press);
        EffectRuntime::get().prepare(*move);
    }
    int ripples = 0;
    for (int i = 0; i < 8; i++)
        ripples += move->ripples[i][3] > 0.f ? 1 : 0;
    EXPECT_EQ(ripples, 2) << "90 px of movement makes two ripples";
    EXPECT_FLOAT_EQ(move->ripples[0][3], 0.6f);
    EXPECT_EQ(press->ripples[1][3], 0.f) << "trigger press does not ripple on movement";
    // ripples expire after 4 s
    for (int k = 0; k < 18; k++){
        t += 0.25;
        frameAt(t);
        EffectRuntime::get().prepare(*press);
    }
    EXPECT_FLOAT_EQ(press->ripples[0][3], 0.f);
    EffectRuntime::get().onPointerLeave(context());
}

TEST_F(EffectRuntimeTest, SeedIsStablePerElement){
    Rml::Element* a = element("a");
    Rml::Element* b = element("b");
    const float seed = EffectRuntime::seedOf(a);
    EXPECT_EQ(seed, EffectRuntime::seedOf(a));
    EXPECT_GE(seed, 0.f);
    EXPECT_LT(seed, 1.f);
    EXPECT_NE(seed, EffectRuntime::seedOf(b));
    EffectClock::setFixedTime(1.0);
    EffectRuntime::get().advanceFrameForTests();
    EffectInstance* fx = acquire("a", "stripes");
    EffectRuntime::get().prepare(*fx);
    EXPECT_FLOAT_EQ(fx->frame.misc[2], seed);
    // the same element after another load of the same document keeps its seed (its address)
    std::string error;
    release(fx);
    ASSERT_TRUE(scene->loadHtml(kDocument, &error)) << error;
    EXPECT_EQ(EffectRuntime::seedOf(element("a")), seed);
}
