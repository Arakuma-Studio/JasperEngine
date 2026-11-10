// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The rotation-channel cases are ported from Pachislop tests/unit/rotation3d.test.ts (commit
// ecaa136) by SafEight, used with the author's permission.

#include <gtest/gtest.h>

#include "titleart/designer/TitleGizmo3D.h"

#include <cmath>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine::editor::titledesigner;
namespace r3d = jasperengine::editor::titledesigner::rotation3d;

namespace {

    constexpr double PI = 3.14159265358979323846;
    constexpr double DEG = PI / 180.0;
    const Vec3d RIGHT{1.0, 0.0, 0.0};
    const Vec3d UP{0.0, 1.0, 0.0};

    void expectClose(const Mat3d& a, const Mat3d& b) {
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) EXPECT_NEAR(a.m[i][j], b.m[i][j], 1e-6) << "element " << i << "," << j;
        }
    }

    Mat3d scaledBy(const Mat3d& a, double k) {
        Mat3d r = a;
        for (auto& row : r.m) {
            for (double& v : row) v *= k;
        }
        return r;
    }

    double dot(const Vec3d& a, const Vec3d& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    // a mirrored (flip X), turned layer: the frame a letter's tilts turn in
    Mat3d mirrored() { return r3d::eulerZYX(0.7, -0.4, 0.3) * Mat3d::diagonal(-1.0, 1.0, 1.0); }

    std::vector<std::pair<std::string, RotationModel>> models() {
        return {{"layer", RotationModel::layer()}, {"glyph on a turned, mirrored layer", RotationModel::glyph(mirrored())}};
    }

    const std::vector<Vec3d> POSES = {{0.0, 0.0, 0.0}, {60.0, 40.0, 45.0}, {-120.0, 75.0, 170.0}, {179.0, -30.0, -179.0}};

    CanvasXform canvas(float zoom, ImVec2 origin) {
        CanvasXform v;
        v.zoom = zoom;
        v.origin = origin;
        return v;
    }

    const FieldEdit* find(const std::vector<FieldEdit>& edits, const std::string& path) {
        for (const FieldEdit& e : edits) {
            if (e.path == path) return &e;
        }
        return nullptr;
    }

}

// ---- rotation3d.test.ts ----

TEST(TitleGizmo3DTest, ChannelsReadBackFromTheirOrientation) {
    for (const auto& [name, model] : models()) {
        for (const Vec3d& rot : POSES) {
            const Vec3d got = model.fromOrient(model.orient(rot), rot);
            EXPECT_NEAR(got.x, rot.x, 1e-6) << name;
            EXPECT_NEAR(got.y, rot.y, 1e-6) << name;
            EXPECT_NEAR(got.z, rot.z, 1e-6) << name;
        }
    }
}

TEST(TitleGizmo3DTest, TrackballWrittenBackGivesTheDraggedOrientation) {
    for (const auto& [name, model] : models()) {
        for (const Vec3d& rot : POSES) {
            const Mat3d w = r3d::trackball(model.orient(rot), RIGHT, UP, 35.0, -50.0);
            expectClose(model.orient(model.fromOrient(w, rot)), w);
        }
    }
}

// The nearest Euler branch: no step of a long drag jumps a value by more than the turn itself.
// Red control: fromOrient returning the first solution (no nearestBranch) flips by ~180° mid-drag.
TEST(TitleGizmo3DTest, LongDragStaysContinuous) {
    for (const auto& [name, model] : models()) {
        const Vec3d rot0{60.0, 40.0, 45.0};
        const Mat3d w0 = model.orient(rot0);
        Vec3d prev = rot0;
        for (int px = 1; px <= 900; px++) {
            const Vec3d next = model.fromOrient(r3d::trackball(w0, RIGHT, UP, px * 0.4, px * 0.15), prev);
            for (int i = 0; i < 3; i++) ASSERT_LT(std::abs(next[i] - prev[i]), 20.0) << name << " px " << px;
            prev = next;
        }
    }
}

TEST(TitleGizmo3DTest, ArcTurnsAboutItsCompAxis) {
    for (const auto& [name, model] : models()) {
        for (const Vec3d& rot : POSES) {
            for (int i = 0; i < 3; i++) {
                const Vec3d n = r3d::compAxis(i);
                const Mat3d w0 = model.orient(rot);
                const Mat3d w = r3d::turnAbout(w0, n, 70.0);
                expectClose(model.orient(model.fromOrient(w, rot)), w);
                // the turn is about n: n stays where it is
                const Mat3d q = w * w0.transposed();
                EXPECT_NEAR(dot(q * n, n), 1.0, 1e-6) << name;
            }
        }
    }
}

