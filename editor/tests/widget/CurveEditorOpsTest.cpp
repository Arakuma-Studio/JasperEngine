// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The curve editor's model layer (docs/specs/S27-editor-tools.md R8, R10, R11, AC15)

#include "widget/CurveEditorOps.h"
#include "widget/CurvePresets.h"

#include "support/TestTempDir.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::curveops;
using jasperengine::test::TestTempDir;
namespace fs = std::filesystem;

namespace {

    // [0,1]x[0,1] over 100x100 pixels (y down)
    View unitView() {
        View v;
        v.t0 = 0.0f;
        v.t1 = 1.0f;
        v.v0 = 0.0f;
        v.v1 = 1.0f;
        v.x0 = 0.0f;
        v.y0 = 0.0f;
        v.x1 = 100.0f;
        v.y1 = 100.0f;
        return v;
    }

}

TEST(CurveEditorOps, HitTest) {
    const Curve curve = Curve::linear(0.0f, 0.0f, 1.0f, 1.0f);
    const View view = unitView();
    const std::vector<const Curve*> curves{&curve};
    const std::vector<bool> editable{true};
    // the key at (1, 1) is the pixel (100, 0)
    Hit hit = hitTest(curves, editable, view, 99.0f, 1.0f, 6.0f, 30.0f, {});
    EXPECT_EQ(hit.kind, Hit::Kind::Key);
    EXPECT_EQ(hit.key, 1);
    // the middle of the line
    hit = hitTest(curves, editable, view, 50.0f, 52.0f, 6.0f, 30.0f, {});
    EXPECT_EQ(hit.kind, Hit::Kind::Curve);
    EXPECT_NEAR(hit.time, 0.5f, 1e-6f);
    // nothing far from it
    hit = hitTest(curves, editable, view, 10.0f, 10.0f, 6.0f, 30.0f, {});
    EXPECT_EQ(hit.kind, Hit::Kind::None);
    // a selected key's out handle (slope 1: up-right, 30 px along it)
    float hx, hy;
    handlePosition(curve.keys[0], Side::Out, view, 30.0f, hx, hy);
    EXPECT_NEAR(hx, 30.0f / std::sqrt(2.0f), 1e-3f);
    EXPECT_NEAR(hy, 100.0f - 30.0f / std::sqrt(2.0f), 1e-3f);
    hit = hitTest(curves, editable, view, hx, hy, 6.0f, 30.0f, {{0, 0}});
    EXPECT_EQ(hit.kind, Hit::Kind::OutHandle);
    // a curve that is not editable is never hit
    hit = hitTest(curves, {false}, view, 99.0f, 1.0f, 6.0f, 30.0f, {});
    EXPECT_EQ(hit.kind, Hit::Kind::None);
}

TEST(CurveEditorOps, AddMoveDelete) {
    Curve curve = Curve::easeInOut(0.0f, 0.0f, 1.0f, 1.0f);
    // double-click on the curve: a key on it, the curve passing where it did
    const float before = curve.evaluate(0.5f);
    const int added = addKeyOnCurve(curve, 0.5f, nullptr);
    ASSERT_EQ(added, 1);
    EXPECT_FLOAT_EQ(curve.keys[1].value, before);
    EXPECT_EQ(curve.keys[1].inMode(), CurveTangentMode::ClampedAuto);
    EXPECT_EQ(addKeyOnCurve(curve, 0.5f, nullptr), -1);
    // double-click on empty space: at the point
    EXPECT_EQ(addKeyAt(curve, 0.25f, 0.9f, nullptr), 1);
    EXPECT_FLOAT_EQ(curve.keys[1].value, 0.9f);

    // a drag of two keys, from where they were
    std::vector<Curve*> curves{&curve};
    const std::vector<Curve> original{curve};
    std::vector<KeyRef> selection{{0, 1}, {0, 2}};
    moveKeys(curves, original, selection, 0.1f, -0.2f, nullptr);
    EXPECT_FLOAT_EQ(curve.keys[1].time, 0.35f);
    EXPECT_FLOAT_EQ(curve.keys[2].time, 0.6f);
    EXPECT_NEAR(curve.keys[1].value, 0.7f, 1e-6f);
    ASSERT_EQ(selection.size(), 2u);
    // a key moved past its neighbour: re-sorted, the selection follows
    moveKeys(curves, original, selection = std::vector<KeyRef>{{0, 1}}, 0.5f, 0.0f, nullptr);
    EXPECT_EQ(selection.size(), 1u);
    EXPECT_FLOAT_EQ(curve.keys[(size_t)selection[0].key].time, 0.75f);
    EXPECT_EQ(selection[0].key, 2);

    // Delete: the selected keys go, a curve keeps one key
    selection = {{0, 0}, {0, 1}};
    deleteKeys(curves, selection);
    EXPECT_EQ(curve.keys.size(), 2u);
    EXPECT_TRUE(selection.empty());
    selection = {{0, 0}, {0, 1}};
    deleteKeys(curves, selection);
    EXPECT_EQ(curve.keys.size(), 1u);
}

