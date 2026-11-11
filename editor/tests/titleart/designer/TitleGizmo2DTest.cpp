// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/designer/TitleGizmo2D.h"

#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    constexpr double PI = 3.14159265358979323846;

    CanvasXform view() {
        CanvasXform v;
        v.zoom = 1.0f;
        v.origin = ImVec2(400.0f, 300.0f);
        return v;
    }

    Gizmo2DTarget target(ta::LayerType type, const std::string& id = "0000000a") {
        Gizmo2DTarget t;
        t.layerId = id;
        t.type = type;
        t.localBox = {-100.0, -25.0, 200.0, 50.0};
        return t;
    }

    Affine2 docOf(const Gizmo2DTarget& t) { return t.parentToDoc * layerTransformAffine(t.transform); }

    // the box corners BL, BR, TR, TL in document space
    TitleVec2 corner(const Gizmo2DTarget& t, int i) {
        const TitleRect& b = t.localBox;
        const TitleVec2 local[4] = {{b.x, b.y}, {b.x + b.width, b.y}, {b.x + b.width, b.y + b.height}, {b.x, b.y + b.height}};
        return docOf(t).apply(local[i]);
    }

    ImVec2 screen(TitleVec2 p) { return view().toScreen(p); }
    ImVec2 offset(ImVec2 p, float dx, float dy) { return ImVec2(p.x + dx, p.y + dy); }

    const FieldEdit* find(const std::vector<FieldEdit>& edits, const std::string& layer, const std::string& path) {
        for (const FieldEdit& e : edits) {
            if (e.layer == layer && e.path == path) return &e;
        }
        return nullptr;
    }

    // the target with the gizmo's edits applied, as the canvas stages them
    Gizmo2DTarget applied(Gizmo2DTarget t, const std::vector<FieldEdit>& edits) {
        for (const FieldEdit& e : edits) {
            if (e.layer != t.layerId) continue;
            if (e.path == "transform.position") t.transform.position = {e.value[0].get<double>(), e.value[1].get<double>()};
            else if (e.path == "transform.rotation") t.transform.rotation = e.value.get<double>();
            else if (e.path == "transform.scale") t.transform.scale = e.value.get<double>();
            else if (e.path == "h_scale") t.hScale = e.value.get<double>();
            else if (e.path == "v_scale") t.vScale = e.value.get<double>();
            else if (e.path == "size") t.size = e.value.get<double>();
            else if (e.path == "aspect") t.aspect = e.value.get<double>();
        }
        return t;
    }

    double distance(TitleVec2 a, TitleVec2 b) { return std::hypot(a.x - b.x, a.y - b.y); }

    // a screen point `degrees` clockwise (on screen) around `centre` from `from`
    ImVec2 turnedOnScreen(ImVec2 centre, ImVec2 from, double degrees) {
        const double a = degrees * PI / 180.0, dx = from.x - centre.x, dy = from.y - centre.y;
        // screen y is down: clockwise on screen is the mathematical positive turn of (x, y-down)
        return ImVec2((float)(centre.x + dx * std::cos(a) - dy * std::sin(a)), (float)(centre.y + dx * std::sin(a) + dy * std::cos(a)));
    }

}