// Full turns: a whole arc sweep stays continuous and comes back to the start.
TEST(TitleGizmo3DTest, FullArcSweepComesBack) {
    for (const auto& [name, model] : models()) {
        for (int i = 0; i < 3; i++) {
            const Vec3d rot0{60.0, 40.0, 45.0};
            const Mat3d w0 = model.orient(rot0);
            const Vec3d n = r3d::compAxis(i);
            Vec3d prev = rot0;
            for (int deg = 1; deg <= 360; deg++) {
                const Vec3d next = model.fromOrient(r3d::turnAbout(w0, n, deg), prev);
                for (int k = 0; k < 3; k++) ASSERT_LT(std::abs(next[k] - prev[k]), 20.0) << name << " axis " << i << " deg " << deg;
                prev = next;
            }
            expectClose(model.orient(prev), w0);
        }
    }
}

TEST(TitleGizmo3DTest, SchematicArmsAreConstant) {
    EXPECT_EQ(r3d::schematicArm(0).x, (float)r3d::ARM_PX);
    EXPECT_EQ(r3d::schematicArm(0).y, 0.0f);
    EXPECT_EQ(r3d::schematicArm(1).x, 0.0f);
    EXPECT_EQ(r3d::schematicArm(1).y, (float)-r3d::ARM_PX);
    EXPECT_NEAR(r3d::schematicArm(2).x, -r3d::schematicArm(2).y, 1e-6);
    EXPECT_LT(r3d::schematicArm(2).x, 0.0f);
    EXPECT_NEAR(std::hypot(r3d::schematicArm(2).x, r3d::schematicArm(2).y), r3d::Z_ARM_PX, 1e-4);
}

// The Z arrow moves at the X arrow's rate per px.
TEST(TitleGizmo3DTest, ArmTravelAtThePivotRate) {
    const double k = 0.75;  // screen px per world unit at the pivot
    EXPECT_NEAR(r3d::armTravel(0, 30.0, 0.0, k), 40.0, 1e-5);
    EXPECT_NEAR(r3d::armTravel(1, 0.0, -30.0, k), 40.0, 1e-5);
    EXPECT_NEAR(r3d::armTravel(2, -30.0 * std::sqrt(0.5), 30.0 * std::sqrt(0.5), k), 40.0, 1e-5);
    EXPECT_NEAR(r3d::armTravel(0, 0.0, 25.0, k), 0.0, 1e-9);
}

TEST(TitleGizmo3DTest, EachArcRunsCyclicallyPositiveAboutItsAxis) {
    for (int i = 0; i < 3; i++) {
        const Vec3d a = r3d::compAxis((i + 1) % 3), b = r3d::compAxis((i + 2) % 3);
        EXPECT_NEAR(dot(r3d::turnAbout(Mat3d(), r3d::compAxis(i), 90.0) * a, b), 1.0, 1e-6);
    }
}

TEST(TitleGizmo3DTest, LetterOnAMirroredLayerTurnsAboutCompY) {
    const Mat3d base = mirrored();
    const RotationModel glyph = RotationModel::glyph(base);
    const Vec3d rot0{20.0, -35.0, 10.0};
    const Vec3d turned = glyph.fromOrient(r3d::turnAbout(glyph.orient(rot0), r3d::compAxis(1), 50.0), rot0);
    const Mat3d ry = r3d::turnAbout(Mat3d(), UP, 50.0);
    expectClose(glyph.orient(turned), ry * glyph.orient(rot0));
    // inside the layer: the turn conjugated by the layer's rotation and flip
    const Mat3d inv = base.inverse();
    expectClose(inv * glyph.orient(turned), inv * ry * base * (inv * glyph.orient(rot0)));
}

