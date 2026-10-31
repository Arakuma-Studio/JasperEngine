// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The curve and gradient fields' pure parts (lines and trails spec 6.1, "Editor widgets"): the
// canvas mapping and zoom to fit, hit testing of keys, tangent handles and gradient markers, key
// insert / delete / move with re-sorting, the Auto / Linear / Constant / Free tangent modes
// against hand-computed slopes (no overshoot at an extremum), mirrored and broken tangent drags,
// the curve presets, and gradient keys: add, move, remove, the 8-key cap and the last-key rule.
// Pure.

#include <gtest/gtest.h>

#include "window/widget/CurveFieldLogic.h"
#include "window/widget/GradientFieldLogic.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::curvefield;
using namespace jasperengine::editor::gradientfield;

namespace {

    // 100 x 100 pixels at the origin showing [0, 1] x [0, 1]
    View unitView(){
        View view;
        view.canvas = Rect{0.0f, 0.0f, 100.0f, 100.0f};
        view.timeMin = 0.0f;
        view.timeMax = 1.0f;
        view.valueMin = 0.0f;
        view.valueMax = 1.0f;
        return view;
    }

    void expectSampledWithin(const AnimationCurve& curve, float t0, float t1, float lo, float hi){
        for (int i = 0; i <= 100; i++){
            const float t = t0 + (t1 - t0) * (float)i / 100.0f;
            const float v = curve.evaluate(t);
            EXPECT_GE(v, lo - 1e-5f) << "t = " << t;
            EXPECT_LE(v, hi + 1e-5f) << "t = " << t;
        }
    }

}

// ---- the view ----

TEST(CurveFieldLogicTest, ViewMapsTimeAndValueToPixels){
    View view;
    view.canvas = Rect{10.0f, 20.0f, 200.0f, 100.0f};
    view.timeMin = 0.0f;
    view.timeMax = 2.0f;
    view.valueMin = -1.0f;
    view.valueMax = 1.0f;
    EXPECT_FLOAT_EQ(view.timeToX(0.0f), 10.0f);
    EXPECT_FLOAT_EQ(view.timeToX(1.0f), 110.0f);
    EXPECT_FLOAT_EQ(view.valueToY(1.0f), 20.0f);     // top
    EXPECT_FLOAT_EQ(view.valueToY(-1.0f), 120.0f);   // bottom
    EXPECT_FLOAT_EQ(view.valueToY(0.0f), 70.0f);
    EXPECT_FLOAT_EQ(view.xToTime(160.0f), 1.5f);
    EXPECT_FLOAT_EQ(view.yToValue(45.0f), 0.5f);
    EXPECT_FLOAT_EQ(view.pixelsPerTime(), 100.0f);
    EXPECT_FLOAT_EQ(view.pixelsPerValue(), 50.0f);
}

