// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// origin/main's curve and gradient field logic tests (its window/widget CurveFieldLogic and
// GradientFieldLogic) on S27's editor: widget/CurveEditorOps (the view, framing, zoom and pan,
// hit testing, tangent handles and modes, key edits), CurvePresets and widget/GradientEditorOps
// with the Default swatch library's gradient presets (docs/specs/S27-editor-tools.md, Upstream
// absorption). Where S27 decides otherwise the test says so: an end key's Auto tangent is the
// slope to its neighbour (origin/main's was flat), a key moved onto another key's time keeps its
// own time.

#include <gtest/gtest.h>

#include "imgui.h"

#include "util/Color.h"
#include "widget/CurveEditorOps.h"
#include "widget/CurvePresets.h"
#include "widget/GradientEditorOps.h"
#include "widget/GradientField.h"
#include "widget/SwatchLibrary.h"

#include <cmath>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::curveops;

namespace {

    const float INF = std::numeric_limits<float>::infinity();

    // 100 x 100 pixels at the origin showing [0, 1] x [0, 1]
    View unitView(){
        View view;
        view.x0 = 0.0f; view.y0 = 0.0f; view.x1 = 100.0f; view.y1 = 100.0f;
        view.t0 = 0.0f; view.t1 = 1.0f; view.v0 = 0.0f; view.v1 = 1.0f;
        return view;
    }

    CurveKey key(float time, float value, float in = 0.0f, float out = 0.0f){
        CurveKey k;
        k.time = time;
        k.value = value;
        k.inTangent = in;
        k.outTangent = out;
        return k;
    }

    Curve curveOf(std::vector<CurveKey> keys){
        Curve curve;
        curve.keys = std::move(keys);
        return curve;
    }

    void expectSampledWithin(const Curve& curve, float t0, float t1, float lo, float hi){
        for (int i = 0; i <= 100; i++){
            const float t = t0 + (t1 - t0) * (float)i / 100.0f;
            const float v = curve.evaluate(t);
            EXPECT_GE(v, lo - 1e-5f) << "t = " << t;
            EXPECT_LE(v, hi + 1e-5f) << "t = " << t;
        }
    }

    Hit hitOf(const Curve& curve, const View& view, float x, float y, float radius, const std::vector<KeyRef>& selection = {}){
        return hitTest({&curve}, {true}, view, x, y, radius, 10.0f, selection);
    }

    const GradientPreset* presetNamed(const SwatchLibrary& library, const std::string& name){
        for (const GradientPreset& p : library.gradients){
            if (p.name == name) return &p;
        }
        return nullptr;
    }

}

// ---- the view ----

TEST(CurveFieldLogicTest, ViewMapsTimeAndValueToPixels){
    View view;
    view.x0 = 10.0f; view.y0 = 20.0f; view.x1 = 210.0f; view.y1 = 120.0f;
    view.t0 = 0.0f; view.t1 = 2.0f; view.v0 = -1.0f; view.v1 = 1.0f;
    EXPECT_FLOAT_EQ(view.toX(0.0f), 10.0f);
    EXPECT_FLOAT_EQ(view.toX(1.0f), 110.0f);
    EXPECT_FLOAT_EQ(view.toY(1.0f), 20.0f);     // top
    EXPECT_FLOAT_EQ(view.toY(-1.0f), 120.0f);   // bottom
    EXPECT_FLOAT_EQ(view.toY(0.0f), 70.0f);
    EXPECT_FLOAT_EQ(view.toTime(160.0f), 1.5f);
    EXPECT_FLOAT_EQ(view.toValue(45.0f), 0.5f);
    EXPECT_FLOAT_EQ(view.scaleX(), 100.0f);
    EXPECT_FLOAT_EQ(view.scaleY(), 50.0f);
}

