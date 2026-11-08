// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S86's side of the Title Designer (docs/specs/S86-title-3d.md R55, R56; S82 R35, AC14): the
// Gizmo3DTarget of a Dimension 3D layer (transform.position / z / rotation_x / rotation_y /
// rotation, steps "Move 3D Layer" / "Rotate 3D Layer") and of its selected letters (offset, dz,
// tilt_x, tilt_y, rotation, each letter from its own value: "Tilt Letters"), none for a 2D layer;
// the environment picker's CPU tile.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/three_d/Title3DDesigner.h"
#include "titleart/three_d/Title3DEditor.h"

#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titledesigner;
namespace ta = jasperengine::titleart;

namespace {

    // a new title whose Title layer is Dimension 3D, moved and turned
    ta::TitleArtDoc threeD(std::string& layerId, const ta::Json& extra = ta::Json::object()) {
        registerTitle3DEditor();
        ta::TitleArtDoc doc = ta::makeNewTitleArt();
        ta::Json values = {{"transform.position", ta::Json::array({40, -10})}, {"transform.z", 5}, {"transform.rotation_x", 10}, {"transform.rotation", 20}};
        for (auto it = extra.begin(); it != extra.end(); ++it) values[it.key()] = it.value();
        const ta::Json ops = ta::Json::array({ta::Json{{"op", "set_dimension"}, {"layer", "Title"}, {"dimension", "3d"}},
                                              ta::Json{{"op", "set"}, {"layer", "Title"}, {"values", values}}});
        TitleOpsResult result = TitleArtOps::apply(doc, ops);
        EXPECT_TRUE(result.ok) << result.error();
        ta::forEachLayer(result.doc.root, [&](const ta::Json& layer, const ta::Json*, int) {
            if (layerId.empty()) layerId = layer.value("id", std::string());
            return true;
        });
        return result.doc;
    }

    std::map<std::string, ta::Json> byPath(const std::vector<FieldEdit>& edits) {
        std::map<std::string, ta::Json> out;
        for (const FieldEdit& e : edits) out[e.path] = e.value;
        return out;
    }

}

TEST(Title3DDesignerTest, LayerTarget) {
    std::string id;
    const ta::TitleArtDoc doc = threeD(id);
    Gizmo3DTarget target;
    ASSERT_TRUE(title3DGizmoTarget(doc, id, nullptr, target));
    EXPECT_DOUBLE_EQ(target.origin.x, 40.0);
    EXPECT_DOUBLE_EQ(target.origin.y, -10.0);
    EXPECT_DOUBLE_EQ(target.rot.x, 10.0);
    EXPECT_DOUBLE_EQ(target.rot.z, 20.0);
    EXPECT_EQ(target.moveStep, "Move 3D Layer");
    EXPECT_EQ(target.rotateStep, "Rotate 3D Layer");
    // R56: the centre square writes transform.position, the Z arrow transform.z
    auto move = byPath(target.write(Vec3d{3.0, 4.0, 5.0}, Vec3d{}, Gizmo3DGesture::Move));
    EXPECT_EQ(move["transform.position"], ta::Json::array({43.0, -6.0}));
    EXPECT_DOUBLE_EQ(move["transform.z"].get<double>(), 10.0);
    // the arcs and the ring write rotation_x, rotation_y and rotation
    auto turn = byPath(target.write(Vec3d{}, Vec3d{1.0, 2.0, 3.0}, Gizmo3DGesture::Rotate));
    EXPECT_DOUBLE_EQ(turn["transform.rotation_x"].get<double>(), 11.0);
    EXPECT_DOUBLE_EQ(turn["transform.rotation_y"].get<double>(), 2.0);
    EXPECT_DOUBLE_EQ(turn["transform.rotation"].get<double>(), 23.0);
    // the written edits apply as one batch
    TitleOpsResult applied = TitleArtOps::apply(doc, editsToOps(target.write(Vec3d{3.0, 4.0, 5.0}, Vec3d{}, Gizmo3DGesture::Move)));
    EXPECT_TRUE(applied.ok) << applied.error();
}

