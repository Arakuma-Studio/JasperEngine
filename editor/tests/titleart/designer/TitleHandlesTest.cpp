// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include "titleart/designer/TitleHandles.h"

#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    constexpr double PI = 3.14159265358979323846;
    const char* LAYER = "0000000a";

    CanvasXform view() {
        CanvasXform v;
        v.zoom = 1.0f;
        v.origin = ImVec2(400.0f, 300.0f);
        return v;
    }

    HandleFrame frame() {
        HandleFrame f;
        f.layerId = LAYER;
        f.localBox = {-100.0, -25.0, 200.0, 50.0};
        f.sourceSize = 50.0;
        return f;
    }

    // a document with one text layer holding a drop shadow (Use Global Light per `global`), an
    // outer glow, a face material and a distort
    Json document(bool global) {
        Json root = Json::parse(R"({
            "global_light": {"angle": 120, "altitude": 30},
            "layers": [{
                "id": "0000000a", "type": "text",
                "transform": {"position": [0, 0], "rotation": 0, "scale": 1, "flip_x": false, "flip_y": false},
                "effects": [
                    {"id": "0000000b", "type": "drop_shadow", "enabled": true,
                     "params": {"angle": 60, "distance": 10, "size": 14, "spread": 0.1, "color": "#000000", "opacity": 0.75, "use_global_light": true}},
                    {"id": "0000000c", "type": "outer_glow", "enabled": true, "params": {"fill": "solid", "size": 40}}
                ],
                "style": {"face": {
                    "fill": {"kind": "linear", "angle": 0, "scale": 1, "offset": 0},
                    "bevel": {"enabled": true, "use_global_light": false, "light_angle": 120, "light_altitude": 30},
                    "satin": {"enabled": false}
                }},
                "warp": {"style": "none", "bend": 0},
                "distort": {"mode": "corners", "corners": [[0, 0], [0, 0], [0, 0], [0, 0]]},
                "masks": [{"id": "0000000d", "type": "gradient", "linked": true, "position": [0, 0], "angle": 0, "length": 100}]
            }]
        })");
        root["layers"][0]["effects"][0]["params"]["use_global_light"] = global;
        return root;
    }

    NodeRef effectNode(const char* id) {
        NodeRef n;
        n.kind = NodeKind::Effect;
        n.layer = LAYER;
        n.item = id;
        return n;
    }

    const FieldEdit* find(const std::vector<FieldEdit>& edits, const std::string& layer, const std::string& path) {
        for (const FieldEdit& e : edits) {
            if (e.layer == layer && e.path == path) return &e;
        }
        return nullptr;
    }

    ImVec2 screen(TitleVec2 p) { return view().toScreen(p); }

    // a drag of handle `index` from where it is drawn to `to`
    std::vector<FieldEdit> dragTo(TitleHandles& h, int index, ImVec2 to, const GizmoModifiers& modifiers = GizmoModifiers()) {
        EXPECT_TRUE(h.begin(index, h.screenPosition(index), modifiers));
        std::vector<FieldEdit> edits = h.update(to, modifiers);
        return edits;
    }

    int indexOf(const TitleHandles& h, HandleKind kind) {
        for (int i = 0; i < h.count(); i++) {
            if (h.kind(i) == kind) return i;
        }
        return -1;
    }

}

// A drop shadow following the global light: its arrow edits global_light.angle (a document
// member, layer "") through the Away relation, never the shadow's own angle, and says so.
// Red control: removing the global-light branch (h.global never set in addHint) writes
// effects[0].params.angle = 30 and no global_light.angle, and this test fails.
TEST(TitleHandlesTest, DropShadowWithGlobalLightEditsTheGlobalAngle) {
    const Json root = document(true);
    TitleHandles h;
    h.setView(view());
    h.addNode(root, effectNode("0000000b"), frame());
    ASSERT_EQ(h.count(), 1);
    EXPECT_EQ(h.kind(0), HandleKind::Direction);
    EXPECT_EQ(h.label(0).rfind("Global Light 120\xC2\xB0", 0), 0u) << h.label(0);
    // 40 px from the box centre, 30° clockwise: the shadow falls there, so the light comes from 150°
    const double a = 30.0 * PI / 180.0;
    const ImVec2 to(400.0f + (float)(40.0 * std::cos(a)), 300.0f + (float)(40.0 * std::sin(a)));
    const std::vector<FieldEdit> edits = dragTo(h, 0, to);
    const FieldEdit* global = find(edits, "", "global_light.angle");
    ASSERT_NE(global, nullptr);
    EXPECT_NEAR(global->value.get<double>(), 150.0, 1e-9);
    EXPECT_EQ(find(edits, LAYER, "effects[0].params.angle"), nullptr);
    // the distance stays the layer's own
    const FieldEdit* distance = find(edits, LAYER, "effects[0].params.distance");
    ASSERT_NE(distance, nullptr);
    EXPECT_NEAR(distance->value.get<double>(), 40.0, 1e-9);
    EXPECT_EQ(h.label(0).rfind("Global Light 150\xC2\xB0", 0), 0u) << h.label(0);
    EXPECT_EQ(h.gestureName(), "Change Global Light");
}