TEST(CurveFieldLogicTest, FrameCoversKeysAndValuesWithMargins){
    View pixels = unitView();
    pixels.x1 = 300.0f;
    pixels.y1 = 150.0f;
    const Curve line = Curve::linear(0.0f, 0.0f, 1.0f, 2.0f);
    const View view = frame({&line}, {}, pixels, 0.1f, 0.0f);
    EXPECT_FLOAT_EQ(view.t0, -0.1f);
    EXPECT_FLOAT_EQ(view.t1, 1.1f);
    EXPECT_FLOAT_EQ(view.v0, -0.2f);
    EXPECT_FLOAT_EQ(view.v1, 2.2f);
    EXPECT_FLOAT_EQ(view.x1, 300.0f) << "the pixels kept";

    // a flat curve: at least minSpan around its value; one key the same
    const Curve flat = Curve::constant(0.35f);
    const View flatView = frame({&flat}, {}, pixels, 0.0f, 1.0f);
    EXPECT_NEAR(flatView.v0, -0.15f, 1e-6f);
    EXPECT_NEAR(flatView.v1, 0.85f, 1e-6f);

    // the overshoot between keys is in view: 4 s (1 - s) peaks at 1 between two keys at 0
    const Curve bump = curveOf({key(0.0f, 0.0f, 4.0f, 4.0f), key(1.0f, 0.0f, -4.0f, -4.0f)});
    const View bumpView = frame({&bump}, {}, pixels, 0.0f, 0.0f);
    EXPECT_NEAR(bumpView.v1, 1.0f, 1e-2f);
    EXPECT_FLOAT_EQ(bumpView.v0, 0.0f);

    // two curves are framed together
    const Curve low = Curve::linear(0.5f, -3.0f, 2.0f, -1.0f);
    const View both = frame({&line, &low}, {}, pixels, 0.0f, 0.0f);
    EXPECT_FLOAT_EQ(both.t0, 0.0f);
    EXPECT_FLOAT_EQ(both.t1, 2.0f);
    EXPECT_FLOAT_EQ(both.v0, -3.0f);
    EXPECT_FLOAT_EQ(both.v1, 2.0f);
}

TEST(CurveFieldLogicTest, ZoomKeepsTheAnchorAndPanFollowsTheMouse){
    const View view = unitView();
    // time spans halved around x = 25 (t 0.25), values kept
    const View zoomed = zoom(view, 25.0f, 50.0f, 0.5f, 1.0f);
    EXPECT_FLOAT_EQ(zoomed.t0, 0.125f);
    EXPECT_FLOAT_EQ(zoomed.t1, 0.625f);
    EXPECT_FLOAT_EQ(zoomed.v0, 0.0f);
    EXPECT_FLOAT_EQ(zoomed.v1, 1.0f);
    EXPECT_FLOAT_EQ(zoomed.toTime(25.0f), 0.25f);

    const View panned = pan(view, 10.0f, 20.0f);   // right and down: the graph follows
    EXPECT_FLOAT_EQ(panned.t0, -0.1f);
    EXPECT_FLOAT_EQ(panned.v0, 0.2f);
    EXPECT_FLOAT_EQ(panned.v1, 1.2f);
}

// ---- hit testing ----

TEST(CurveFieldLogicTest, HitKeyFindsTheNearestWithinTheRadius){
    const View view = unitView();
    const Curve curve = curveOf({key(0.0f, 0.0f), key(0.5f, 0.5f), key(0.55f, 0.5f), key(1.0f, 1.0f)});
    EXPECT_EQ(hitOf(curve, view, 1.0f, 99.0f, 4.0f).key, 0);
    EXPECT_EQ(hitOf(curve, view, 51.0f, 50.0f, 6.0f).key, 1);   // 1 px from key 1, 4 px from key 2
    EXPECT_EQ(hitOf(curve, view, 54.0f, 51.0f, 6.0f).key, 2);
    EXPECT_EQ(hitOf(curve, view, 99.0f, 1.0f, 4.0f).key, 3);
    EXPECT_EQ(hitOf(curve, view, 1.0f, 99.0f, 4.0f).kind, Hit::Kind::Key);
    const Hit miss = hitOf(curve, view, 30.0f, 10.0f, 4.0f);
    EXPECT_EQ(miss.kind, Hit::Kind::None);
    EXPECT_EQ(hitOf(Curve(), view, 0.0f, 0.0f, 100.0f).kind, Hit::Kind::None);
}