TEST(CurveFieldLogicTest, FitViewCoversKeysAndValuesWithMargins){
    const Rect canvas{0.0f, 0.0f, 300.0f, 150.0f};
    const AnimationCurve line = AnimationCurve::linear(0.0f, 0.0f, 1.0f, 2.0f);
    const View view = fitView(line, canvas, nullptr, 0.05f, 0.1f);
    EXPECT_FLOAT_EQ(view.timeMin, -0.05f);
    EXPECT_FLOAT_EQ(view.timeMax, 1.05f);
    EXPECT_FLOAT_EQ(view.valueMin, -0.2f);
    EXPECT_FLOAT_EQ(view.valueMax, 2.2f);
    EXPECT_FLOAT_EQ(view.canvas.width, 300.0f);

    // a flat curve: a range of max(1, |v|) around its value; one key shows [0, 1]
    const View flat = fitView(AnimationCurve::constant(0.35f), canvas, nullptr, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(flat.timeMin, 0.0f);
    EXPECT_FLOAT_EQ(flat.timeMax, 1.0f);
    EXPECT_NEAR(flat.valueMin, -0.15f, 1e-6f);
    EXPECT_NEAR(flat.valueMax, 0.85f, 1e-6f);
    const View big = fitView(AnimationCurve::constant(10.0f), canvas, nullptr, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(big.valueMin, 5.0f);
    EXPECT_FLOAT_EQ(big.valueMax, 15.0f);

    // the sampled overshoot is in view: 4 s (1 - s) peaks at 1 between two keys at 0
    const AnimationCurve bump({CurveKey(0.0f, 0.0f, 4.0f, 4.0f), CurveKey(1.0f, 0.0f, -4.0f, -4.0f)});
    const View bumpView = fitView(bump, canvas, nullptr, 0.0f, 0.0f);
    EXPECT_NEAR(bumpView.valueMax, 1.0f, 1e-3f);
    EXPECT_FLOAT_EQ(bumpView.valueMin, 0.0f);

    // a second curve is fitted together
    const AnimationCurve low = AnimationCurve::linear(0.5f, -3.0f, 2.0f, -1.0f);
    const View both = fitView(line, canvas, &low, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(both.timeMin, 0.0f);
    EXPECT_FLOAT_EQ(both.timeMax, 2.0f);
    EXPECT_FLOAT_EQ(both.valueMin, -3.0f);
    EXPECT_FLOAT_EQ(both.valueMax, 2.0f);
}

TEST(CurveFieldLogicTest, ZoomKeepsTheAnchorAndPanFollowsTheMouse){
    const View view = unitView();
    const View zoomed = zoomView(view, Point{25.0f, 50.0f}, 2.0f, 1.0f);
    EXPECT_FLOAT_EQ(zoomed.timeMin, 0.125f);
    EXPECT_FLOAT_EQ(zoomed.timeMax, 0.625f);
    EXPECT_FLOAT_EQ(zoomed.valueMin, 0.0f);
    EXPECT_FLOAT_EQ(zoomed.valueMax, 1.0f);
    EXPECT_FLOAT_EQ(zoomed.xToTime(25.0f), 0.25f);

    const View panned = panView(view, 10.0f, 20.0f);   // right and down
    EXPECT_FLOAT_EQ(panned.timeMin, -0.1f);
    EXPECT_FLOAT_EQ(panned.valueMin, 0.2f);
    EXPECT_FLOAT_EQ(panned.valueMax, 1.2f);
}

// ---- hit testing ----

TEST(CurveFieldLogicTest, HitKeyFindsTheNearestWithinTheRadius){
    const View view = unitView();
    const AnimationCurve curve({CurveKey(0.0f, 0.0f), CurveKey(0.5f, 0.5f), CurveKey(0.55f, 0.5f), CurveKey(1.0f, 1.0f)});
    EXPECT_EQ(hitKey(curve, view, Point{1.0f, 99.0f}, 4.0f), 0);
    EXPECT_EQ(hitKey(curve, view, Point{51.0f, 50.0f}, 6.0f), 1);   // 1 px from key 1, 4 px from key 2
    EXPECT_EQ(hitKey(curve, view, Point{54.0f, 51.0f}, 6.0f), 2);
    EXPECT_EQ(hitKey(curve, view, Point{99.0f, 1.0f}, 4.0f), 3);
    EXPECT_EQ(hitKey(curve, view, Point{30.0f, 30.0f}, 4.0f), -1);
    EXPECT_EQ(hitKey(AnimationCurve(), view, Point{0.0f, 0.0f}, 100.0f), -1);
}

TEST(CurveFieldLogicTest, TangentHandlesAtAFixedPixelLength){
    const View view = unitView();
    const AnimationCurve curve({CurveKey(0.0f, 0.0f, 0.0f, 0.0f), CurveKey(0.5f, 0.5f, 1.0f, 1.0f),
                                CurveKey(1.0f, 1.0f, AnimationCurve::STEP, AnimationCurve::STEP)});
    const float h = 10.0f / std::sqrt(2.0f);

    // slope 1 on a square view: 45 degrees; In to the left
    const Point out = tangentHandlePosition(curve, 1, TangentSide::Out, view, 10.0f);
    EXPECT_NEAR(out.x, 50.0f + h, 1e-4f);
    EXPECT_NEAR(out.y, 50.0f - h, 1e-4f);
    const Point in = tangentHandlePosition(curve, 1, TangentSide::In, view, 10.0f);
    EXPECT_NEAR(in.x, 50.0f - h, 1e-4f);
    EXPECT_NEAR(in.y, 50.0f + h, 1e-4f);

    // on a view twice as wide per time unit the same slope is flatter on screen
    View wide = view;
    wide.canvas.width = 200.0f;
    const Point flatter = tangentHandlePosition(curve, 1, TangentSide::Out, wide, 10.0f);
    EXPECT_NEAR(flatter.x - 100.0f, 10.0f * 2.0f / std::sqrt(5.0f), 1e-4f);
    EXPECT_NEAR(50.0f - flatter.y, 10.0f * 1.0f / std::sqrt(5.0f), 1e-4f);

    TangentHit hit = hitTangentHandle(curve, view, 1, Point{out.x + 1.0f, out.y}, 4.0f, 10.0f);
    EXPECT_EQ(hit.key, 1);
    EXPECT_EQ(hit.side, TangentSide::Out);
    hit = hitTangentHandle(curve, view, 1, Point{in.x, in.y - 1.0f}, 4.0f, 10.0f);
    EXPECT_EQ(hit.key, 1);
    EXPECT_EQ(hit.side, TangentSide::In);
    EXPECT_EQ(hitTangentHandle(curve, view, 1, Point{80.0f, 80.0f}, 4.0f, 10.0f).key, -1);
    EXPECT_EQ(hitTangentHandle(curve, view, -1, out, 4.0f, 10.0f).key, -1);

    // the first key has no In handle, the last no Out handle, a step tangent none
    EXPECT_FALSE(hasTangentHandle(curve, 0, TangentSide::In));
    EXPECT_TRUE(hasTangentHandle(curve, 0, TangentSide::Out));
    EXPECT_FALSE(hasTangentHandle(curve, 2, TangentSide::In));
    EXPECT_FALSE(hasTangentHandle(curve, 2, TangentSide::Out));
    const Point first = keyPosition(view, curve.keys[0]);
    EXPECT_EQ(hitTangentHandle(curve, view, 0, Point{first.x - 10.0f, first.y}, 4.0f, 10.0f).key, -1);
    EXPECT_EQ(hitTangentHandle(curve, view, 0, Point{first.x + 10.0f, first.y}, 4.0f, 10.0f).side, TangentSide::Out);
}

// ---- tangent modes ----

TEST(CurveFieldLogicTest, AutoTangentIsClampedCatmullRom){
    // interior: (v2 - v0) / (t2 - t0) = 2 / 3 (within 3x the smaller secant 0.5)
    AnimationCurve curve({CurveKey(0.0f, 0.0f, 5.0f, 5.0f), CurveKey(1.0f, 1.0f, 5.0f, 5.0f), CurveKey(3.0f, 2.0f, 5.0f, 5.0f)});
    EXPECT_FLOAT_EQ(autoTangent(curve, 1), 2.0f / 3.0f);
    EXPECT_FLOAT_EQ(autoTangent(curve, 0), 0.0f);   // the ends are flat
    EXPECT_FLOAT_EQ(autoTangent(curve, 2), 0.0f);
    for (size_t i = 0; i < curve.keys.size(); i++) setAuto(curve, i);
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 2.0f / 3.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 2.0f / 3.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 0.0f);
    EXPECT_EQ(detectMode(curve, 1), TangentMode::Auto);
    EXPECT_EQ(detectMode(curve, 0), TangentMode::Auto);
    EXPECT_FALSE(isBroken(curve.keys[1]));

    // a local extremum is flat and nothing overshoots it
    AnimationCurve peak({CurveKey(0.0f, 0.0f), CurveKey(1.0f, 1.0f), CurveKey(2.0f, 0.0f)});
    for (size_t i = 0; i < peak.keys.size(); i++) setAuto(peak, i);
    EXPECT_FLOAT_EQ(peak.keys[1].inTangent, 0.0f);
    EXPECT_FLOAT_EQ(peak.keys[1].outTangent, 0.0f);
    expectSampledWithin(peak, 0.0f, 2.0f, 0.0f, 1.0f);

    // a steep side next to a shallow one: the slope (1 - 0) / 2 = 0.5 is limited to 3 x 0.1
    AnimationCurve knee({CurveKey(0.0f, 0.0f), CurveKey(1.0f, 0.1f), CurveKey(2.0f, 1.0f)});
    EXPECT_FLOAT_EQ(autoTangent(knee, 1), 0.3f);
    for (size_t i = 0; i < knee.keys.size(); i++) setAuto(knee, i);
    expectSampledWithin(knee, 0.0f, 1.0f, 0.0f, 0.1f);
    expectSampledWithin(knee, 1.0f, 2.0f, 0.1f, 1.0f);

    // one key or two keys: flat
    AnimationCurve one = AnimationCurve::constant(3.0f);
    one.keys[0].inTangent = one.keys[0].outTangent = 2.0f;
    setAuto(one, 0);
    EXPECT_FLOAT_EQ(one.keys[0].outTangent, 0.0f);
}

TEST(CurveFieldLogicTest, LinearTangentsAreTheNeighbouringSlopes){
    AnimationCurve curve({CurveKey(0.0f, 0.0f), CurveKey(1.0f, 2.0f), CurveKey(3.0f, 3.0f)});
    for (size_t i = 0; i < curve.keys.size(); i++) setLinear(curve, i);
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 2.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 0.5f);
    EXPECT_FLOAT_EQ(curve.keys[0].inTangent, 2.0f);   // an end takes its one slope on both sides
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 2.0f);
    EXPECT_FLOAT_EQ(curve.keys[2].inTangent, 0.5f);
    EXPECT_FLOAT_EQ(curve.keys[2].outTangent, 0.5f);
    EXPECT_EQ(detectMode(curve, 1), TangentMode::Linear);
    EXPECT_TRUE(isBroken(curve.keys[1]));
    // segments with linear tangents at both ends are straight
    EXPECT_FLOAT_EQ(curve.evaluate(0.5f), 1.0f);
    EXPECT_FLOAT_EQ(curve.evaluate(2.0f), 2.5f);
}

