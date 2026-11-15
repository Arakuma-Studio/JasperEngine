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
#include "titleart/designer/TitleDesignerHooks.h"
#include "titleart/designer/TitleRows.h"
#include "titleart/three_d/Title3DEditor.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include "support/PosixCompat.h"

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

// R51 "Inherited": a layer's lighting values it has not set come from the document, a part's
// environment from the layer, then the document
TEST(Title3DDesignerTest, InheritedLightingValues) {
    registerTitle3DDesigner();
    std::string id;
    const ta::TitleArtDoc doc = threeD(id);
    const ta::Json* layer = titledesigner::layerById(doc, id);
    ASSERT_NE(layer, nullptr);
    const ta::Json lighting = (*layer)["style_3d"]["lighting"];
    EXPECT_FALSE(lighting.contains("exposure")) << lighting.dump();
    EXPECT_FALSE(lighting.contains("tone_mapping")) << lighting.dump();
    const InheritanceRule* exposure = inheritanceOf("lighting_3d", "exposure", nullptr, nullptr);
    ASSERT_NE(exposure, nullptr);
    ASSERT_TRUE(exposure->sparse);
    EXPECT_EQ(exposure->valueAt(doc, {id, "style_3d.lighting.exposure"}, "exposure"), ta::Json(1.0)) << doc.root["scene_3d"].dump();
    const InheritanceRule* tone = inheritanceOf("lighting_3d", "tone_mapping", nullptr, nullptr);
    ASSERT_NE(tone, nullptr);
    EXPECT_EQ(tone->valueAt(doc, {id, "style_3d.lighting.tone_mapping"}, "tone_mapping"), ta::Json("neutral"));
    const InheritanceRule* env = inheritanceOf("environment_3d_overrides", "intensity", nullptr, nullptr);
    ASSERT_NE(env, nullptr);
    EXPECT_EQ(env->valueAt(doc, {id, "style_3d.face.environment.intensity"}, "intensity"), ta::Json(1.0));
}