TEST(CurveFieldLogicTest, TangentHandlesAtAFixedPixelLength){
    const View view = unitView();
    const Curve curve = curveOf({key(0.0f, 0.0f), key(0.5f, 0.5f, 1.0f, 1.0f), key(1.0f, 1.0f, INF, INF)});
    const float h = 10.0f / std::sqrt(2.0f);

    // slope 1 on a square view: 45 degrees; In to the left
    float ox, oy, ix, iy;
    handlePosition(curve.keys[1], Side::Out, view, 10.0f, ox, oy);
    EXPECT_NEAR(ox, 50.0f + h, 1e-4f);
    EXPECT_NEAR(oy, 50.0f - h, 1e-4f);
    handlePosition(curve.keys[1], Side::In, view, 10.0f, ix, iy);
    EXPECT_NEAR(ix, 50.0f - h, 1e-4f);
    EXPECT_NEAR(iy, 50.0f + h, 1e-4f);

    // on a view twice as wide per time unit the same slope is flatter on screen
    View wide = view;
    wide.x1 = 200.0f;
    float fx, fy;
    handlePosition(curve.keys[1], Side::Out, wide, 10.0f, fx, fy);
    EXPECT_NEAR(fx - 100.0f, 10.0f * 2.0f / std::sqrt(5.0f), 1e-4f);
    EXPECT_NEAR(50.0f - fy, 10.0f * 1.0f / std::sqrt(5.0f), 1e-4f);
    // a step's handle lies flat
    float sx, sy;
    handlePosition(curve.keys[2], Side::In, view, 10.0f, sx, sy);
    EXPECT_NEAR(sy, 0.0f, 1e-4f);

    // a selected key's handles are hit first; the first key has no In handle
    const std::vector<KeyRef> selected = {{0, 1}};
    EXPECT_EQ(hitOf(curve, view, ox + 1.0f, oy, 4.0f, selected).kind, Hit::Kind::OutHandle);
    EXPECT_EQ(hitOf(curve, view, ix, iy - 1.0f, 4.0f, selected).kind, Hit::Kind::InHandle);
    EXPECT_NE(hitOf(curve, view, ox + 1.0f, oy, 4.0f).kind, Hit::Kind::OutHandle) << "only a selected key's handles";
    const std::vector<KeyRef> first = {{0, 0}};
    EXPECT_NE(hitOf(curve, view, -10.0f, 100.0f, 4.0f, first).kind, Hit::Kind::InHandle);
    EXPECT_EQ(hitOf(curve, view, 10.0f, 100.0f, 4.0f, first).kind, Hit::Kind::OutHandle);
}

// ---- tangent modes ----

TEST(CurveFieldLogicTest, ClampedAutoIsMonotoneAndAutoIsCatmullRom){
    // interior Auto: the mean of the neighbouring slopes (1 and 0.5): 0.75
    Curve curve = curveOf({key(0.0f, 0.0f, 5.0f, 5.0f), key(1.0f, 1.0f, 5.0f, 5.0f), key(3.0f, 2.0f, 5.0f, 5.0f)});
    for (size_t i = 0; i < curve.keys.size(); i++) setAuto(curve, i);
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 0.75f);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 0.75f);
    // S27: an end key's Auto is the slope to its neighbour
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 1.0f);
    EXPECT_EQ(curve.keys[1].inMode(), CurveTangentMode::Auto);
    EXPECT_FALSE(curve.keys[1].broken());

    // Clamped Auto: a local extremum is flat and nothing overshoots it
    Curve peak = curveOf({key(0.0f, 0.0f), key(1.0f, 1.0f), key(2.0f, 0.0f)});
    for (size_t i = 0; i < peak.keys.size(); i++) setClampedAuto(peak, i);
    EXPECT_FLOAT_EQ(peak.keys[1].inTangent, 0.0f);
    EXPECT_FLOAT_EQ(peak.keys[1].outTangent, 0.0f);
    expectSampledWithin(peak, 0.0f, 2.0f, 0.0f, 1.0f);

    // a steep side next to a shallow one: the mean 0.5 limited to 3 x 0.1
    Curve knee = curveOf({key(0.0f, 0.0f), key(1.0f, 0.1f), key(2.0f, 1.0f)});
    for (size_t i = 0; i < knee.keys.size(); i++) setClampedAuto(knee, i);
    EXPECT_NEAR(knee.keys[1].outTangent, 0.3f, 1e-6f);
    expectSampledWithin(knee, 1.0f, 2.0f, 0.1f, 1.0f);

    // one key: flat
    Curve one = Curve::constant(3.0f);
    one.keys[0].inTangent = one.keys[0].outTangent = 2.0f;
    setAuto(one, 0);
    EXPECT_FLOAT_EQ(one.keys[0].outTangent, 0.0f);
}