TEST(CurveFieldLogicTest, ConstantTangentsStepAndFreeKeepsTangents){
    AnimationCurve curve({CurveKey(0.0f, 0.0f), CurveKey(1.0f, 1.0f, 0.7f, 0.7f), CurveKey(2.0f, 3.0f)});
    setConstant(curve, 1);
    EXPECT_EQ(curve.keys[1].inTangent, AnimationCurve::STEP);
    EXPECT_EQ(curve.keys[1].outTangent, AnimationCurve::STEP);
    EXPECT_EQ(detectMode(curve, 1), TangentMode::Constant);
    EXPECT_FLOAT_EQ(curve.evaluate(0.5f), 0.0f);   // the segment before holds key 0
    EXPECT_FLOAT_EQ(curve.evaluate(1.5f), 1.0f);   // the segment after holds key 1
    EXPECT_FALSE(hasTangentHandle(curve, 1, TangentSide::In));

    AnimationCurve smooth({CurveKey(0.0f, 0.0f), CurveKey(1.0f, 1.0f, 0.7f, 0.7f), CurveKey(2.0f, 3.0f)});
    std::vector<char> broken;
    setFree(smooth, 1, broken);
    ASSERT_EQ(broken.size(), 3u);
    EXPECT_EQ(broken[1], 1);
    EXPECT_EQ(broken[0], 0);
    EXPECT_FLOAT_EQ(smooth.keys[1].inTangent, 0.7f);
    EXPECT_FLOAT_EQ(smooth.keys[1].outTangent, 0.7f);
    EXPECT_EQ(detectMode(smooth, 1), TangentMode::Free);

    setMode(smooth, 1, TangentMode::Auto);
    EXPECT_FLOAT_EQ(smooth.keys[1].outTangent, 1.5f);   // (3 - 0) / 2, within 3 x 1
    setMode(smooth, 1, TangentMode::Free);
    EXPECT_FLOAT_EQ(smooth.keys[1].outTangent, 1.5f);
}