// With Use Global Light off, the same drag edits the shadow's own angle.
TEST(TitleHandlesTest, DirectionWithoutGlobalLightEditsItsParam) {
    const Json root = document(false);
    TitleHandles h;
    h.setView(view());
    h.addNode(root, effectNode("0000000b"), frame());
    ASSERT_EQ(h.count(), 1);
    EXPECT_EQ(h.label(0).rfind("Angle 60\xC2\xB0", 0), 0u) << h.label(0);
    const double a = 30.0 * PI / 180.0;
    const std::vector<FieldEdit> edits = dragTo(h, 0, ImVec2(400.0f + (float)(40.0 * std::cos(a)), 300.0f + (float)(40.0 * std::sin(a))));
    ASSERT_NE(find(edits, LAYER, "effects[0].params.angle"), nullptr);
    EXPECT_NEAR(find(edits, LAYER, "effects[0].params.angle")->value.get<double>(), 30.0, 1e-9);
    EXPECT_EQ(find(edits, "", "global_light.angle"), nullptr);
    EXPECT_EQ(h.gestureName(), "Change Drop Shadow Angle");
    // Shift: 15° steps
    GizmoModifiers shift;
    shift.shift = true;
    const double b = 37.0 * PI / 180.0;
    const std::vector<FieldEdit> stepped = h.update(ImVec2(400.0f + (float)(40.0 * std::cos(b)), 300.0f + (float)(40.0 * std::sin(b))), shift);
    EXPECT_NEAR(find(stepped, LAYER, "effects[0].params.angle")->value.get<double>(), 30.0, 1e-9);
}

// The inverse of angleFromGlobalLight for every relation and convention.
TEST(TitleHandlesTest, GlobalAngleInvertsTheBinding) {
    for (ta::LightRelation relation : {ta::LightRelation::From, ta::LightRelation::Away, ta::LightRelation::Floor}) {
        for (AngleConvention convention : {AngleConvention::ClockwiseFromRight, AngleConvention::CounterClockwiseFromRight}) {
            GlobalLightBinding b;
            b.angleKey = "angle";
            b.relation = relation;
            b.convention = convention;
            for (double g : {120.0, -45.0, 0.0, 179.0}) EXPECT_NEAR(globalAngleForParam(b, ta::angleFromGlobalLight(b, g)), g, 1e-9);
        }
    }
}

// A face fill's gradient line: the end handle sets angle and scale, the line the offset; every
// drag writes all three.
// Red control: swapping the line drag's sign (offset - along / span) gives -0.25.
TEST(TitleHandlesTest, GradientLineEditsAngleScaleOffset) {
    const Json root = document(true);
    TitleHandles h;
    h.setView(view());
    NodeRef face;
    face.kind = NodeKind::Part;
    face.layer = LAYER;
    face.part = "face";
    h.addNode(root, face, frame());
    const int end = indexOf(h, HandleKind::GradientEnd), line = indexOf(h, HandleKind::GradientLine);
    ASSERT_GE(end, 0);
    ASSERT_GE(line, 0);
    ASSERT_GE(indexOf(h, HandleKind::GradientStart), 0);
    // angle 0 across a 200 px box: t = 0 at x -100, t = 1 at x 100
    EXPECT_NEAR(h.screenPosition(end).x, 500.0f, 1e-3f);
    std::vector<FieldEdit> edits = dragTo(h, end, screen({0.0, -100.0}));
    EXPECT_NEAR(find(edits, LAYER, "style.face.fill.angle")->value.get<double>(), 45.0, 1e-9);
    EXPECT_NEAR(find(edits, LAYER, "style.face.fill.scale")->value.get<double>(), 0.8, 1e-9);
    ASSERT_NE(find(edits, LAYER, "style.face.fill.offset"), nullptr);
    h.end();
    // grab the line in its middle and slide it 50 px along its axis
    h.begin(line, screen({0.0, 0.0}), {});
    edits = h.update(screen({50.0, 0.0}), {});
    EXPECT_NEAR(find(edits, LAYER, "style.face.fill.offset")->value.get<double>(), 0.25, 1e-9);
    EXPECT_NEAR(find(edits, LAYER, "style.face.fill.angle")->value.get<double>(), 0.0, 1e-9);
    EXPECT_NEAR(find(edits, LAYER, "style.face.fill.scale")->value.get<double>(), 1.0, 1e-9);
    // the bevel light (its own, Use Global Light off) is a dial beside the box
    const int light = indexOf(h, HandleKind::Light);
    ASSERT_GE(light, 0);
    EXPECT_EQ(h.label(light), "Light 120\xC2\xB0 / 30\xC2\xB0");
}