TEST(CurveFieldLogicTest, LinearTangentsAreTheNeighbouringSlopes){
    Curve curve = curveOf({key(0.0f, 0.0f), key(1.0f, 2.0f), key(3.0f, 3.0f)});
    for (size_t i = 0; i < curve.keys.size(); i++) setTangentMode(curve, i, Side::Both, CurveTangentMode::Linear);
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 2.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 0.5f);
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 2.0f);
    EXPECT_FLOAT_EQ(curve.keys[2].inTangent, 0.5f);
    EXPECT_EQ(curve.keys[1].inMode(), CurveTangentMode::Linear);
    // segments with linear tangents at both ends are straight
    EXPECT_FLOAT_EQ(curve.evaluate(0.5f), 1.0f);
    EXPECT_FLOAT_EQ(curve.evaluate(2.0f), 2.5f);
}

TEST(CurveFieldLogicTest, ConstantTangentsStepAndFreeKeepsTangents){
    Curve curve = curveOf({key(0.0f, 0.0f), key(1.0f, 1.0f, 0.7f, 0.7f), key(2.0f, 3.0f)});
    setTangentMode(curve, 1, Side::Both, CurveTangentMode::Constant);
    EXPECT_TRUE(std::isinf(curve.keys[1].inTangent));
    EXPECT_TRUE(std::isinf(curve.keys[1].outTangent));
    EXPECT_FLOAT_EQ(curve.evaluate(0.5f), 0.0f);   // the segment before holds key 0
    EXPECT_FLOAT_EQ(curve.evaluate(1.5f), 1.0f);   // the segment after holds key 1

    Curve smooth = curveOf({key(0.0f, 0.0f), key(1.0f, 1.0f, 0.7f, 0.7f), key(2.0f, 3.0f)});
    setBroken(smooth, 1, true);
    EXPECT_TRUE(smooth.keys[1].broken());
    EXPECT_FALSE(smooth.keys[0].broken());
    EXPECT_FLOAT_EQ(smooth.keys[1].inTangent, 0.7f);
    EXPECT_FLOAT_EQ(smooth.keys[1].outTangent, 0.7f);
    EXPECT_EQ(smooth.keys[1].outMode(), CurveTangentMode::Free);

    setAuto(smooth, 1);
    EXPECT_FLOAT_EQ(smooth.keys[1].outTangent, 1.5f);   // the mean of 1 and 2
    setFreeSmooth(smooth, 1);
    EXPECT_FLOAT_EQ(smooth.keys[1].outTangent, 1.5f);   // Free keeps it
    EXPECT_FALSE(smooth.keys[1].broken());
}

TEST(CurveFieldLogicTest, TangentDragMirrorsUnlessBroken){
    const View view = unitView();
    Curve curve = curveOf({key(0.0f, 0.0f), key(0.5f, 0.5f), key(1.0f, 1.0f)});

    // Out handle 10 px right, 10 px up: slope 1, mirrored to In
    dragTangent(curve, 1, Side::Out, slopeTowards(curve.keys[1], Side::Out, view, 60.0f, 40.0f));
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 1.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 1.0f);

    // broken: In handle 10 px left, 20 px up: coming down into the key, slope -2; Out kept
    setBroken(curve, 1, true);
    dragTangent(curve, 1, Side::In, slopeTowards(curve.keys[1], Side::In, view, 40.0f, 30.0f));
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, -2.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 1.0f);
    EXPECT_TRUE(curve.keys[1].broken());

    // dragged past the key's vertical: steep but finite, on its own side
    const float steep = slopeTowards(curve.keys[1], Side::Out, view, 45.0f, 30.0f);
    EXPECT_TRUE(std::isfinite(steep));
    EXPECT_GT(steep, 50.0f);

    // a step key gets a finite slope back
    setTangentMode(curve, 1, Side::Both, CurveTangentMode::Constant);
    setBroken(curve, 1, false);
    dragTangent(curve, 1, Side::Out, slopeTowards(curve.keys[1], Side::Out, view, 60.0f, 50.0f));
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 0.0f);
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 0.0f);
    // a non-finite slope or no such key: nothing
    dragTangent(curve, 1, Side::Out, INF);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 0.0f);
    dragTangent(curve, 7, Side::Out, 1.0f);
    EXPECT_EQ(curve.keys.size(), 3u);
}

// ---- key edits ----