TEST(CurveFieldLogicTest, TangentDragMirrorsUnlessBroken){
    const View view = unitView();
    AnimationCurve curve({CurveKey(0.0f, 0.0f), CurveKey(0.5f, 0.5f), CurveKey(1.0f, 1.0f)});

    // Out handle 10 px right, 10 px up: slope 1, mirrored to In
    EXPECT_TRUE(dragTangent(curve, 1, TangentSide::Out, Point{60.0f, 40.0f}, view, false));
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 1.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 1.0f);

    // broken: In handle 10 px left, 20 px up: coming down into the key, slope -2; Out kept
    EXPECT_TRUE(dragTangent(curve, 1, TangentSide::In, Point{40.0f, 30.0f}, view, true));
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, -2.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 1.0f);
    EXPECT_TRUE(isBroken(curve.keys[1]));

    // mirrored from In on a view with 200 px per time unit
    View wide = view;
    wide.canvas.width = 200.0f;   // the key is at x = 100
    EXPECT_TRUE(dragTangent(curve, 1, TangentSide::In, Point{80.0f, 55.0f}, wide, false));
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 0.5f);   // rise 0.05 over 0.1
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 0.5f);

    // dragged past the key's vertical: steep but finite, the sign from the rise
    EXPECT_TRUE(dragTangent(curve, 1, TangentSide::Out, Point{95.0f, 30.0f}, wide, false));
    EXPECT_TRUE(std::isfinite(curve.keys[1].outTangent));
    EXPECT_GT(curve.keys[1].outTangent, 50.0f);

    // a step key gets a finite slope back
    setConstant(curve, 1);
    EXPECT_TRUE(dragTangent(curve, 1, TangentSide::Out, Point{60.0f, 50.0f}, view, false));
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 0.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 0.0f);

    EXPECT_FALSE(dragTangent(curve, 7, TangentSide::Out, Point{60.0f, 50.0f}, view, false));
}