TEST(CurveEditorOps, TangentModes) {
    Curve curve({{0.0f, 0.0f}, {1.0f, 1.0f}, {2.0f, 0.0f}});
    setAuto(curve, 1);
    EXPECT_EQ(curve.keys[1].inMode(), CurveTangentMode::Auto);
    EXPECT_FLOAT_EQ(curve.keys[1].inTangent, 0.0f);   // mean of +1 and -1
    setTangentMode(curve, 1, Side::Out, CurveTangentMode::Linear);
    EXPECT_TRUE(curve.keys[1].broken());
    EXPECT_FLOAT_EQ(curve.keys[1].outTangent, -1.0f);
    setTangentMode(curve, 1, Side::In, CurveTangentMode::Constant);
    EXPECT_TRUE(std::isinf(curve.keys[1].inTangent));
    setFreeSmooth(curve, 1);
    EXPECT_FALSE(curve.keys[1].broken());
    EXPECT_EQ(curve.keys[1].inTangent, curve.keys[1].outTangent);
    setFlat(curve, 1);
    EXPECT_EQ(curve.keys[1].inTangent, 0.0f);
    EXPECT_EQ(curve.keys[1].outTangent, 0.0f);
    setBroken(curve, 1, true);
    EXPECT_TRUE(curve.keys[1].broken());
    // a handle drag: that side Free; unbroken, both sides follow
    dragTangent(curve, 1, Side::In, 2.0f);
    EXPECT_EQ(curve.keys[1].inTangent, 2.0f);
    EXPECT_EQ(curve.keys[1].outTangent, 0.0f);
    setBroken(curve, 1, false);
    dragTangent(curve, 1, Side::Out, -3.0f);
    EXPECT_EQ(curve.keys[1].inTangent, -3.0f);
    EXPECT_EQ(curve.keys[1].outTangent, -3.0f);
    setClampedAuto(curve, 1);
    EXPECT_EQ(curve.keys[1].inTangent, 0.0f);   // a peak is flat
    // the slope a handle dragged to a point gives; never across its key
    const View view = unitView();
    EXPECT_NEAR(slopeTowards(CurveKey{0.5f, 0.5f}, Side::Out, view, 75.0f, 25.0f), 1.0f, 1e-5f);
    EXPECT_GT(std::fabs(slopeTowards(CurveKey{0.5f, 0.5f}, Side::In, view, 75.0f, 25.0f)), 1000.0f);
}