TEST(CurveFieldLogicTest, InsertKeyOnTheCurveWithAutoTangents){
    Curve curve = Curve::easeInOut(0.0f, 0.0f, 1.0f, 1.0f);
    const int index = addKeyOnCurve(curve, 0.5f, nullptr);
    ASSERT_EQ(index, 1);
    ASSERT_EQ(curve.keys.size(), 3u);
    EXPECT_FLOAT_EQ(curve.keys[1].time, 0.5f);
    EXPECT_FLOAT_EQ(curve.keys[1].value, 0.5f);   // the smoothstep's value there
    EXPECT_EQ(curve.keys[1].inMode(), CurveTangentMode::ClampedAuto);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 1.0f);   // the mean of the slopes to its neighbours
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 0.0f) << "a Free neighbour keeps its tangent";

    // sorted in; refused where a key already is
    EXPECT_EQ(addKeyOnCurve(curve, 0.25f, nullptr), 1);
    EXPECT_EQ(addKeyOnCurve(curve, 2.0f, nullptr), 4);
    EXPECT_FLOAT_EQ(curve.keys[4].value, 1.0f);   // past the end: the clamped value
    EXPECT_EQ(addKeyOnCurve(curve, 0.5f, nullptr), -1);
    EXPECT_EQ(curve.keys.size(), 5u);
    for (size_t i = 1; i < curve.keys.size(); i++) EXPECT_LT(curve.keys[i - 1].time, curve.keys[i].time);

    // the first key of an empty curve: value 0; a range clamps it
    Curve empty;
    EXPECT_EQ(addKeyOnCurve(empty, 0.3f, nullptr), 0);
    EXPECT_FLOAT_EQ(empty.keys[0].value, 0.0f);
    CurveRange range;
    range.clamp = true;
    Curve ranged;
    EXPECT_EQ(addKeyAt(ranged, 2.0f, -1.0f, &range), 0);
    EXPECT_FLOAT_EQ(ranged.keys[0].time, 1.0f);
    EXPECT_FLOAT_EQ(ranged.keys[0].value, 0.0f);
}

TEST(CurveFieldLogicTest, InsertAndDeleteRecomputeAutoNeighbours){
    // Clamped Auto neighbours follow a key added next to them
    Curve curve = curveOf({key(0.0f, 0.0f), key(1.0f, 1.0f), key(2.0f, 2.0f)});
    for (size_t i = 0; i < curve.keys.size(); i++) setClampedAuto(curve, i);
    ASSERT_FLOAT_EQ(curve.keys[1].outTangent, 1.0f);
    ASSERT_EQ(addKeyAt(curve, 1.5f, 3.0f, nullptr), 2);
    // key 1 between (0, 0) and (1.5, 3): slopes 1 and 4, mean 2.5 within 3 x 1
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 2.5f);

    // a Free key keeps its tangents when its neighbours change
    std::vector<KeyRef> selection = {{0, 2}};
    Curve mixed = curveOf({key(0.0f, 0.0f), key(1.0f, 1.0f, 5.0f, 5.0f), key(2.0f, 2.0f), key(3.0f, 0.0f)});
    std::vector<Curve*> curves = {&mixed};
    deleteKeys(curves, selection);
    ASSERT_EQ(mixed.keys.size(), 3u);
    EXPECT_FLOAT_EQ(mixed.keys[1].inTangent, 5.0f);
    EXPECT_FLOAT_EQ(mixed.keys[1].outTangent, 5.0f);
    EXPECT_FLOAT_EQ(mixed.keys[2].time, 3.0f);
    EXPECT_TRUE(selection.empty());

    // all Clamped Auto (0,0) (1,1) (2,2) (3,1.5): deleting key 2 gives key 1 the neighbours
    // (0, 0) and (3, 1.5): slopes 1 and 0.25, mean 0.625 within 3 x 0.25
    Curve autos = curveOf({key(0.0f, 0.0f), key(1.0f, 1.0f), key(2.0f, 2.0f), key(3.0f, 1.5f)});
    for (size_t i = 0; i < autos.keys.size(); i++) setClampedAuto(autos, i);
    selection = {{0, 2}};
    curves = {&autos};
    deleteKeys(curves, selection);
    EXPECT_FLOAT_EQ(autos.keys[1].outTangent, 0.625f);

    // the only key stays
    Curve one = Curve::constant(1.0f);
    curves = {&one};
    selection = {{0, 0}};
    deleteKeys(curves, selection);
    EXPECT_EQ(one.keys.size(), 1u);
}