TEST(TitleGizmo3DTest, MoveAlongCompXBecomesTheLetterValues) {
    const double k = 2.4;  // 1 % of a 160 px size x scale 1.5
    const Mat3d m3 = scaledBy(mirrored(), 1.5);
    const std::array<Vec3d, 3> axes = {m3 * Vec3d{k, 0.0, 0.0}, m3 * Vec3d{0.0, k, 0.0}, m3 * Vec3d{-0.3 * k, 0.1 * k, k}};
    const Vec3d v = r3d::moveValues(axes, {10.0, 0.0, 0.0});
    const Vec3d moved{axes[0].x * v.x + axes[1].x * v.y + axes[2].x * v.z, axes[0].y * v.x + axes[1].y * v.y + axes[2].y * v.z,
                      axes[0].z * v.x + axes[1].z * v.y + axes[2].z * v.z};
    EXPECT_NEAR(moved.x, 10.0, 1e-6);
    EXPECT_NEAR(moved.y, 0.0, 1e-6);
    EXPECT_NEAR(moved.z, 0.0, 1e-6);
    int nonZero = 0;
    for (int i = 0; i < 3; i++) nonZero += std::abs(v[i]) > 1e-6 ? 1 : 0;
    EXPECT_GT(nonZero, 1);
}

TEST(TitleGizmo3DTest, LayerAndLetterWriteTheSameValuesOnAnUnturnedLayer) {
    const RotationModel glyph = RotationModel::glyph(Mat3d());
    for (int i = 0; i < 3; i++) {
        const Mat3d w = r3d::turnAbout(Mat3d(), r3d::compAxis(i), 30.0);
        // rotZ / rotate are clockwise-positive
        const Vec3d want{i == 0 ? 30.0 : 0.0, i == 1 ? 30.0 : 0.0, i == 2 ? -30.0 : 0.0};
        for (const RotationModel& model : {RotationModel::layer(), glyph}) {
            const Vec3d got = model.fromOrient(w, {0.0, 0.0, 0.0});
            for (int c = 0; c < 3; c++) EXPECT_NEAR(got[c], want[c], 1e-6);
        }
    }
}

// ---- the schematic on the canvas (S82 AC14) ----

// The drawn arms relative to the centre are the same for any position, zoom, camera and object
// rotation; the centre sits on the device-pixel grid.
// Red control: drawing an arm as the projection of its world axis (Pachislop before ecaa136) makes
// the arms depend on the zoom and the camera.
TEST(TitleGizmo3DTest, ArmEndpointsIdenticalEverywhere) {
    std::mt19937 random(82);
    // screen coordinates stay within a few thousand px, where a float keeps 1e-3 px
    std::uniform_real_distribution<double> pos(-1000.0, 1000.0), zoom(0.1, 4.0), angle(-180.0, 180.0), focal(400.0, 1200.0);
    TitleGizmo3D gizmo;
    for (int n = 0; n < 50; n++) {
        const CanvasXform view = canvas((float)zoom(random), ImVec2((float)(pos(random) * 0.4), (float)(pos(random) * 0.4)));
        const Vec3d origin{pos(random), pos(random), pos(random) * 0.1};
        Gizmo3DCamera camera = Gizmo3DCamera::frontOn(view, n % 2 == 0 ? 1.0f : 2.0f);
        if (n % 3 == 0) {
            // a perspective camera looking down -z from `distance`
            const double distance = focal(random) + 1500.0, f = focal(random);
            camera.project = [view, distance, f](const Vec3d& w, ImVec2& s) {
                const double depth = distance - w.z;
                if (depth <= 1.0) return false;
                s = view.toScreen(TitleVec2{w.x * f / depth, w.y * f / depth});
                return true;
            };
        }
        const Gizmo3DTarget target = layerGizmo3DTarget("0000000a", origin, origin, {angle(random), angle(random), angle(random)}, 10.0);
        ASSERT_TRUE(gizmo.layout(target, camera));
        const ImVec2 c = gizmo.centre();
        EXPECT_NEAR(c.x * camera.pixelRatio, std::round(c.x * camera.pixelRatio), 1e-3);
        EXPECT_NEAR(c.y * camera.pixelRatio, std::round(c.y * camera.pixelRatio), 1e-3);
        for (int axis = 0; axis < 3; axis++) {
            const ImVec2 end = gizmo.armEnd(axis), arm = r3d::schematicArm(axis);
            EXPECT_NEAR(end.x - c.x, arm.x, 1e-3) << "case " << n << " axis " << axis;
            EXPECT_NEAR(end.y - c.y, arm.y, 1e-3) << "case " << n << " axis " << axis;
        }
    }
}