// A radius (outer glow size, canvas px) is a ring around the box; a hidden param (`when`) has no handle.
TEST(TitleHandlesTest, RadiusWritesItsSize) {
    const Json root = document(true);
    TitleHandles h;
    h.setView(view());
    h.addNode(root, effectNode("0000000c"), frame());
    // the angle is a Gradient Across param: a solid glow shows only its size
    ASSERT_EQ(h.count(), 1);
    EXPECT_EQ(h.kind(0), HandleKind::Radius);
    EXPECT_NEAR(h.screenPosition(0).x, 540.0f, 1e-3f);
    const std::vector<FieldEdit> edits = dragTo(h, 0, ImVec2(560.0f, 300.0f));
    EXPECT_NEAR(find(edits, LAYER, "effects[1].params.size")->value.get<double>(), 60.0, 1e-9);
    EXPECT_EQ(h.gestureName(), "Change Outer Glow Size");
}

// Handles show only for the selected node.
TEST(TitleHandlesTest, HandlesOnlyForTheSelectedNode) {
    const Json root = document(true);
    TitleHandles h;
    h.setView(view());
    h.addNode(root, effectNode("0000000c"), frame());
    ASSERT_EQ(h.count(), 1);
    EXPECT_NE(find(dragTo(h, 0, ImVec2(560.0f, 300.0f)), LAYER, "effects[1].params.size"), nullptr);
    h.end();
    h.clear();
    h.addNode(root, effectNode("ffffffff"), frame());
    EXPECT_EQ(h.count(), 0);
    NodeRef wholeDocument;
    h.addNode(root, wholeDocument, frame());
    EXPECT_EQ(h.count(), 0);
    NodeRef other = effectNode("0000000b");
    other.layer = "00000099";
    h.addNode(root, other, frame());
    EXPECT_EQ(h.count(), 0);
}

// Registry Point and Light hints (test definitions): a point writes its Vec2 (Shift snaps to the
// layer centre); a light following the global light writes global_light.angle and altitude.
class TitleHandlesRegistryTest : public ::testing::Test {
protected:
    void SetUp() override {
        ta::EffectDef point;
        point.type = "test_handles_point";
        point.label = "Test Point";
        ta::ParamDef centre;
        centre.key = "center";
        centre.label = "Center";
        centre.kind = ta::ParamKind::Vec2;
        centre.unit = ParamUnit::Px;
        centre.defaultValue = Json::array({0, 0});
        point.params = {centre};
        point.gizmos = {{ta::GizmoKind::Point, "center", nullptr}};
        ASSERT_TRUE(ta::TitleEffectRegistry::get().add(point));

        ta::EffectDef light;
        light.type = "test_handles_light";
        light.label = "Test Light";
        ta::ParamDef angle;
        angle.key = "angle";
        angle.label = "Angle";
        angle.unit = ParamUnit::Deg;
        angle.min = -180.0;
        angle.max = 360.0;
        angle.defaultValue = 45.0;
        ta::ParamDef altitude;
        altitude.key = "altitude";
        altitude.label = "Altitude";
        altitude.unit = ParamUnit::Deg;
        altitude.min = 0.0;
        altitude.max = 90.0;
        altitude.defaultValue = 30.0;
        light.params = {angle, altitude};
        light.gizmos = {{ta::GizmoKind::Light, "angle", "altitude"}};
        ASSERT_TRUE(ta::TitleEffectRegistry::get().add(ta::withGlobalLight(light, "angle", ta::LightRelation::From, "altitude")));
    }
    void TearDown() override {
        ta::TitleEffectRegistry::get().removeEffect("test_handles_point");
        ta::TitleEffectRegistry::get().removeEffect("test_handles_light");
    }
};