TEST(CurveEditorOps, RangeClamp) {
    const CurveRange range{0.0f, -1.0f, 1.0f, 2.0f, true};
    Curve curve;
    EXPECT_EQ(addKeyAt(curve, 3.0f, 5.0f, &range), 0);
    EXPECT_FLOAT_EQ(curve.keys[0].time, 1.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].value, 2.0f);
    std::vector<Curve*> curves{&curve};
    addKeyAt(curve, 0.5f, 0.0f, &range);
    const std::vector<Curve> original{curve};
    std::vector<KeyRef> selection{{0, 0}};
    moveKeys(curves, original, selection, -2.0f, -9.0f, &range);
    EXPECT_FLOAT_EQ(curve.keys[0].time, 0.0f);
    EXPECT_FLOAT_EQ(curve.keys[0].value, -1.0f);
    Curve outside({{-1.0f, 7.0f}, {0.5f, 0.0f}, {4.0f, -3.0f}});
    clampToRange(outside, range);
    for (const CurveKey& k : outside.keys) {
        EXPECT_GE(k.time, 0.0f);
        EXPECT_LE(k.time, 1.0f);
        EXPECT_GE(k.value, -1.0f);
        EXPECT_LE(k.value, 2.0f);
    }
}

TEST(CurveEditorOps, PresetsRemapToTheRange) {
    const CurveRange range{0.0f, -1.0f, 2.0f, 3.0f, false};
    const CurvePresetLibrary& builtIn = curvepresets::builtIn();
    ASSERT_EQ(builtIn.presets.size(), 8u);
    const char* names[] = {"Constant", "Linear", "Linear Down", "Ease In", "Ease Out", "Ease In Out", "Ease In Out Down", "Bell"};
    for (size_t i = 0; i < 8; i++) EXPECT_EQ(builtIn.presets[i].name, names[i]);
    // Linear over [0,2]x[-1,3]: from (0,-1) to (2,3), slope 2
    const Curve linear = remap(builtIn.presets[1].curve, range);
    EXPECT_FLOAT_EQ(linear.keys[0].value, -1.0f);
    EXPECT_FLOAT_EQ(linear.keys[1].time, 2.0f);
    EXPECT_FLOAT_EQ(linear.keys[1].value, 3.0f);
    EXPECT_FLOAT_EQ(linear.keys[0].outTangent, 2.0f);
    EXPECT_FLOAT_EQ(linear.evaluate(1.0f), 1.0f);
    // and back
    const Curve back = normalise(linear, range);
    EXPECT_EQ(back, builtIn.presets[1].curve);
    // the bell peaks at the middle of the range
    const Curve bell = remap(builtIn.presets[7].curve, range);
    EXPECT_FLOAT_EQ(bell.evaluate(1.0f), 3.0f);
}

TEST(CurveEditorOps, CopyPasteAndFrame) {
    Curve curve({{0.0f, 0.0f}, {0.5f, 1.0f}, {1.0f, 0.0f}});
    const std::vector<CurveKey> copied = copyKeys(curve, {{0, 1}, {0, 2}}, 0);
    ASSERT_EQ(copied.size(), 2u);
    EXPECT_FLOAT_EQ(copied[0].time, 0.0f);
    EXPECT_FLOAT_EQ(copied[1].time, 0.5f);
    const std::vector<int> pasted = pasteKeys(curve, copied, 2.0f, nullptr);
    ASSERT_EQ(pasted.size(), 2u);
    EXPECT_EQ(curve.keys.size(), 5u);
    EXPECT_FLOAT_EQ(curve.keys[(size_t)pasted[1]].time, 2.5f);
    // pasted onto existing keys' times (0 and 0.5): they are replaced
    pasteKeys(curve, copied, 0.0f, nullptr);
    EXPECT_EQ(curve.keys.size(), 5u);
    EXPECT_FLOAT_EQ(curve.keys[0].value, 1.0f);
    // F: the keys framed with a margin
    const View pixels = unitView();
    const View all = frame({&curve}, {}, pixels, 0.1f, 0.1f);
    EXPECT_LT(all.t0, 0.0f);
    EXPECT_GT(all.t1, 2.5f);
    const View one = frame({&curve}, {{0, 0}}, pixels, 0.1f, 0.5f);
    EXPECT_NEAR(one.t1 - one.t0, 0.6f, 1e-5f);
}