// ---- key edits ----

TEST(CurveFieldLogicTest, InsertKeyOnTheCurveWithAutoTangents){
    AnimationCurve curve = AnimationCurve::easeInOut(0.0f, 0.0f, 1.0f, 1.0f);
    const int index = insertKey(curve, 0.5f);
    ASSERT_EQ(index, 1);
    ASSERT_EQ(curve.keys.size(), 3u);
    EXPECT_FLOAT_EQ(curve.keys[1].time, 0.5f);
    EXPECT_FLOAT_EQ(curve.keys[1].value, 0.5f);   // the smoothstep's value there
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 1.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 1.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 0.0f);

    // sorted in; refused where a key already is
    EXPECT_EQ(insertKey(curve, 0.25f), 1);
    EXPECT_EQ(insertKey(curve, 2.0f), 4);
    EXPECT_FLOAT_EQ(curve.keys[4].value, 1.0f);   // past the end: the clamped value
    EXPECT_EQ(insertKey(curve, 0.5f), -1);
    EXPECT_EQ(curve.keys.size(), 5u);
    for (size_t i = 1; i < curve.keys.size(); i++) EXPECT_LT(curve.keys[i - 1].time, curve.keys[i].time);

    // the first key of an empty curve: value 0
    AnimationCurve empty;
    EXPECT_EQ(insertKey(empty, 0.3f), 0);
    EXPECT_FLOAT_EQ(empty.keys[0].value, 0.0f);
}

TEST(CurveFieldLogicTest, InsertAndDeleteRecomputeAutoNeighbours){
    // all Auto: the middle key's slope is 1 between (0, 0) and (2, 2)
    AnimationCurve curve({CurveKey(0.0f, 0.0f), CurveKey(1.0f, 1.0f, 1.0f, 1.0f), CurveKey(2.0f, 2.0f)});
    ASSERT_EQ(detectMode(curve, 1), TangentMode::Auto);
    // a key at (1.5, v) changes key 1's right neighbour: still Auto
    const int index = insertKey(curve, 1.5f);
    ASSERT_EQ(index, 2);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, autoTangent(curve, 1));
    EXPECT_EQ(detectMode(curve, 1), TangentMode::Auto);

    // a Free key keeps its tangents when its neighbours change
    AnimationCurve mixed({CurveKey(0.0f, 0.0f), CurveKey(1.0f, 1.0f, 5.0f, 5.0f), CurveKey(2.0f, 2.0f), CurveKey(3.0f, 0.0f)});
    ASSERT_TRUE(deleteKey(mixed, 2));
    EXPECT_FLOAT_EQ(mixed.keys[1].inTangent, 5.0f);
    EXPECT_FLOAT_EQ(mixed.keys[1].outTangent, 5.0f);
    ASSERT_EQ(mixed.keys.size(), 3u);
    EXPECT_FLOAT_EQ(mixed.keys[2].time, 3.0f);

    // all Auto (0,0) (1,1) (2,2) (3,1.5): deleting key 2 gives key 1 the neighbours (0, 0) and (3, 1.5)
    AnimationCurve autos({CurveKey(0.0f, 0.0f), CurveKey(1.0f, 1.0f), CurveKey(2.0f, 2.0f), CurveKey(3.0f, 1.5f)});
    for (size_t i = 0; i < autos.keys.size(); i++) setAuto(autos, i);
    EXPECT_FLOAT_EQ(autos.keys[1].outTangent, 1.0f);
    ASSERT_TRUE(deleteKey(autos, 2));
    // key 1 is now between (0, 0) and (3, 1.5): slope 0.5, secants 1 and 0.25 -> limit 0.75
    EXPECT_FLOAT_EQ(autos.keys[1].outTangent, 0.5f);

    // the only key stays; out of range refused
    AnimationCurve one = AnimationCurve::constant(1.0f);
    EXPECT_FALSE(deleteKey(one, 0));
    EXPECT_FALSE(deleteKey(autos, 9));
    EXPECT_EQ(one.keys.size(), 1u);
}