TEST(CurveFieldLogicTest, MoveKeyResortsAndKeepsTangents){
    Curve curve = curveOf({key(0.0f, 0.0f, 3.0f, 3.0f), key(0.5f, 1.0f, 7.0f, 7.0f), key(1.0f, 0.0f, 9.0f, 9.0f)});
    const std::vector<Curve> original = {curve};
    std::vector<Curve*> curves = {&curve};
    // key 0 past key 1: it ends at index 1, its tangents with it, the selection following
    std::vector<KeyRef> selection = {{0, 0}};
    moveKeys(curves, original, selection, 0.75f, 0.25f, nullptr);
    ASSERT_EQ(selection.size(), 1u);
    EXPECT_EQ(selection[0].key, 1);
    EXPECT_FLOAT_EQ(curve.keys[0].time, 0.5f);
    EXPECT_FLOAT_EQ(curve.keys[1].time, 0.75f);
    EXPECT_FLOAT_EQ(curve.keys[1].value, 0.25f);
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, 3.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 7.0f);

    // onto another key's time: only the value changes (S27 keeps the key's own time)
    const std::vector<Curve> before = {curve};
    selection = {{0, 1}};
    moveKeys(curves, before, selection, 0.25f, 1.75f, nullptr);
    EXPECT_EQ(selection[0].key, 1);
    EXPECT_FLOAT_EQ(curve.keys[1].time, 0.75f);
    EXPECT_FLOAT_EQ(curve.keys[1].value, 2.0f);
    ASSERT_EQ(curve.keys.size(), 3u);

    // to the front
    const std::vector<Curve> again = {curve};
    selection = {{0, 2}};
    moveKeys(curves, again, selection, -2.0f, 4.0f, nullptr);
    EXPECT_EQ(selection[0].key, 0);
    EXPECT_FLOAT_EQ(curve.keys[0].time, -1.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].outTangent, 9.0f);

    // a Clamped Auto middle key follows its moved neighbour: (0,0) (1,1) (2,2) -> (2,4)
    Curve autos = curveOf({key(0.0f, 0.0f), key(1.0f, 1.0f), key(2.0f, 2.0f)});
    for (size_t i = 0; i < autos.keys.size(); i++) setClampedAuto(autos, i);
    const std::vector<Curve> start = {autos};
    std::vector<Curve*> autoCurves = {&autos};
    selection = {{0, 2}};
    moveKeys(autoCurves, start, selection, 0.0f, 2.0f, nullptr);
    // slopes 1 and 3: mean 2 within 3 x 1
    EXPECT_FLOAT_EQ(autos.keys[1].inTangent, 2.0f);
    EXPECT_FLOAT_EQ(autos.keys[1].outTangent, 2.0f);
}

// ---- presets ----

TEST(CurveFieldLogicTest, PresetsOverTheUnitInterval){
    // S27's built-in presets cover origin/main's (linear up and down, ease in, out, in-out, constant)
    const CurvePresetLibrary& library = curvepresets::builtIn();
    ASSERT_GE(library.presets.size(), 6u);
    for (const CurvePreset& preset : library.presets){
        SCOPED_TRACE(preset.name);
        ASSERT_FALSE(preset.curve.keys.empty());
        EXPECT_FLOAT_EQ(preset.curve.firstTime(), 0.0f);
        EXPECT_FLOAT_EQ(preset.curve.lastTime(), 1.0f);
        EXPECT_FALSE(preset.name.empty());
    }
    auto named = [&](const std::string& name) -> const Curve& {
        for (const CurvePreset& preset : library.presets){
            if (preset.name == name) return preset.curve;
        }
        static const Curve none;
        ADD_FAILURE() << "no preset " << name;
        return none;
    };
    EXPECT_FLOAT_EQ(named("Linear").evaluate(0.3f), 0.3f);
    EXPECT_FLOAT_EQ(named("Linear Down").evaluate(0.3f), 0.7f);
    EXPECT_FLOAT_EQ(named("Ease In").evaluate(0.5f), 0.25f);    // t^2
    EXPECT_FLOAT_EQ(named("Ease Out").evaluate(0.5f), 0.75f);   // 2t - t^2
    EXPECT_FLOAT_EQ(named("Ease In Out").evaluate(0.5f), 0.5f);
    EXPECT_FLOAT_EQ(named("Ease In Out").evaluate(0.25f), 0.15625f);   // smoothstep
    EXPECT_TRUE(named("Constant").isConstant());
    EXPECT_FLOAT_EQ(named("Constant").evaluate(0.4f), 1.0f);

    // a preset maps onto the field's range, and back
    CurveRange range;
    range.t0 = 0.0f; range.t1 = 4.0f; range.v0 = 2.0f; range.v1 = 5.0f;
    const Curve down = remap(Curve::linear(0.0f, 1.0f, 1.0f, 0.0f), range);
    EXPECT_FLOAT_EQ(down.evaluate(0.0f), 5.0f);
    EXPECT_FLOAT_EQ(down.evaluate(4.0f), 2.0f);
    const Curve back = normalise(down, range);
    EXPECT_NEAR(back.evaluate(0.5f), 0.5f, 1e-5f);
}