// The 8 handles take presses within a 24 px square (12 px each way); just outside a corner is
// its rotate zone, inside the box a move.
// Red control: a 16 px hit (handleHit 16) misses the 11 px offsets.
TEST(TitleGizmo2DTest, HandlesHitAt24PxAndRotateZones) {
    TitleGizmo2D g;
    // tall enough that an edge handle's outside is clear of the corners' rotate zones
    Gizmo2DTarget t = target(ta::LayerType::Text);
    t.localBox = {-100.0, -60.0, 200.0, 120.0};
    g.setTargets({t}, Tool::Rect, PivotMode::Pivot, view());
    const Gizmo2DHandle corners[4] = {Gizmo2DHandle::CornerBL, Gizmo2DHandle::CornerBR, Gizmo2DHandle::CornerTR, Gizmo2DHandle::CornerTL};
    const Gizmo2DHandle rotates[4] = {Gizmo2DHandle::RotateBL, Gizmo2DHandle::RotateBR, Gizmo2DHandle::RotateTR, Gizmo2DHandle::RotateTL};
    // the outward diagonal of each corner on screen (y down)
    const float out[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
    for (int i = 0; i < 4; i++) {
        const ImVec2 c = screen(corner(t, i));
        EXPECT_EQ(g.hitTest(c), corners[i]);
        EXPECT_EQ(g.hitTest(offset(c, 11.0f * out[i][0], 11.0f * out[i][1])), corners[i]);
        EXPECT_EQ(g.hitTest(offset(c, -11.0f * out[i][0], -11.0f * out[i][1])), corners[i]);
        // past the hit square, outside the box: the rotate zone, up to 36 px from the corner
        EXPECT_EQ(g.hitTest(offset(c, 13.0f * out[i][0], 13.0f * out[i][1])), rotates[i]);
        EXPECT_EQ(g.hitTest(offset(c, 24.0f * out[i][0], 24.0f * out[i][1])), rotates[i]);
        EXPECT_EQ(g.hitTest(offset(c, 30.0f * out[i][0], 30.0f * out[i][1])), Gizmo2DHandle::None);
        // past the hit square, inside the box: a move, never a rotation
        EXPECT_EQ(g.hitTest(offset(c, -14.0f * out[i][0], -14.0f * out[i][1])), Gizmo2DHandle::Inside);
    }
    const struct {
        TitleVec2 at;
        Gizmo2DHandle handle;
        float nx, ny;   // outward on screen
    } edges[4] = {{{-100.0, 0.0}, Gizmo2DHandle::EdgeL, -1, 0}, {{100.0, 0.0}, Gizmo2DHandle::EdgeR, 1, 0},
                  {{0.0, -60.0}, Gizmo2DHandle::EdgeB, 0, 1}, {{0.0, 60.0}, Gizmo2DHandle::EdgeT, 0, -1}};
    for (const auto& e : edges) {
        const ImVec2 c = screen(e.at);
        EXPECT_EQ(g.hitTest(c), e.handle);
        EXPECT_EQ(g.hitTest(offset(c, 11.0f * e.nx + 11.0f * e.ny, 11.0f * e.ny + 11.0f * e.nx)), e.handle);
        EXPECT_EQ(g.hitTest(offset(c, 13.0f * e.nx, 13.0f * e.ny)), Gizmo2DHandle::None);
    }
    // the pivot circle (Pivot mode: the position) wins over the box
    EXPECT_EQ(g.hitTest(screen({0.0, 0.0})), Gizmo2DHandle::Pivot);
    EXPECT_EQ(g.cursor(Gizmo2DHandle::RotateTR), Gizmo2DCursor::Rotate);
    EXPECT_EQ(g.cursor(Gizmo2DHandle::EdgeR), Gizmo2DCursor::ResizeEW);
    EXPECT_EQ(g.cursor(Gizmo2DHandle::EdgeT), Gizmo2DCursor::ResizeNS);
}

// A corner scales uniformly about the opposite corner: that corner stays within 1e-4 px, on a
// turned, scaled, mirrored layer.
// Red control: scaling about the pivot instead (o = drag.pivot without Alt) moves BL by tens of px.
TEST(TitleGizmo2DTest, CornerScaleKeepsTheOppositeCornerFixed) {
    Gizmo2DTarget t = target(ta::LayerType::Image);
    t.transform.position = {40.0, -20.0};
    t.transform.rotation = 30.0;
    t.transform.scale = 1.5;
    t.transform.flipX = true;
    TitleGizmo2D g;
    g.setTargets({t}, Tool::Rect, PivotMode::Pivot, view());
    const ImVec2 tr = screen(corner(t, 2));
    ASSERT_EQ(g.hitTest(tr), Gizmo2DHandle::CornerTR);
    ASSERT_TRUE(g.begin(Gizmo2DHandle::CornerTR, tr, {}));
    const std::vector<FieldEdit> edits = g.update(offset(tr, 37.0f, -23.0f), {});
    ASSERT_NE(find(edits, t.layerId, "transform.scale"), nullptr);
    ASSERT_NE(find(edits, t.layerId, "transform.position"), nullptr);
    EXPECT_EQ(find(edits, t.layerId, "transform.rotation"), nullptr);
    const Gizmo2DTarget after = applied(t, edits);
    EXPECT_NE(after.transform.scale, 1.5);
    EXPECT_LT(distance(corner(after, 0), corner(t, 0)), 1e-4);
    EXPECT_EQ(g.gestureName(), "Scale Layer");
    EXPECT_EQ(g.readout().rfind("Scale", 0), 0u);
}

// Alt scales about the pivot: the point under the pivot stays.
TEST(TitleGizmo2DTest, AltScalesAboutThePivot) {
    Gizmo2DTarget t = target(ta::LayerType::Image);
    t.transform.rotation = -20.0;
    t.hasPivot = true;
    t.pivot = {10.0, 5.0};
    TitleGizmo2D g;
    g.setTargets({t}, Tool::Rect, PivotMode::Pivot, view());
    const ImVec2 tr = screen(corner(t, 2));
    GizmoModifiers alt;
    alt.alt = true;
    ASSERT_TRUE(g.begin(Gizmo2DHandle::CornerTR, tr, alt));
    const Gizmo2DTarget after = applied(t, g.update(offset(tr, 30.0f, -10.0f), alt));
    EXPECT_GT(after.transform.scale, 1.0);
    const TitleVec2 local = docOf(t).inverse().apply(t.pivot);
    EXPECT_LT(distance(docOf(after).apply(local), t.pivot), 1e-6);
    // without Alt the opposite corner stays instead, and the pivot point moves
    TitleGizmo2D plain;
    plain.setTargets({t}, Tool::Rect, PivotMode::Pivot, view());
    ASSERT_TRUE(plain.begin(Gizmo2DHandle::CornerTR, tr, {}));
    const Gizmo2DTarget other = applied(t, plain.update(offset(tr, 30.0f, -10.0f), {}));
    EXPECT_GT(distance(docOf(other).apply(local), t.pivot), 1.0);
}

// Edges: Text writes h_scale / v_scale, Shape aspect / size, Image a uniform scale; the opposite
// edge stays.
// Red control: routing text edges through the uniform scale writes transform.scale and no h_scale.
TEST(TitleGizmo2DTest, EdgesWriteTheLayerTypesFields) {
    TitleGizmo2D g;
    const Gizmo2DTarget text = target(ta::LayerType::Text);
    g.setTargets({text}, Tool::Rect, PivotMode::Pivot, view());
    const ImVec2 right = screen({100.0, 0.0});
    ASSERT_TRUE(g.begin(Gizmo2DHandle::EdgeR, right, {}));
    std::vector<FieldEdit> edits = g.update(offset(right, 50.0f, 0.0f), {});
    ASSERT_NE(find(edits, text.layerId, "h_scale"), nullptr);
    EXPECT_NEAR(find(edits, text.layerId, "h_scale")->value.get<double>(), 1.25, 1e-9);
    EXPECT_EQ(find(edits, text.layerId, "transform.scale"), nullptr);
    // the content scales about the layer origin; the position keeps the left edge at -100
    Gizmo2DTarget after = applied(text, edits);
    EXPECT_NEAR(after.transform.position.x + -100.0 * 1.25, -100.0, 1e-9);
    EXPECT_EQ(g.gestureName(), "Change Horizontal Scale");
    EXPECT_NE(g.readout().find("125 %"), std::string::npos);
    g.end();
    const ImVec2 top = screen({0.0, 25.0});
    ASSERT_TRUE(g.begin(Gizmo2DHandle::EdgeT, top, {}));
    edits = g.update(offset(top, 0.0f, -25.0f), {});
    ASSERT_NE(find(edits, text.layerId, "v_scale"), nullptr);
    EXPECT_NEAR(find(edits, text.layerId, "v_scale")->value.get<double>(), 1.5, 1e-9);
    g.end();

    Gizmo2DTarget shape = target(ta::LayerType::Shape);
    shape.size = 50.0;
    shape.aspect = 4.0;
    g.setTargets({shape}, Tool::Rect, PivotMode::Pivot, view());
    ASSERT_TRUE(g.begin(Gizmo2DHandle::EdgeR, right, {}));
    edits = g.update(offset(right, 50.0f, 0.0f), {});
    ASSERT_NE(find(edits, shape.layerId, "aspect"), nullptr);
    EXPECT_NEAR(find(edits, shape.layerId, "aspect")->value.get<double>(), 5.0, 1e-9);
    EXPECT_EQ(find(edits, shape.layerId, "size"), nullptr);
    g.end();
    ASSERT_TRUE(g.begin(Gizmo2DHandle::EdgeT, top, {}));
    edits = g.update(offset(top, 0.0f, -25.0f), {});
    ASSERT_NE(find(edits, shape.layerId, "size"), nullptr);
    EXPECT_NEAR(find(edits, shape.layerId, "size")->value.get<double>(), 75.0, 1e-9);
    // the width (size x aspect) stays
    EXPECT_NEAR(find(edits, shape.layerId, "aspect")->value.get<double>() * 75.0, 200.0, 1e-9);
    g.end();

    const Gizmo2DTarget image = target(ta::LayerType::Image);
    g.setTargets({image}, Tool::Rect, PivotMode::Pivot, view());
    ASSERT_TRUE(g.begin(Gizmo2DHandle::EdgeR, right, {}));
    edits = g.update(offset(right, 50.0f, 0.0f), {});
    ASSERT_NE(find(edits, image.layerId, "transform.scale"), nullptr);
    EXPECT_EQ(find(edits, image.layerId, "h_scale"), nullptr);
    EXPECT_NEAR(find(edits, image.layerId, "transform.scale")->value.get<double>(), 1.25, 1e-9);
    // the left edge's midpoint stays
    after = applied(image, edits);
    EXPECT_LT(distance(docOf(after).apply({-100.0, 0.0}), {-100.0, 0.0}), 1e-9);
}

// The rotate zone turns about the pivot; Shift steps by 15°; Ctrl by the 15° increment.
// Red control: dropping the Shift branch writes 37° instead of 30°.
TEST(TitleGizmo2DTest, RotationShiftSteps15Degrees) {
    const Gizmo2DTarget t = target(ta::LayerType::Text);
    TitleGizmo2D g;
    g.setTargets({t}, Tool::Rect, PivotMode::Pivot, view());
    const ImVec2 press = offset(screen(corner(t, 2)), 20.0f, -20.0f);
    ASSERT_EQ(g.hitTest(press), Gizmo2DHandle::RotateTR);
    const ImVec2 pivot = screen({0.0, 0.0});
    const ImVec2 to = turnedOnScreen(pivot, press, 37.0);

    ASSERT_TRUE(g.begin(Gizmo2DHandle::RotateTR, press, {}));
    std::vector<FieldEdit> edits = g.update(to, {});
    ASSERT_NE(find(edits, t.layerId, "transform.rotation"), nullptr);
    EXPECT_NEAR(find(edits, t.layerId, "transform.rotation")->value.get<double>(), 37.0, 1e-3);
    g.end();

    GizmoModifiers shift;
    shift.shift = true;
    ASSERT_TRUE(g.begin(Gizmo2DHandle::RotateTR, press, shift));
    edits = g.update(to, shift);
    EXPECT_DOUBLE_EQ(find(edits, t.layerId, "transform.rotation")->value.get<double>(), 30.0);
    // the pivot is the position: it stays
    const FieldEdit* position = find(edits, t.layerId, "transform.position");
    ASSERT_NE(position, nullptr);
    EXPECT_NEAR(position->value[0].get<double>(), 0.0, 1e-9);
    EXPECT_NEAR(position->value[1].get<double>(), 0.0, 1e-9);
    EXPECT_EQ(g.readout(), "Rotate \xC2\xB7 30\xC2\xB0");
    EXPECT_EQ(g.gestureName(), "Rotate Layer");
    g.end();

    GizmoModifiers ctrl;
    ctrl.ctrl = true;
    ASSERT_TRUE(g.begin(Gizmo2DHandle::RotateTR, press, ctrl));
    edits = g.update(to, ctrl);
    EXPECT_DOUBLE_EQ(find(edits, t.layerId, "transform.rotation")->value.get<double>(), 30.0);
}

// Dragging the pivot moves the gizmo's pivot only: no edit.
// Red control: making the pivot drag a Move gesture returns position edits.
TEST(TitleGizmo2DTest, PivotDragChangesNoDocumentValue) {
    const Gizmo2DTarget t = target(ta::LayerType::Text);
    TitleGizmo2D g;
    g.setTargets({t}, Tool::Rect, PivotMode::Pivot, view());
    const ImVec2 p = screen({0.0, 0.0});
    ASSERT_EQ(g.hitTest(p), Gizmo2DHandle::Pivot);
    ASSERT_TRUE(g.begin(Gizmo2DHandle::Pivot, p, {}));
    EXPECT_TRUE(g.update(offset(p, 30.0f, 10.0f), {}).empty());
    EXPECT_TRUE(g.pivotMoved());
    EXPECT_NEAR(g.pivot().x, 30.0, 1e-9);
    EXPECT_NEAR(g.pivot().y, -10.0, 1e-9);
    // the next frame (the selection unchanged) keeps the dragged pivot while the drag runs
    g.setTargets({t}, Tool::Rect, PivotMode::Pivot, view());
    EXPECT_NEAR(g.pivot().x, 30.0, 1e-9);
    EXPECT_EQ(g.gestureName(), "");
    // Center mode puts the pivot at the box centre
    Gizmo2DTarget off = t;
    off.localBox = {0.0, 0.0, 100.0, 40.0};
    g.end();
    g.setTargets({off}, Tool::Rect, PivotMode::Center, view());
    EXPECT_NEAR(g.pivot().x, 50.0, 1e-9);
    EXPECT_NEAR(g.pivot().y, 20.0, 1e-9);
}

// Several layers turn about the shared pivot in one gesture.
// Red control: rotating each layer about its own position leaves the positions unchanged.
TEST(TitleGizmo2DTest, MultiTargetRotationAboutTheSharedPivot) {
    Gizmo2DTarget a = target(ta::LayerType::Text, "0000000a");
    a.transform.position = {100.0, 0.0};
    a.transform.rotation = 10.0;
    a.hasPivot = true;
    a.pivot = {20.0, 10.0};
    Gizmo2DTarget b = target(ta::LayerType::Shape, "0000000b");
    b.transform.position = {-50.0, 40.0};
    b.transform.rotation = -5.0;
    TitleGizmo2D g;
    g.setTargets({a, b}, Tool::Rotate, PivotMode::Pivot, view());
    const ImVec2 pv = screen(a.pivot);
    const ImVec2 press = offset(pv, 72.0f, 0.0f);
    ASSERT_EQ(g.hitTest(press), Gizmo2DHandle::Ring);
    ASSERT_TRUE(g.begin(Gizmo2DHandle::Ring, press, {}));
    const std::vector<FieldEdit> edits = g.update(offset(pv, 0.0f, 72.0f), {});
    const Gizmo2DTarget a1 = applied(a, edits), b1 = applied(b, edits);
    EXPECT_NEAR(a1.transform.rotation, 100.0, 1e-9);
    EXPECT_NEAR(b1.transform.rotation, 85.0, 1e-9);
    // 90° clockwise about (20, 10): (x, y) - p -> (y, -x) + p
    EXPECT_NEAR(a1.transform.position.x, 10.0, 1e-9);
    EXPECT_NEAR(a1.transform.position.y, -70.0, 1e-9);
    EXPECT_NEAR(b1.transform.position.x, 50.0, 1e-9);
    EXPECT_NEAR(b1.transform.position.y, 80.0, 1e-9);
    EXPECT_EQ(g.gestureName(), "Rotate 2 Layers");
}

// A layer in a turned, scaled, mirrored group: the document result is the document-space turn
// of the layer, its rotation runs the other way in the group, and corner scales and moves stay exact.
// Red control: ignoring the parent's mirror (sigma = 1) writes 55° instead of -35° and the matrices differ.
TEST(TitleGizmo2DTest, ParentedMirroredTarget) {
    ta::TitleTransform group;
    group.position = {30.0, -10.0};
    group.rotation = 25.0;
    group.scale = 2.0;
    group.flipX = true;
    Gizmo2DTarget t = target(ta::LayerType::Image);
    t.parentToDoc = layerTransformAffine(group);
    t.transform.position = {15.0, 5.0};
    t.transform.rotation = 10.0;
    t.transform.scale = 0.8;

    TitleGizmo2D g;
    g.setTargets({t}, Tool::Rotate, PivotMode::Pivot, view());
    const TitleVec2 pivot = t.parentToDoc.apply(t.transform.position);
    const ImVec2 pv = screen(pivot);
    const ImVec2 press = offset(pv, 72.0f, 0.0f);
    ASSERT_EQ(g.hitTest(press), Gizmo2DHandle::Ring);
    GizmoModifiers shift;
    shift.shift = true;
    ASSERT_TRUE(g.begin(Gizmo2DHandle::Ring, press, shift));
    const Gizmo2DTarget after = applied(t, g.update(turnedOnScreen(pv, press, 44.0), shift));
    EXPECT_NEAR(after.transform.rotation, 10.0 - 45.0, 1e-9);
    const Affine2 turn = Affine2::translate(pivot.x, pivot.y) * Affine2::rotateClockwise(45.0) * Affine2::translate(-pivot.x, -pivot.y);
    const Affine2 want = turn * docOf(t), got = docOf(after);
    EXPECT_NEAR(got.a, want.a, 1e-9);
    EXPECT_NEAR(got.b, want.b, 1e-9);
    EXPECT_NEAR(got.c, want.c, 1e-9);
    EXPECT_NEAR(got.d, want.d, 1e-9);
    EXPECT_NEAR(got.e, want.e, 1e-9);
    EXPECT_NEAR(got.f, want.f, 1e-9);
    g.end();

    g.setTargets({t}, Tool::Rect, PivotMode::Pivot, view());
    const ImVec2 bl = screen(corner(t, 0));
    ASSERT_TRUE(g.begin(Gizmo2DHandle::CornerBL, bl, {}));
    const Gizmo2DTarget scaled = applied(t, g.update(offset(bl, -25.0f, 18.0f), {}));
    EXPECT_NE(scaled.transform.scale, 0.8);
    EXPECT_LT(distance(corner(scaled, 2), corner(t, 2)), 1e-4);
    g.end();

    // a move inside the box: the document position moves by exactly the drag
    const ImVec2 inside = screen(docOf(t).apply({20.0, 0.0}));
    ASSERT_EQ(g.hitTest(inside), Gizmo2DHandle::Inside);
    ASSERT_TRUE(g.begin(Gizmo2DHandle::Inside, inside, {}));
    const Gizmo2DTarget moved = applied(t, g.update(offset(inside, 30.0f, -12.0f), {}));
    const TitleVec2 from = t.parentToDoc.apply(t.transform.position), to = moved.parentToDoc.apply(moved.transform.position);
    EXPECT_NEAR(to.x - from.x, 30.0, 1e-9);
    EXPECT_NEAR(to.y - from.y, 12.0, 1e-9);
}

// Moves snap within 8 screen px to a guide; Ctrl uses increments instead (10 px, 0.1 scale); Shift
// keeps one axis.
// Red control: passing no SnapContext leaves the 45 px move at 45.
TEST(TitleGizmo2DTest, SnappingAndIncrements) {
    const Gizmo2DTarget t = target(ta::LayerType::Image);
    TitleGizmo2D g;
    g.setTargets({t}, Tool::Rect, PivotMode::Pivot, view());
    SnapContext snap;
    snap.zoom = 1.0f;
    snap.guidesX = {150.0};
    const ImVec2 inside = screen({50.0, 0.0});
    ASSERT_TRUE(g.begin(Gizmo2DHandle::Inside, inside, {}));
    std::vector<FieldEdit> edits = g.update(offset(inside, 45.0f, 0.0f), {}, &snap);
    // the right edge (100 + 45) is 5 px from the guide: it snaps to 150
    EXPECT_NEAR(find(edits, t.layerId, "transform.position")->value[0].get<double>(), 50.0, 1e-9);
    ASSERT_EQ(g.lastSnap().lines.size(), 1u);
    EXPECT_DOUBLE_EQ(g.lastSnap().lines[0].at, 150.0);
    edits = g.update(offset(inside, 40.0f, 0.0f), {}, &snap);
    EXPECT_NEAR(find(edits, t.layerId, "transform.position")->value[0].get<double>(), 40.0, 1e-9);
    // Shift keeps the larger axis
    GizmoModifiers shift;
    shift.shift = true;
    edits = g.update(offset(inside, 33.0f, -4.0f), shift);
    EXPECT_NEAR(find(edits, t.layerId, "transform.position")->value[1].get<double>(), 0.0, 1e-9);
    // Ctrl: 10 px increments, no snapping
    GizmoModifiers ctrl;
    ctrl.ctrl = true;
    edits = g.update(offset(inside, 43.0f, -17.0f), ctrl, &snap);
    EXPECT_NEAR(find(edits, t.layerId, "transform.position")->value[0].get<double>(), 40.0, 1e-9);
    EXPECT_NEAR(find(edits, t.layerId, "transform.position")->value[1].get<double>(), 20.0, 1e-9);
    g.end();
    // Ctrl on a corner: the scale lands on a 0.1 step
    const ImVec2 tr = screen(corner(t, 2));
    ASSERT_TRUE(g.begin(Gizmo2DHandle::CornerTR, tr, ctrl));
    edits = g.update(offset(tr, 23.0f, -7.0f), ctrl, &snap);
    const double s = find(edits, t.layerId, "transform.scale")->value.get<double>();
    EXPECT_NEAR(s * 10.0, std::round(s * 10.0), 1e-9);
}

// Arrow keys: 1 canvas px, Shift 10, converted into each layer's parent space.
TEST(TitleGizmo2DTest, NudgeInDocumentSpace) {
    EXPECT_DOUBLE_EQ(nudgeDelta(1, 0, false).x, 1.0);
    EXPECT_DOUBLE_EQ(nudgeDelta(0, -1, true).y, -10.0);
    ta::TitleTransform group;
    group.rotation = 90.0;
    group.flipX = true;
    Gizmo2DTarget t = target(ta::LayerType::Text);
    t.parentToDoc = layerTransformAffine(group);
    const std::vector<FieldEdit> edits = nudgeEdits({t}, {10.0, 0.0});
    ASSERT_EQ(edits.size(), 1u);
    const Gizmo2DTarget after = applied(t, edits);
    const TitleVec2 moved = after.parentToDoc.apply(after.transform.position);
    EXPECT_NEAR(moved.x, 10.0, 1e-9);
    EXPECT_NEAR(moved.y, 0.0, 1e-9);
}