TEST(CurveFieldLogicTest, MoveKeyResortsAndKeepsTangents){
    AnimationCurve curve({CurveKey(0.0f, 0.0f, 3.0f, 3.0f), CurveKey(0.5f, 1.0f, 7.0f, 7.0f), CurveKey(1.0f, 0.0f, 9.0f, 9.0f)});
    // key 0 past key 1: it ends at index 1, its tangents with it
    int to = moveKey(curve, 0, 0.75f, 0.25f);
    ASSERT_EQ(to, 1);
    EXPECT_FLOAT_EQ(curve.keys[0].time, 0.5f);
    EXPECT_FLOAT_EQ(curve.keys[1].time, 0.75f);
    EXPECT_FLOAT_EQ(curve.keys[1].value, 0.25f);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 3.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 7.0f);

    // onto another key's time: only the value changes
    to = moveKey(curve, 1, 1.0f, 2.0f);
    EXPECT_EQ(to, 1);
    EXPECT_FLOAT_EQ(curve.keys[1].time, 0.75f);
    EXPECT_FLOAT_EQ(curve.keys[1].value, 2.0f);
    ASSERT_EQ(curve.keys.size(), 3u);

    // to the front
    to = moveKey(curve, 2, -1.0f, 4.0f);
    EXPECT_EQ(to, 0);
    EXPECT_FLOAT_EQ(curve.keys[0].time, -1.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 9.0f);

    EXPECT_EQ(moveKey(curve, 5, 0.0f, 0.0f), -1);
    EXPECT_EQ(moveKey(curve, 0, std::nanf(""), 0.0f), -1);

    // an Auto middle key follows its moved neighbour: (0,0) (1,1) (2,2) -> (2,4): slope 4 / 2 = 2
    AnimationCurve autos({CurveKey(0.0f, 0.0f), CurveKey(1.0f, 1.0f), CurveKey(2.0f, 2.0f)});
    for (size_t i = 0; i < autos.keys.size(); i++) setAuto(autos, i);
    EXPECT_EQ(moveKey(autos, 2, 2.0f, 4.0f), 2);
    EXPECT_FLOAT_EQ(autos.keys[1].inTangent, 2.0f);
    EXPECT_FLOAT_EQ(autos.keys[1].outTangent, 2.0f);
}

// ---- presets ----