// ---- the gradient field ----

TEST(GradientFieldLogicTest, HitMarkerFindsTheNearest){
    using namespace gradientops;
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
    const Gradient g = Gradient::twoColors(Vector3(0, 0, 0), Vector3(1, 1, 1));
    EXPECT_EQ(colorTimes(g), std::vector<float>({0.0f, 1.0f}));
    EXPECT_EQ(alphaTimes(g), std::vector<float>({0.0f, 1.0f}));
}

TEST(GradientFieldLogicTest, AddKeysTakeTheCurrentValueUpToEight){
    using namespace gradientops;
    Gradient gradient = Gradient::twoColors(Vector3(0, 0, 0), Vector3(1, 1, 1), 1.0f, 0.0f);
    int index = addColorKey(gradient, 0.25f);
    ASSERT_EQ(index, 1);
    EXPECT_FLOAT_EQ(gradient.colorKeys[1].time, 0.25f);
    // the colour there (sRGB keys, blended in sRGB: a quarter of the way is sRGB 0.25)
    EXPECT_NEAR(gradient.colorKeys[1].color.x, 0.25f, 1e-5f);
    EXPECT_NEAR(gradient.colorKeys[1].color.z, 0.25f, 1e-5f);
    index = addAlphaKey(gradient, 0.75f);
    ASSERT_EQ(index, 1);
    EXPECT_FLOAT_EQ(gradient.alphaKeys[1].alpha, 0.25f);
    // the look is unchanged by a key added on it
    EXPECT_NEAR(gradient.evaluate(0.5f).x, Color::sRGBToLinear(Vector3(0.5f, 0.5f, 0.5f)).x, 1e-5f);
    EXPECT_FLOAT_EQ(gradient.evaluate(0.5f).w, 0.5f);

    // clamped times; after the keys at the same time
    EXPECT_EQ(addColorKey(gradient, 1.5f), 3);
    EXPECT_FLOAT_EQ(gradient.colorKeys[3].time, 1.0f);
    EXPECT_EQ(addColorKey(gradient, 0.0f), 1);

    // eight keys: refused
    while (gradient.colorKeys.size() < Gradient::kMaxKeys) ASSERT_GE(addColorKey(gradient, 0.5f), 0);
    EXPECT_EQ(addColorKey(gradient, 0.3f), -1);
    EXPECT_EQ(gradient.colorKeys.size(), Gradient::kMaxKeys);
    while (gradient.alphaKeys.size() < Gradient::kMaxKeys) ASSERT_GE(addAlphaKey(gradient, 0.1f), 0);
    EXPECT_EQ(addAlphaKey(gradient, 0.3f), -1);
    for (size_t i = 1; i < gradient.colorKeys.size(); i++) EXPECT_LE(gradient.colorKeys[i - 1].time, gradient.colorKeys[i].time);
    for (size_t i = 1; i < gradient.alphaKeys.size(); i++) EXPECT_LE(gradient.alphaKeys[i - 1].time, gradient.alphaKeys[i].time);

    // an empty list takes the fallback (white, alpha 1)
    Gradient empty;
    EXPECT_EQ(addColorKey(empty, 0.5f), 0);
    EXPECT_FLOAT_EQ(empty.colorKeys[0].color.y, 1.0f);
    EXPECT_EQ(addAlphaKey(empty, 0.5f), 0);
    EXPECT_FLOAT_EQ(empty.alphaKeys[0].alpha, 1.0f);
}