TEST_F(TitleHandlesRegistryTest, PointAndLightHints) {
    TitleHandles h;
    h.setView(view());
    const GlobalLight light{120.0, 30.0};
    h.addEffect(frame(), *ta::findEffect("test_handles_point"), Json::parse(R"({"center": [10, 20]})"), "effects[0].params", light);
    ASSERT_EQ(h.count(), 1);
    EXPECT_EQ(h.kind(0), HandleKind::Point);
    EXPECT_NEAR(h.screenPosition(0).x, 410.0f, 1e-3f);
    EXPECT_NEAR(h.screenPosition(0).y, 280.0f, 1e-3f);
    std::vector<FieldEdit> edits = dragTo(h, 0, ImVec2(440.0f, 270.0f));
    const FieldEdit* centre = find(edits, LAYER, "effects[0].params.center");
    ASSERT_NE(centre, nullptr);
    EXPECT_NEAR(centre->value[0].get<double>(), 40.0, 1e-9);
    EXPECT_NEAR(centre->value[1].get<double>(), 30.0, 1e-9);
    // Shift near the layer centre: exactly on it
    GizmoModifiers shift;
    shift.shift = true;
    edits = h.update(ImVec2(395.0f, 296.0f), shift);
    EXPECT_NEAR(find(edits, LAYER, "effects[0].params.center")->value[0].get<double>(), 0.0, 1e-9);
    EXPECT_NEAR(find(edits, LAYER, "effects[0].params.center")->value[1].get<double>(), 0.0, 1e-9);
    h.end();

    h.clear();
    h.addEffect(frame(), *ta::findEffect("test_handles_light"), Json::parse(R"({"angle": 45, "altitude": 10, "use_global_light": true})"),
                "effects[0].params", light);
    ASSERT_EQ(h.count(), 1);
    EXPECT_EQ(h.kind(0), HandleKind::Light);
    EXPECT_EQ(h.label(0), "Global Light 120\xC2\xB0 / 30\xC2\xB0");
    // the dial sits up-right of the box's top-right corner (500, 275): centre (536, 239)
    const ImVec2 dial(536.0f, 239.0f);
    edits = dragTo(h, 0, ImVec2(dial.x + 26.0f, dial.y));
    EXPECT_NEAR(find(edits, "", "global_light.angle")->value.get<double>(), 0.0, 1e-9);
    EXPECT_NEAR(find(edits, "", "global_light.altitude")->value.get<double>(), 30.0, 1e-9);
    EXPECT_EQ(find(edits, LAYER, "effects[0].params.angle"), nullptr);
    // Shift sets the altitude: half the radius from the centre is 60°
    edits = h.update(ImVec2(dial.x + 13.0f, dial.y), shift);
    EXPECT_NEAR(find(edits, "", "global_light.altitude")->value.get<double>(), 60.0, 1e-9);
    h.end();

    h.clear();
    h.addEffect(frame(), *ta::findEffect("test_handles_light"), Json::parse(R"({"angle": 45, "altitude": 10, "use_global_light": false})"),
                "effects[0].params", light);
    edits = dragTo(h, 0, ImVec2(dial.x, dial.y - 20.0f));
    EXPECT_NEAR(find(edits, LAYER, "effects[0].params.angle")->value.get<double>(), 90.0, 1e-9);
    EXPECT_NEAR(find(edits, LAYER, "effects[0].params.altitude")->value.get<double>(), 10.0, 1e-9);
    EXPECT_EQ(find(edits, "", "global_light.angle"), nullptr);
}

// An angle fill's dial and the bevel's own light.
TEST(TitleHandlesTest, AngleDialAndBevelLight) {
    Json root = document(true);
    root["layers"][0]["style"]["face"]["fill"] = Json::parse(R"({"kind": "angle", "angle": 0, "scale": 1, "offset": 0})");
    TitleHandles h;
    h.setView(view());
    NodeRef face;
    face.kind = NodeKind::Part;
    face.layer = LAYER;
    face.part = "face";
    h.addNode(root, face, frame());
    const int dial = indexOf(h, HandleKind::AngleDial), light = indexOf(h, HandleKind::Light);
    ASSERT_GE(dial, 0);
    ASSERT_GE(light, 0);
    // straight down on screen is 90° clockwise
    std::vector<FieldEdit> edits = dragTo(h, dial, ImVec2(400.0f, 334.0f));
    EXPECT_NEAR(find(edits, LAYER, "style.face.fill.angle")->value.get<double>(), 90.0, 1e-9);
    h.end();
    const ImVec2 centre(536.0f, 239.0f);
    edits = dragTo(h, light, ImVec2(centre.x - 20.0f, centre.y));
    EXPECT_NEAR(find(edits, LAYER, "style.face.bevel.light_angle")->value.get<double>(), 180.0, 1e-9);
    EXPECT_NEAR(find(edits, LAYER, "style.face.bevel.light_altitude")->value.get<double>(), 30.0, 1e-9);
    EXPECT_EQ(find(edits, "", "global_light.angle"), nullptr);
    EXPECT_EQ(h.gestureName(), "Change Bevel Light");
}