TEST(TitleGizmo3DTest, NotDrawnBehindTheCamera) {
    Gizmo3DCamera camera;
    camera.project = [](const Vec3d&, ImVec2&) { return false; };
    TitleGizmo3D gizmo;
    EXPECT_FALSE(gizmo.layout(layerGizmo3DTarget("0000000a", {}, {}, {}, 10.0), camera));
    EXPECT_FALSE(gizmo.visible());
    EXPECT_EQ(gizmo.hitTest(ImVec2(0.0f, 0.0f)), Gizmo3DHandle::None);
    EXPECT_FALSE(gizmo.begin(Gizmo3DHandle::MoveX, ImVec2(0.0f, 0.0f), {}));
}

// Where an arc crosses a shaft both stay grabbable by their own zones: the shaft from 0.74 of the
// arm outwards, the arc at 0.6; heads have a 4 px margin; between strokes nothing is hit.
// Red control: a shaft hit from the centre (grab at 0) takes the arcs' crossing at 50 px.
TEST(TitleGizmo3DTest, ArcAndShaftCrossingHitByTheirOwnZones) {
    TitleGizmo3D gizmo;
    ASSERT_TRUE(gizmo.layout(layerGizmo3DTarget("0000000a", {}, {}, {}, 10.0), Gizmo3DCamera::frontOn(canvas(1.0f, ImVec2(400.0f, 300.0f)))));
    const ImVec2 c = gizmo.centre();
    const auto at = [&](float dx, float dy) { return ImVec2(c.x + dx, c.y + dy); };
    // on the X arm where the Y and Z arcs end (50 px): an arc
    const Gizmo3DHandle crossing = gizmo.hitTest(at(50.0f, 0.0f));
    EXPECT_TRUE(crossing == Gizmo3DHandle::RotateY || crossing == Gizmo3DHandle::RotateZ);
    // the X shaft beyond 0.74 of the arm
    EXPECT_EQ(gizmo.hitTest(at(66.0f, 0.0f)), Gizmo3DHandle::MoveX);
    EXPECT_EQ(gizmo.hitTest(at(66.0f, 8.0f)), Gizmo3DHandle::MoveX);
    // the Y shaft and the Z shaft likewise
    EXPECT_EQ(gizmo.hitTest(at(0.0f, -66.0f)), Gizmo3DHandle::MoveY);
    EXPECT_EQ(gizmo.hitTest(at(-37.0f, 37.0f)), Gizmo3DHandle::MoveZ);
    // the Z arc (between X and Y) in its middle
    EXPECT_EQ(gizmo.hitTest(at(35.36f, -35.36f)), Gizmo3DHandle::RotateZ);
    // the X head and its 4 px margin past the tip
    EXPECT_EQ(gizmo.hitTest(at(83.0f, 0.0f)), Gizmo3DHandle::MoveX);
    EXPECT_EQ(gizmo.hitTest(at(87.0f, 0.0f)), Gizmo3DHandle::MoveX);
    EXPECT_EQ(gizmo.hitTest(at(90.0f, 0.0f)), Gizmo3DHandle::None);
    // the centre square and the ring
    EXPECT_EQ(gizmo.hitTest(at(5.0f, 5.0f)), Gizmo3DHandle::MovePlane);
    EXPECT_EQ(gizmo.hitTest(at(0.0f, 114.0f)), Gizmo3DHandle::FreeRotate);
    // between the strokes the object stays draggable
    EXPECT_EQ(gizmo.hitTest(at(20.0f, -65.0f)), Gizmo3DHandle::None);
}