TEST(CurveFieldLogicTest, PresetsOverTheUnitInterval){
    const AnimationCurve up = presetCurve(CurvePreset::LinearUp);
    EXPECT_FLOAT_EQ(up.evaluate(0.3f), 0.3f);
    EXPECT_FALSE(isBroken(up.keys[0]));
    const AnimationCurve down = presetCurve(CurvePreset::LinearDown);
    EXPECT_FLOAT_EQ(down.evaluate(0.3f), 0.7f);
    const AnimationCurve easeIn = presetCurve(CurvePreset::EaseIn);
    EXPECT_FLOAT_EQ(easeIn.evaluate(0.5f), 0.25f);   // t^2
    const AnimationCurve easeOut = presetCurve(CurvePreset::EaseOut);
    EXPECT_FLOAT_EQ(easeOut.evaluate(0.5f), 0.75f);  // 2t - t^2
    const AnimationCurve easeInOut = presetCurve(CurvePreset::EaseInOut);
    EXPECT_FLOAT_EQ(easeInOut.evaluate(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(easeInOut.evaluate(0.25f), 0.15625f);   // smoothstep
    const AnimationCurve constant = presetCurve(CurvePreset::Constant);
    EXPECT_TRUE(constant.isConstant());
    EXPECT_FLOAT_EQ(constant.evaluate(0.4f), 1.0f);
    for (int i = 0; i < CURVE_PRESET_COUNT; i++){
        const AnimationCurve c = presetCurve((CurvePreset)i);
        EXPECT_FLOAT_EQ(c.firstTime(), 0.0f);
        EXPECT_FLOAT_EQ(c.lastTime(), 1.0f);
        EXPECT_STRNE(presetName((CurvePreset)i), "");
    }

    // the current value range and wrap modes are kept
    AnimationCurve current = AnimationCurve::linear(0.0f, 2.0f, 4.0f, 5.0f);
    current.postWrap = CurveWrap::Loop;
    const AnimationCurve kept = presetCurve(CurvePreset::LinearDown, current);
    EXPECT_FLOAT_EQ(kept.evaluate(0.0f), 5.0f);
    EXPECT_FLOAT_EQ(kept.evaluate(1.0f), 2.0f);
    EXPECT_EQ(kept.postWrap, CurveWrap::Loop);
    EXPECT_FLOAT_EQ(presetCurve(CurvePreset::EaseIn, current).evaluate(0.5f), 2.75f);
    // a flat curve at v: from 0 to v; at 0: [0, 1]
    EXPECT_FLOAT_EQ(presetCurve(CurvePreset::LinearUp, AnimationCurve::constant(0.5f)).evaluate(1.0f), 0.5f);
    EXPECT_FLOAT_EQ(presetCurve(CurvePreset::LinearUp, AnimationCurve::constant(0.5f)).evaluate(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(presetCurve(CurvePreset::LinearUp, AnimationCurve::constant(0.0f)).evaluate(1.0f), 1.0f);
    EXPECT_FLOAT_EQ(presetCurve(CurvePreset::LinearUp, AnimationCurve::constant(-2.0f)).evaluate(0.0f), -2.0f);
}

// ---- the gradient field ----

TEST(GradientFieldLogicTest, HitMarkerFindsTheNearest){
    const std::vector<float> times = {0.0f, 0.5f, 0.6f, 1.0f};
    EXPECT_EQ(hitMarker(times, 10.0f, 100.0f, 12.0f, 5.0f), 0);
    EXPECT_EQ(hitMarker(times, 10.0f, 100.0f, 61.0f, 6.0f), 1);
    EXPECT_EQ(hitMarker(times, 10.0f, 100.0f, 68.0f, 6.0f), 2);
    EXPECT_EQ(hitMarker(times, 10.0f, 100.0f, 109.0f, 5.0f), 3);
    EXPECT_EQ(hitMarker(times, 10.0f, 100.0f, 35.0f, 5.0f), -1);
    // a tie goes to the later marker (drawn on top)
    EXPECT_EQ(hitMarker({0.25f, 0.75f}, 0.0f, 100.0f, 50.0f, 30.0f), 1);
    EXPECT_EQ(hitMarker({}, 0.0f, 100.0f, 50.0f, 30.0f), -1);

    EXPECT_FLOAT_EQ(timeAtX(10.0f, 100.0f, 35.0f), 0.25f);
    EXPECT_FLOAT_EQ(timeAtX(10.0f, 100.0f, -50.0f), 0.0f);
    EXPECT_FLOAT_EQ(timeAtX(10.0f, 100.0f, 500.0f), 1.0f);
    EXPECT_FLOAT_EQ(xAtTime(10.0f, 100.0f, 0.25f), 35.0f);
}

TEST(GradientFieldLogicTest, AddKeysTakeTheCurrentValueUpToEight){
    Gradient gradient = Gradient::twoColors(Vector4(0, 0, 0, 1), Vector4(1, 1, 1, 0));
    int index = addColorKey(gradient, 0.25f);
    ASSERT_EQ(index, 1);
    EXPECT_FLOAT_EQ(gradient.colorKeys[1].time, 0.25f);
    EXPECT_FLOAT_EQ(gradient.colorKeys[1].color.x, 0.25f);
    EXPECT_FLOAT_EQ(gradient.colorKeys[1].color.z, 0.25f);
    index = addAlphaKey(gradient, 0.75f);
    ASSERT_EQ(index, 1);
    EXPECT_FLOAT_EQ(gradient.alphaKeys[1].alpha, 0.25f);
    // the look is unchanged by a key added on it
    EXPECT_FLOAT_EQ(gradient.evaluate(0.5f).x, 0.5f);
    EXPECT_FLOAT_EQ(gradient.evaluate(0.5f).w, 0.5f);

    // clamped times; after the keys at the same time
    EXPECT_EQ(addColorKey(gradient, 1.5f), 3);
    EXPECT_FLOAT_EQ(gradient.colorKeys[3].time, 1.0f);
    EXPECT_EQ(addColorKey(gradient, 0.0f), 1);

    // eight keys: refused
    while (gradient.colorKeys.size() < Gradient::MAX_KEYS){
        ASSERT_GE(addColorKey(gradient, 0.5f), 0);
    }
    EXPECT_EQ(addColorKey(gradient, 0.3f), -1);
    EXPECT_EQ(gradient.colorKeys.size(), Gradient::MAX_KEYS);
    while (gradient.alphaKeys.size() < Gradient::MAX_KEYS){
        ASSERT_GE(addAlphaKey(gradient, 0.1f), 0);
    }
    EXPECT_EQ(addAlphaKey(gradient, 0.3f), -1);
    for (size_t i = 1; i < gradient.colorKeys.size(); i++) EXPECT_LE(gradient.colorKeys[i - 1].time, gradient.colorKeys[i].time);
    for (size_t i = 1; i < gradient.alphaKeys.size(); i++) EXPECT_LE(gradient.alphaKeys[i - 1].time, gradient.alphaKeys[i].time);

    // an empty list takes the fallback (white, alpha 1)
    Gradient empty({}, {});
    EXPECT_EQ(addColorKey(empty, 0.5f), 0);
    EXPECT_FLOAT_EQ(empty.colorKeys[0].color.y, 1.0f);
    EXPECT_EQ(addAlphaKey(empty, 0.5f), 0);
    EXPECT_FLOAT_EQ(empty.alphaKeys[0].alpha, 1.0f);
}

TEST(GradientFieldLogicTest, MoveKeysResort){
    Gradient gradient({GradientColorKey(0.0f, Vector3(1, 0, 0)), GradientColorKey(0.5f, Vector3(0, 1, 0)), GradientColorKey(1.0f, Vector3(0, 0, 1))},
                      {GradientAlphaKey(0.0f, 1.0f), GradientAlphaKey(1.0f, 0.0f)});
    int to = moveColorKey(gradient, 0, 0.75f);
    ASSERT_EQ(to, 1);
    EXPECT_FLOAT_EQ(gradient.colorKeys[0].time, 0.5f);
    EXPECT_FLOAT_EQ(gradient.colorKeys[1].time, 0.75f);
    EXPECT_FLOAT_EQ(gradient.colorKeys[1].color.x, 1.0f);   // the red key moved

    to = moveColorKey(gradient, 2, -3.0f);   // clamped to 0, to the front
    EXPECT_EQ(to, 0);
    EXPECT_FLOAT_EQ(gradient.colorKeys[0].time, 0.0f);
    EXPECT_FLOAT_EQ(gradient.colorKeys[0].color.z, 1.0f);

    to = moveAlphaKey(gradient, 1, 0.0f);   // onto the other key's time: after it
    EXPECT_EQ(to, 1);
    EXPECT_FLOAT_EQ(gradient.alphaKeys[1].alpha, 0.0f);
    to = moveAlphaKey(gradient, 0, 2.0f);
    EXPECT_EQ(to, 1);
    EXPECT_FLOAT_EQ(gradient.alphaKeys[1].time, 1.0f);
    EXPECT_FLOAT_EQ(gradient.alphaKeys[1].alpha, 1.0f);

    EXPECT_EQ(moveColorKey(gradient, 3, 0.5f), -1);
    EXPECT_EQ(moveAlphaKey(gradient, 2, 0.5f), -1);
}

TEST(GradientFieldLogicTest, RemoveKeepsTheLastKey){
    Gradient gradient;   // white, two keys per list
    EXPECT_TRUE(removeColorKey(gradient, 0));
    EXPECT_EQ(gradient.colorKeys.size(), 1u);
    EXPECT_FALSE(removeColorKey(gradient, 0));
    EXPECT_EQ(gradient.colorKeys.size(), 1u);
    EXPECT_FALSE(removeAlphaKey(gradient, 2));
    EXPECT_TRUE(removeAlphaKey(gradient, 1));
    EXPECT_FALSE(removeAlphaKey(gradient, 0));
    EXPECT_EQ(gradient.alphaKeys.size(), 1u);
}

TEST(GradientFieldLogicTest, Presets){
    const Gradient fadeOut = presetGradient(GradientPreset::FadeOut);
    EXPECT_FLOAT_EQ(fadeOut.evaluate(0.0f).w, 1.0f);
    EXPECT_FLOAT_EQ(fadeOut.evaluate(0.5f).w, 0.5f);
    EXPECT_FLOAT_EQ(fadeOut.evaluate(1.0f).w, 0.0f);
    EXPECT_FLOAT_EQ(fadeOut.evaluate(0.5f).x, 1.0f);
    EXPECT_FLOAT_EQ(presetGradient(GradientPreset::FadeIn).evaluate(0.25f).w, 0.25f);
    EXPECT_EQ(presetGradient(GradientPreset::White), Gradient());
    const Gradient bw = presetGradient(GradientPreset::BlackToWhite);
    EXPECT_FLOAT_EQ(bw.evaluate(0.5f).y, 0.5f);
    EXPECT_FLOAT_EQ(bw.evaluate(0.5f).w, 1.0f);
    EXPECT_FLOAT_EQ(presetGradient(GradientPreset::WhiteToBlack).evaluate(0.25f).y, 0.75f);
    const Gradient rainbow = presetGradient(GradientPreset::Rainbow);
    EXPECT_EQ(rainbow.colorKeys.size(), 6u);
    EXPECT_EQ(rainbow.evaluate(0.2f), Vector4(1, 1, 0, 1));
    for (int i = 0; i < GRADIENT_PRESET_COUNT; i++){
        Gradient g = presetGradient((GradientPreset)i);
        const Gradient copy = g;
        g.normalize();
        EXPECT_EQ(g, copy);   // already valid
        EXPECT_EQ(g.mode, GradientMode::Blend);
        EXPECT_STRNE(presetName((GradientPreset)i), "");
    }
}