// Distort corners move by fractions of the frame (Shift mirrors), Reset zeroes them.
TEST(TitleHandlesTest, DistortCornersAndReset) {
    const Json root = document(true);
    TitleHandles h;
    h.setView(view());
    NodeRef layer;
    layer.kind = NodeKind::Layer;
    layer.layer = LAYER;
    h.addNode(root, layer, frame());
    ASSERT_EQ(h.count(), 5);
    EXPECT_EQ(h.kind(0), HandleKind::DistortPoint);
    EXPECT_EQ(h.kind(4), HandleKind::DistortReset);
    // corner 0 is the top-left
    EXPECT_NEAR(h.screenPosition(0).x, 300.0f, 1e-3f);
    EXPECT_NEAR(h.screenPosition(0).y, 275.0f, 1e-3f);
    GizmoModifiers shift;
    shift.shift = true;
    std::vector<FieldEdit> edits = dragTo(h, 0, ImVec2(320.0f, 275.0f), shift);
    const FieldEdit* corners = find(edits, LAYER, "distort.corners");
    ASSERT_NE(corners, nullptr);
    EXPECT_NEAR(corners->value[0][0].get<double>(), 0.1, 1e-9);
    EXPECT_NEAR(corners->value[1][0].get<double>(), -0.1, 1e-9);
    EXPECT_NEAR(corners->value[2][0].get<double>(), 0.0, 1e-9);
    h.end();
    edits = dragTo(h, 4, h.screenPosition(4));
    ASSERT_NE(find(edits, LAYER, "distort.corners"), nullptr);
    ASSERT_NE(find(edits, LAYER, "distort.mesh"), nullptr);
    EXPECT_EQ(find(edits, LAYER, "distort.mesh")->value.size(), 16u);
    EXPECT_EQ(h.gestureName(), "Reset Distort");
}

// Shape handles: corner, points, inner, tail; and the warp bend.
TEST(TitleHandlesTest, ShapeHandlesAndWarpBend) {
    TitleHandles h;
    h.setView(view());
    // a 200 x 100 rect with a 10 % (10 px) corner: on-curve points where the arcs meet the edges
    const std::vector<TitleVec2> rect = {{-90, 50}, {90, 50}, {100, 40}, {100, -40}, {90, -50}, {-90, -50}, {-100, -40}, {-100, 40}};
    h.addShape(frame(), Json::parse(R"({"shape": "rect", "size": 100, "aspect": 2, "corner": 10})"), "", rect);
    ASSERT_EQ(h.count(), 1);
    EXPECT_EQ(h.kind(0), HandleKind::ShapeCorner);
    EXPECT_EQ(h.label(0), "Corner 10 %");
    std::vector<FieldEdit> edits = dragTo(h, 0, screen({-80.0, 50.0}));
    EXPECT_NEAR(find(edits, LAYER, "corner")->value.get<double>(), 20.0, 1e-9);
    h.end();

    h.clear();
    const std::vector<TitleVec2> star = {{0, 50}, {10, 15}, {48, 15}, {17, -6}, {29, -40}, {0, -19}, {-29, -40}, {-17, -6}, {-48, 15}, {-10, 15}};
    h.addShape(frame(), Json::parse(R"({"shape": "star", "size": 100, "points": 5, "inner": 0.4})"), "masks[0].shape.", star);
    const int inner = indexOf(h, HandleKind::ShapeInner), points = indexOf(h, HandleKind::ShapePoints);
    ASSERT_GE(inner, 0);
    ASSERT_GE(points, 0);
    edits = dragTo(h, points, ImVec2(h.screenPosition(points).x + 25.0f, h.screenPosition(points).y));
    EXPECT_EQ(find(edits, LAYER, "masks[0].shape.points")->value.get<int>(), 7);
    h.end();
    // the handle sits on the valley nearest the centre: 1.5 times as far is 1.5 times the inner radius
    const ImVec2 v = h.screenPosition(inner);
    edits = dragTo(h, inner, ImVec2(400.0f + (v.x - 400.0f) * 1.5f, 300.0f + (v.y - 300.0f) * 1.5f));
    EXPECT_NEAR(find(edits, LAYER, "masks[0].shape.inner")->value.get<double>(), 0.6, 1e-3);
    h.end();

    h.clear();
    h.addShape(frame(), Json::parse(R"({"shape": "speech", "size": 100, "corner": 0, "tail": {"x": 0, "y": -60}})"), "", rect);
    const int tail = indexOf(h, HandleKind::ShapeTail);
    ASSERT_GE(tail, 0);
    EXPECT_NEAR(h.screenPosition(tail).y, 360.0f, 1e-3f);
    edits = dragTo(h, tail, screen({30.0, -80.0}));
    EXPECT_NEAR(find(edits, LAYER, "tail.x")->value.get<double>(), 30.0, 1e-9);
    EXPECT_NEAR(find(edits, LAYER, "tail.y")->value.get<double>(), -80.0, 1e-9);
    h.end();

    h.clear();
    h.addWarpBend(frame(), Json::parse(R"({"style": "arc", "bend": 0})"), "warp", false, false);
    ASSERT_EQ(h.count(), 1);
    // three quarters across the top edge, 22 px above it
    EXPECT_NEAR(h.screenPosition(0).x, 450.0f, 1e-3f);
    EXPECT_NEAR(h.screenPosition(0).y, 253.0f, 1e-3f);
    edits = dragTo(h, 0, ImVec2(450.0f, 233.0f));
    EXPECT_NEAR(find(edits, LAYER, "warp.bend")->value.get<double>(), 10.0, 1e-9);
    EXPECT_EQ(h.gestureName(), "Change Warp Bend");
}