TEST(GradientFieldLogicTest, MoveKeysResort){
    using namespace gradientops;
    Gradient gradient;
    gradient.colorKeys = {{0.0f, Vector3(1, 0, 0)}, {0.5f, Vector3(0, 1, 0)}, {1.0f, Vector3(0, 0, 1)}};
    gradient.alphaKeys = {{0.0f, 1.0f}, {1.0f, 0.0f}};
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
    using namespace gradientops;
    Gradient gradient = Gradient::twoColors(Vector3(1, 1, 1), Vector3(1, 1, 1));
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
    // origin/main's gradient presets, in the Default swatch library
    const SwatchLibrary library = swatches::defaultLibrary();
    const GradientPreset* fadeOut = presetNamed(library, "Fade Out");
    ASSERT_NE(fadeOut, nullptr);
    EXPECT_FLOAT_EQ(fadeOut->gradient.evaluate(0.0f).w, 1.0f);
    EXPECT_FLOAT_EQ(fadeOut->gradient.evaluate(0.5f).w, 0.5f);
    EXPECT_FLOAT_EQ(fadeOut->gradient.evaluate(1.0f).w, 0.0f);
    EXPECT_FLOAT_EQ(fadeOut->gradient.evaluate(0.5f).x, 1.0f);
    ASSERT_NE(presetNamed(library, "Fade In"), nullptr);
    EXPECT_FLOAT_EQ(presetNamed(library, "Fade In")->gradient.evaluate(0.25f).w, 0.25f);
    ASSERT_NE(presetNamed(library, "White"), nullptr);
    EXPECT_EQ(presetNamed(library, "White")->gradient.evaluate(0.3f), Vector4(1, 1, 1, 1));
    const GradientPreset* bw = presetNamed(library, "Black to White");
    ASSERT_NE(bw, nullptr);
    EXPECT_NEAR(bw->gradient.evaluate(0.5f).y, Color::sRGBToLinear(Vector3(0.5f, 0.5f, 0.5f)).x, 1e-5f);
    EXPECT_FLOAT_EQ(bw->gradient.evaluate(0.5f).w, 1.0f);
    ASSERT_NE(presetNamed(library, "White to Black"), nullptr);
    EXPECT_NEAR(presetNamed(library, "White to Black")->gradient.evaluate(0.25f).y, Color::sRGBToLinear(Vector3(0.75f, 0.75f, 0.75f)).x, 1e-5f);
    const GradientPreset* rainbow = presetNamed(library, "Rainbow");
    ASSERT_NE(rainbow, nullptr);
    EXPECT_EQ(rainbow->gradient.colorKeys.size(), 6u);
    EXPECT_EQ(rainbow->gradient.evaluate(0.2f), Vector4(1, 1, 0, 1));
    for (const GradientPreset& p : library.gradients){
        Gradient g = p.gradient;
        const Gradient copy = g;
        g.normalize();
        EXPECT_EQ(g, copy) << p.name << ": already valid";
        EXPECT_EQ(g.mode, GradientMode::Blend);
    }
}

TEST(GradientFieldLogicTest, APresetOverTheFieldsKeyLimitIsRefused){
    // F2: a ramp saved from a Procedural Material (32 keys) applied to a particle gradient (8)
    // would write a file the particle codec refuses
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    Gradient ramp;
    for (int i = 0; i < 32; i++) ramp.colorKeys.push_back({(float)i / 31.0f, Vector3((float)i / 31.0f, 0.5f, 0.2f)});
    ramp.alphaKeys = {{0.0f, 1.0f}, {1.0f, 0.0f}};
    const Gradient field = Gradient::twoColors(Vector3(0, 0, 0), Vector3(1, 1, 1));
    ui::GradientWindow& window = ui::GradientWindow::get();

    ui::GradientFieldOptions particle;
    const ImGuiID particleId = 101;
    window.open(particleId, field, particle);
    EXPECT_FALSE(window.applyPreset(ramp));
    EXPECT_NE(window.getWarning().find("32 colour keys"), std::string::npos) << window.getWarning();
    EXPECT_NE(window.getWarning().find("at most 8"), std::string::npos) << window.getWarning();
    Gradient value = field;
    EXPECT_EQ(window.consume(particleId, value), ui::EditPhase::None);
    EXPECT_EQ(value.colorKeys.size(), 2u) << "the field keeps its gradient";
    // within the limit it applies
    Gradient eight = ramp;
    eight.colorKeys.resize(8);
    EXPECT_TRUE(window.applyPreset(eight));
    EXPECT_EQ(window.consume(particleId, value), ui::EditPhase::Changing);
    EXPECT_EQ(value.colorKeys.size(), 8u);
    window.close();
    window.consume(particleId, value);

    // a graph's ramp holds 32: the same preset applies whole
    ui::GradientFieldOptions graph;
    graph.maxKeys = 32;
    const ImGuiID graphId = 102;
    window.open(graphId, field, graph);
    EXPECT_TRUE(window.applyPreset(ramp));
    value = field;
    EXPECT_EQ(window.consume(graphId, value), ui::EditPhase::Changing);
    EXPECT_EQ(value.colorKeys.size(), 32u);
    window.close();
    window.consume(graphId, value);

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}