// reading a sparse member a level has not set leaves the document as it was: an inserted null
// made the row look "set here" with an empty value. Red control: the lookup indexing the object
// (operator[]) writes the key
TEST(Title3DDesignerTest, ReadingASparseMemberDoesNotInsertIt) {
    std::string id;
    const ta::TitleArtDoc doc = threeD(id);
    const ta::Json* layer = titledesigner::layerById(doc, id);
    ASSERT_NE(layer, nullptr);
    const ta::Json* value = ta::getTitleArtField(*layer, "layer", "style_3d.lighting.exposure");
    ASSERT_NE(value, nullptr);
    EXPECT_TRUE(value->is_null());
    EXPECT_FALSE((*layer)["style_3d"]["lighting"].contains("exposure")) << (*layer)["style_3d"]["lighting"].dump();
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
// R54's Link: S80's link_material accepted only S80's material kind
TEST(Title3DDesignerTest, LinkMaterial3D) {
    std::string id;
    ta::TitleArtDoc doc = threeD(id);
    const auto apply = [&doc](const ta::Json& op) {
        TitleOpsResult result = TitleArtOps::apply(doc, ta::Json::array({op}));
        EXPECT_TRUE(result.ok) << result.error();
        if (result.ok) doc = result.doc;
        return result.ok;
    };
    const auto member = [&doc, &id](const char* path) {
        ta::Json out;
        ta::forEachLayer(doc.root, [&](const ta::Json& layer, const ta::Json*, int) {
            if (layer.value("id", std::string()) == id) {
                const ta::Json& style = layer.at("style_3d");
                out = std::string(path) == "face" ? style.at("face") : style.at("bands").at(0).at("material");
            }
            return true;
        });
        return out;
    };
    ASSERT_TRUE(apply({{"op", "link_material"}, {"layer", "Title"}, {"material", "style_3d.face"}, {"name", "Gold"}}));
    const std::string key = member("face").value("link", std::string());
    EXPECT_FALSE(key.empty()) << "linked";
    ASSERT_TRUE(apply({{"op", "add_band"}, {"layer", "Title"}, {"preset", "bright_ring"}}));
    ASSERT_TRUE(apply({{"op", "link_material"}, {"layer", "Title"}, {"material", "style_3d.bands[0].material"}, {"link", key}}));
    EXPECT_EQ(member("band"), member("face")) << "the new member takes the group's material";
    ASSERT_TRUE(apply({{"op", "set"}, {"layer", "Title"}, {"values", {{"style_3d.face.roughness", 0.8}}}}));
    EXPECT_DOUBLE_EQ(member("band").value("roughness", 0.0), 0.8) << "editing one keeps the group equal";
    ASSERT_TRUE(apply({{"op", "unlink_material"}, {"layer", "Title"}, {"material", "style_3d.bands[0].material"}}));
    EXPECT_EQ(member("band").value("link", std::string("x")), "");
}

// R52: the outline is of the hovered band, and follows its reach
TEST(Title3DDesignerTest, BandOutlineFollowsTheBand) {
    std::string id;
    ta::TitleArtDoc doc = threeD(id);
    TitleOpsResult result = TitleArtOps::apply(doc, ta::Json::array({ta::Json{{"op", "add_band"}, {"layer", "Title"}, {"values", {{"grow", 2.0}}}},
                                                                     ta::Json{{"op", "add_band"}, {"layer", "Title"}, {"values", {{"grow", 24.0}}}}}));
    ASSERT_TRUE(result.ok) << result.error();
    doc = result.doc;
    const auto extent = [](const std::vector<ta::TitleVec2>& lines) {
        double minX = 1e9, maxX = -1e9;
        for (const ta::TitleVec2& p : lines) {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
        }
        return maxX - minX;
    };
    const auto thin = title3DBandOutline(doc, id, 0, nullptr), wide = title3DBandOutline(doc, id, 1, nullptr);
    ASSERT_GE(thin.size(), 8u);
    ASSERT_GE(wide.size(), 8u);
    EXPECT_EQ(thin.size() % 2, 0u) << "line pairs";
    EXPECT_GT(extent(wide), extent(thin) + 20.0) << "the larger grow reaches further";
    EXPECT_TRUE(title3DBandOutline(doc, id, 5, nullptr).empty()) << "no such band";
    EXPECT_TRUE(title3DBandOutline(doc, "nope", 0, nullptr).empty());
}

// R55's Import HDRI...: a copy under Assets/TitleArt/HDRI, once per file
TEST(Title3DDesignerTest, ImportHdriCopiesOnce) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("je-hdri-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path assets = root / "project" / "assets", outside = root / "outside";
    fs::create_directories(assets);
    fs::create_directories(outside);
    const auto write = [](const fs::path& path, const std::string& bytes) { std::ofstream(path, std::ios::binary) << bytes; };
    write(outside / "studio.hdr", "first");
    std::string error;
    const std::string first = copyTitleHdriIntoAssets(outside / "studio.hdr", assets, &error);
    EXPECT_EQ(fs::path(first), assets / "TitleArt" / "HDRI" / "studio.hdr") << error;
    EXPECT_TRUE(fs::exists(first));
    EXPECT_EQ(copyTitleHdriIntoAssets(outside / "studio.hdr", assets, &error), first) << "the same file is the same copy";
    fs::create_directories(outside / "b");
    write(outside / "b" / "studio.hdr", "second");
    const std::string second = copyTitleHdriIntoAssets(outside / "b" / "studio.hdr", assets, &error);
    EXPECT_EQ(fs::path(second).filename(), "studio 2.hdr") << "another file of the name gets a number";
    EXPECT_EQ(copyTitleHdriIntoAssets(first, assets, &error), first) << "a file of the project stays where it is";
    error.clear();
    EXPECT_TRUE(copyTitleHdriIntoAssets(outside / "readme.txt", assets, &error).empty());
    EXPECT_NE(error.find(".hdr or .exr"), std::string::npos);
    fs::remove_all(root);
}

// a preview's render asks for engine frames from a window; the editor draws only on activity, so
// the wish has to outlive the drain or the render stops until the mouse moves
TEST(Title3DDesignerTest, EngineFrameRequestsKeepTheEditorDrawingUntilTheyStop) {
    runEngineFrameRequests();
    runEngineFrameRequests();
    EXPECT_FALSE(engineFramesWanted());
    int runs = 0;
    requestEngineFrame([&runs] { runs++; });
    EXPECT_TRUE(engineFramesWanted());
    runEngineFrameRequests();
    EXPECT_EQ(runs, 1);
    EXPECT_TRUE(engineFramesWanted()) << "drained in this frame: the next frame asks again";
    runEngineFrameRequests();
    EXPECT_FALSE(engineFramesWanted()) << "nothing asked: idle again, a finished preview costs nothing";
}

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