// A gradient mask: its position and its direction with length.
TEST(TitleHandlesTest, GradientMaskPositionAndDirection) {
    const Json root = document(true);
    TitleHandles h;
    h.setView(view());
    NodeRef mask;
    mask.kind = NodeKind::Mask;
    mask.layer = LAYER;
    mask.item = "0000000d";
    h.addNode(root, mask, frame());
    const int dir = indexOf(h, HandleKind::Direction), point = indexOf(h, HandleKind::Point);
    ASSERT_GE(dir, 0);
    ASSERT_GE(point, 0);
    EXPECT_NEAR(h.screenPosition(dir).x, 500.0f, 1e-3f);
    std::vector<FieldEdit> edits = dragTo(h, dir, ImVec2(400.0f, 400.0f));
    EXPECT_NEAR(find(edits, LAYER, "masks[0].angle")->value.get<double>(), 90.0, 1e-9);
    EXPECT_NEAR(find(edits, LAYER, "masks[0].length")->value.get<double>(), 100.0, 1e-9);
    h.end();
    edits = dragTo(h, point, ImVec2(430.0f, 290.0f));
    EXPECT_NEAR(find(edits, LAYER, "masks[0].position")->value[0].get<double>(), 30.0, 1e-9);
    EXPECT_NEAR(find(edits, LAYER, "masks[0].position")->value[1].get<double>(), 10.0, 1e-9);
}

// The light sphere's mapping: centre overhead, rim grazing, the angle in either convention.
TEST(TitleHandlesTest, LightSphereMapping) {
    double angle = 0.0, altitude = 0.0;
    lightFromSphere(0.0, -30.0, 30.0, AngleConvention::CounterClockwiseFromRight, false, angle, altitude);
    EXPECT_DOUBLE_EQ(angle, 90.0);
    EXPECT_DOUBLE_EQ(altitude, 0.0);
    lightFromSphere(0.0, 0.0, 30.0, AngleConvention::CounterClockwiseFromRight, false, angle, altitude);
    EXPECT_DOUBLE_EQ(altitude, 90.0);
    lightFromSphere(0.0, -30.0, 30.0, AngleConvention::ClockwiseFromRight, false, angle, altitude);
    EXPECT_DOUBLE_EQ(angle, 270.0);
    lightFromSphere(20.0, -9.0, 30.0, AngleConvention::CounterClockwiseFromRight, true, angle, altitude);
    EXPECT_DOUBLE_EQ(std::fmod(angle, 15.0), 0.0);
    EXPECT_DOUBLE_EQ(std::fmod(altitude, 5.0), 0.0);
}