TEST(CurvePresets, Libraries) {
    TestTempDir dir("curve-presets");
    const fs::path user = curvepresets::userDirectory(dir.path() / "settings");
    const fs::path project = curvepresets::projectDirectory(dir.path() / "Game");
    std::vector<CurvePresetLibrary> all = curvepresets::loadAll(user, project);
    ASSERT_EQ(all.size(), 2u);   // built-in and the seeded "My Presets"
    EXPECT_EQ(all[1].name, "My Presets");
    std::string error;
    EXPECT_EQ(curvepresets::nextPresetName(all[1]), "My Preset 1");
    ASSERT_TRUE(curvepresets::addPreset(all[1], "", Curve::linear(0.0f, 0.5f), error)) << error;
    EXPECT_EQ(curvepresets::nextPresetName(all[1]), "My Preset 2");
    EXPECT_FALSE(curvepresets::addPreset(all[0], "X", Curve(), error));   // built in: read-only
    all = curvepresets::loadAll(user, project);
    ASSERT_EQ(all[1].presets.size(), 1u);
    EXPECT_EQ(all[1].presets[0].name, "My Preset 1");
    EXPECT_EQ(all[1].presets[0].curve, Curve::linear(0.0f, 0.5f));
    ASSERT_TRUE(curvepresets::removePreset(all[1], 0, error));
    EXPECT_TRUE(curvepresets::loadAll(user, project)[1].presets.empty());
    // an unknown version: read-only
    CurvePresetLibrary future;
    ASSERT_TRUE(curvepresets::fromJson(R"({"version": 3, "presets": []})", future, error));
    EXPECT_TRUE(future.readOnly);
}

// S91 R23: the Curve window's Align, Smooth and Sharpen on the selected keys
TEST(CurveEditorOps, AlignGivesTheSelectedKeysTheirMeanValue) {
    Curve a;
    a.addKey(0.0f, 0.0f);
    a.addKey(0.5f, 1.0f);
    a.addKey(1.0f, 0.2f);
    Curve b;
    b.addKey(0.0f, 0.5f);
    std::vector<Curve*> curves = {&a, &b};
    alignKeys(curves, {{0, 1}, {0, 2}, {1, 0}});
    const float mean = (1.0f + 0.2f + 0.5f) / 3.0f;
    EXPECT_FLOAT_EQ(a.keys[0].value, 0.0f);
    EXPECT_FLOAT_EQ(a.keys[1].value, mean);
    EXPECT_FLOAT_EQ(a.keys[2].value, mean);
    EXPECT_FLOAT_EQ(b.keys[0].value, mean);
    // a stale reference is skipped
    alignKeys(curves, {{0, 7}, {3, 0}});
    EXPECT_FLOAT_EQ(a.keys[1].value, mean);
}

TEST(CurveEditorOps, SmoothJoinsAKeyAlongItsNeighboursAndSharpenMakesACorner) {
    Curve curve;
    curve.addKey(0.0f, 0.0f);
    curve.addKey(1.0f, 1.0f);
    curve.addKey(2.0f, 0.0f);
    std::vector<Curve*> curves = {&curve};
    sharpenKeys(curves, {{0, 1}});
    const CurveKey& middle = curve.keys[1];
    EXPECT_EQ(middle.inMode(), CurveTangentMode::Linear);
    EXPECT_EQ(middle.outMode(), CurveTangentMode::Linear);
    // towards each neighbour: up from the left, down to the right
    EXPECT_FLOAT_EQ(middle.inTangent, 1.0f);
    EXPECT_FLOAT_EQ(middle.outTangent, -1.0f);

    smoothKeys(curves, {{0, 1}});
    EXPECT_EQ(middle.inMode(), CurveTangentMode::Free);
    EXPECT_EQ(middle.outMode(), CurveTangentMode::Free);
    EXPECT_FALSE(middle.broken());
    // the mean of the slopes to its neighbours: a peak is flat
    EXPECT_FLOAT_EQ(middle.inTangent, 0.0f);
    EXPECT_FLOAT_EQ(middle.outTangent, 0.0f);
}