// The Z arrow moves at the X arrow's rate per px; arrows measure their own arm's travel.
// Red control: measuring the Z drag along x only halves (cos 45°) its distance.
TEST(TitleGizmo3DTest, ZArrowMovesAtTheXArrowsRate) {
    TitleGizmo3D gizmo;
    const Vec3d position{10.0, 20.0, 5.0};
    ASSERT_TRUE(gizmo.layout(layerGizmo3DTarget("0000000a", position, position, {}, 10.0), Gizmo3DCamera::frontOn(canvas(2.0f, ImVec2(400.0f, 300.0f)))));
    EXPECT_NEAR(gizmo.pxPerUnit(), 2.0, 1e-6);
    const ImVec2 c = gizmo.centre();
    const ImVec2 x(c.x + 70.0f, c.y);
    ASSERT_EQ(gizmo.hitTest(x), Gizmo3DHandle::MoveX);
    ASSERT_TRUE(gizmo.begin(Gizmo3DHandle::MoveX, x, {}));
    std::vector<FieldEdit> edits = gizmo.update(ImVec2(x.x + 30.0f, x.y + 9.0f), {});
    const FieldEdit* p = find(edits, "transform.position");
    ASSERT_NE(p, nullptr);
    EXPECT_NEAR(p->value[0].get<double>(), 10.0 + 15.0, 1e-4);
    EXPECT_NEAR(p->value[1].get<double>(), 20.0, 1e-4);
    EXPECT_NEAR(find(edits, "transform.z")->value.get<double>(), 5.0, 1e-4);
    gizmo.end();

    const float d = 30.0f * (float)std::sqrt(0.5);
    const ImVec2 z(c.x - 36.0f, c.y + 36.0f);
    ASSERT_EQ(gizmo.hitTest(z), Gizmo3DHandle::MoveZ);
    ASSERT_TRUE(gizmo.begin(Gizmo3DHandle::MoveZ, z, {}));
    edits = gizmo.update(ImVec2(z.x - d, z.y + d), {});
    EXPECT_NEAR(find(edits, "transform.z")->value.get<double>(), 5.0 + 15.0, 1e-4);
    EXPECT_NEAR(find(edits, "transform.position")->value[0].get<double>(), 10.0, 1e-4);
    // Shift: the target's move step
    GizmoModifiers shift;
    shift.shift = true;
    edits = gizmo.update(ImVec2(z.x - d * 0.9f, z.y + d * 0.9f), shift);
    EXPECT_NEAR(find(edits, "transform.z")->value.get<double>(), 5.0 + 10.0, 1e-9);
    EXPECT_EQ(gizmo.actionName(), "Move Z");
}

// The ring is a trackball at 0.4° per px; Shift steps by 15°; an arc turns about its axis.
TEST(TitleGizmo3DTest, RingAndArcDrags) {
    TitleGizmo3D gizmo;
    ASSERT_TRUE(gizmo.layout(layerGizmo3DTarget("0000000a", {}, {}, {}, 10.0), Gizmo3DCamera::frontOn(canvas(1.0f, ImVec2(400.0f, 300.0f)))));
    const ImVec2 c = gizmo.centre();
    const ImVec2 ring(c.x, c.y + 114.0f);
    ASSERT_EQ(gizmo.hitTest(ring), Gizmo3DHandle::FreeRotate);
    ASSERT_TRUE(gizmo.begin(Gizmo3DHandle::FreeRotate, ring, {}));
    std::vector<FieldEdit> edits = gizmo.update(ImVec2(ring.x + 50.0f, ring.y), {});
    EXPECT_NEAR(find(edits, "transform.rotation_y")->value.get<double>(), 20.0, 1e-6);
    EXPECT_NEAR(find(edits, "transform.rotation_x")->value.get<double>(), 0.0, 1e-6);
    GizmoModifiers shift;
    shift.shift = true;
    edits = gizmo.update(ImVec2(ring.x + 50.0f, ring.y), shift);
    EXPECT_NEAR(find(edits, "transform.rotation_y")->value.get<double>(), 15.0, 1e-6);
    EXPECT_NE(gizmo.readout(Gizmo3DHandle::None).find("Y 15"), std::string::npos);
    gizmo.end();

    // along the Z arc (from X toward Y: counter-clockwise on screen), 30° with Shift:
    // clockwise-positive `rotation` = -30
    const auto onArc = [&](double degrees) {
        const double a = degrees * DEG;
        return ImVec2((float)(c.x + 50.0 * std::cos(a)), (float)(c.y - 50.0 * std::sin(a)));
    };
    ASSERT_EQ(gizmo.hitTest(onArc(45.0)), Gizmo3DHandle::RotateZ);
    ASSERT_TRUE(gizmo.begin(Gizmo3DHandle::RotateZ, onArc(45.0), shift));
    for (int step = 1; step <= 10; step++) edits = gizmo.update(onArc(45.0 + 3.0 * step), shift);
    EXPECT_NEAR(find(edits, "transform.rotation")->value.get<double>(), -30.0, 1e-6);
    EXPECT_NEAR(find(edits, "transform.rotation_x")->value.get<double>(), 0.0, 1e-6);
    EXPECT_NEAR(find(edits, "transform.rotation_y")->value.get<double>(), 0.0, 1e-6);
}