// each letter moves and tilts from its own value at the press. Red control: writing the
// primary letter's value to every letter gives 2 and 2 instead of 2 and 5.
TEST(Title3DDesignerTest, LettersShiftEachFromItsOwnValue) {
    std::string id;
    const ta::TitleArtDoc doc = threeD(id, {{"chars.0.offset", ta::Json::array({-1, 0})}, {"chars.1.offset", ta::Json::array({2, 0})},
                                            {"chars.1.tilt_x", 5}, {"chars.2.tilt_x", -4}});
    Gizmo3DLetters letters;
    letters.indexes = {1, 2};
    letters.centre = {10.0, 0.0};
    Gizmo3DTarget target;
    ASSERT_TRUE(title3DGizmoTarget(doc, id, &letters, target));
    EXPECT_EQ(target.moveStep, "Move Letters");
    EXPECT_EQ(target.rotateStep, "Tilt Letters");
    EXPECT_EQ(target.unit, "%");
    EXPECT_DOUBLE_EQ(target.origin.x, 10.0);
    auto move = byPath(target.write(Vec3d{3.0, 0.0, 1.0}, Vec3d{}, Gizmo3DGesture::Move));
    EXPECT_EQ(move["chars.1.offset"], ta::Json::array({5.0, 0.0}));
    EXPECT_EQ(move["chars.2.offset"], ta::Json::array({3.0, 0.0}));
    EXPECT_DOUBLE_EQ(move["chars.1.dz"].get<double>(), 1.0);
    EXPECT_EQ(move.count("chars.0.offset"), 0u) << "an unselected letter keeps its offset";
    auto tilt = byPath(target.write(Vec3d{}, Vec3d{10.0, 0.0, 0.0}, Gizmo3DGesture::Rotate));
    EXPECT_DOUBLE_EQ(tilt["chars.1.tilt_x"].get<double>(), 15.0);
    EXPECT_DOUBLE_EQ(tilt["chars.2.tilt_x"].get<double>(), 6.0);
    TitleOpsResult applied = TitleArtOps::apply(doc, editsToOps(target.write(Vec3d{}, Vec3d{10.0, 0.0, 0.0}, Gizmo3DGesture::Rotate)));
    EXPECT_TRUE(applied.ok) << applied.error();
}

TEST(Title3DDesignerTest, NoTargetFor2DOrLocked) {
    ta::TitleArtDoc doc = ta::makeNewTitleArt();
    std::string id;
    ta::forEachLayer(doc.root, [&](const ta::Json& layer, const ta::Json*, int) {
        if (id.empty()) id = layer.value("id", std::string());
        return true;
    });
    Gizmo3DTarget target;
    EXPECT_FALSE(title3DGizmoTarget(doc, id, nullptr, target)) << "a 2D layer keeps the 2D gizmo";
    std::string id3;
    const ta::TitleArtDoc locked = threeD(id3, {{"locked", true}});
    EXPECT_FALSE(title3DGizmoTarget(locked, id3, nullptr, target)) << "a locked layer has no gizmo";
    EXPECT_FALSE(title3DGizmoTarget(locked, "nope", nullptr, target));
}

// R55: a chrome ball and a gold disc over the panorama strip, lit by the environment
TEST(Title3DDesignerTest, EnvironmentTile) {
    const int w = 96, h = 64;
    const std::vector<uint8_t> studio = title3DEnvironmentTile("studio", w, h);
    ASSERT_EQ(studio.size(), (size_t)w * h * 4);
    auto pixel = [&](const std::vector<uint8_t>& image, int x, int y) {
        const size_t i = ((size_t)y * w + x) * 4;
        return std::array<int, 3>{image[i], image[i + 1], image[i + 2]};
    };
    const int top = h - h / 4;
    // the ball's centre is lit, the corner is the dark background
    EXPECT_NE(pixel(studio, (int)(w * 0.27f), top / 2), (std::array<int, 3>{30, 30, 30}));
    EXPECT_EQ(pixel(studio, 1, 1), (std::array<int, 3>{30, 30, 30}));
    // the gold disc is warmer than the chrome ball
    const auto chrome = pixel(studio, (int)(w * 0.27f), top / 2), gold = pixel(studio, (int)(w * 0.73f), top / 2);
    EXPECT_GT(gold[0] - gold[2], chrome[0] - chrome[2]);
    // different environments give different tiles
    EXPECT_NE(studio, title3DEnvironmentTile("sunset", w, h));
}
