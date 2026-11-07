// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Scene view's AI Navigation overlay (docs/specs/S19-navigation.md R33, docs/specs/S75-navigation-
// diagnostics.md R27, R38, AC14), without a GPU: its workspace settings round trip and an S19
// workspace loads with the new options at their defaults; the options off create nothing; tile
// labels read the tiles' last builds; Color By Islands greys what the query's start cannot reach.

#include "JasperEngine.h"
#include "Project.h"
#include "navigation/NavMesh.h"
#include "navigation/NavigationEditor.h"
#include "navigation/NavigationOverlay.h"

#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // the world transforms the collector reads, as a frame's update leaves them (parents first)
    void resolveOverlayTransforms(Scene& scene) {
        auto transforms = scene.getComponentArray<Transform>();
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < transforms->size(); i++) {
                Transform& transform = transforms->getComponentFromIndex(i);
                transform.needUpdate = true;
                scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }
    }

    // floors of 20 x 20 m (centres given) under one NavMesh Surface, baked, as a project's scene
    struct OverlayWorld {
        Scene scene;
        std::vector<std::unique_ptr<Primitive>> floors;
        std::unique_ptr<Camera> camera;
        SceneProject sceneProject;
        NavigationSystem* system = nullptr;
        Entity surface = NULL_ENTITY;

        explicit OverlayWorld(std::vector<Vector3> centres = {Vector3(0, 0, 0)}) {
            for (const Vector3& centre : centres) {
                floors.push_back(std::make_unique<Primitive>(&scene));
                floors.back()->createPlane(20.0f, 20.0f);
                floors.back()->setPosition(centre.x, centre.y, centre.z);
            }
            surface = floors.front()->getEntity();
            scene.addComponent<NavMeshSurfaceComponent>(surface, NavMeshSurfaceComponent());
            camera = std::make_unique<Camera>(&scene);
            // an id of its own: the editor keeps per-scene caches (two open scenes never share one)
            static uint32_t nextId = 4343;
            sceneProject.id = ++nextId;
            sceneProject.scene = &scene;
            sceneProject.sceneType = SceneType::SCENE_3D;
            system = NavigationEditor::systemOf(sceneProject);
            resolveOverlayTransforms(scene);
            const NavBakeResult result = system->bakeNow(surface, NavBakeMode::FULL);
            EXPECT_TRUE(result.ok) << result.error;
        }
        ~OverlayWorld() { NavigationEditor::get().clearQueryDraw(sceneProject.id); }
    };

}

TEST(NavigationOverlay, SettingsRoundTrip){
    EXPECT_EQ(encodeNavOverlaySettings(NavOverlaySettings()).size(), 0u);
    NavOverlaySettings settings;
    settings.showTileBounds = true;
    settings.showPaths = NavOverlaySettings::Paths::All;
    settings.showNavMesh = false;
    NavOverlaySettings read;
    decodeNavOverlaySettings(YAML::Load(YAML::Dump(encodeNavOverlaySettings(settings))), read);
    EXPECT_EQ(read, settings);
}

TEST(NavigationOverlay, OldWorkspaceLoads){
    // the "navigation" block an S19 editor wrote: its keys read, the S75 options at their defaults
    NavOverlaySettings read;
    decodeNavOverlaySettings(YAML::Load("enabled: true\nshowTileBounds: true\nshowPaths: all\nshowLinks: false"), read);
    NavOverlaySettings expected;
    expected.showTileBounds = true;
    expected.showPaths = NavOverlaySettings::Paths::All;
    expected.showLinks = false;
    EXPECT_EQ(read, expected);
    EXPECT_EQ(read.colorBy, NavOverlaySettings::ColorBy::Areas);
    EXPECT_FALSE(read.showSteepSlopes);
    EXPECT_FALSE(read.showTileLabels);
    // and it writes back the same keys, nothing new
    const YAML::Node written = encodeNavOverlaySettings(read);
    EXPECT_EQ(written.size(), 3u);
    EXPECT_FALSE(written["colorBy"].IsDefined());
}

TEST(NavigationOverlay, NewFlagsRoundTrip){
    for (NavOverlaySettings::ColorBy colorBy : {NavOverlaySettings::ColorBy::Islands, NavOverlaySettings::ColorBy::Tiles}) {
        NavOverlaySettings settings;
        settings.colorBy = colorBy;
        settings.showSteepSlopes = true;
        settings.showTileLabels = true;
        const YAML::Node node = YAML::Load(YAML::Dump(encodeNavOverlaySettings(settings)));
        EXPECT_EQ(node.size(), 3u);
        EXPECT_EQ(node["colorBy"].as<std::string>(), colorBy == NavOverlaySettings::ColorBy::Islands ? "islands" : "tiles");
        NavOverlaySettings read;
        decodeNavOverlaySettings(node, read);
        EXPECT_EQ(read, settings);
    }
    // an unknown colour mode reads as the default
    NavOverlaySettings read;
    decodeNavOverlaySettings(YAML::Load("colorBy: rainbow"), read);
    EXPECT_EQ(read.colorBy, NavOverlaySettings::ColorBy::Areas);
}

TEST(NavigationOverlay, OffCostsNothing){
    OverlayWorld world;
    // a ramp steeper than the agent's 45 degrees beside the floor (collected, so it would tint)
    Primitive ramp(&world.scene);
    ramp.createPlane(4.0f, 4.0f);
    ramp.setPosition(0.0f, 1.0f, 0.0f);
    ramp.setRotation(0.0f, 0.0f, 60.0f);
    resolveOverlayTransforms(world.scene);

    NavigationOverlay overlay(&world.scene);
    NavOverlaySettings settings;
    overlay.update(world.sceneProject, world.camera.get(), {}, settings);
    const size_t objects = overlay.getObjectCount();
    EXPECT_GT(overlay.getTriangleCount(), 0u);
    // the navmesh's meshes, its edges and the gizmo lines: what S19 made, no slope mesh, no label
    const size_t meshes = (overlay.getTriangleCount() + 21844) / 21845;
    EXPECT_EQ(objects, meshes + 2);
    EXPECT_EQ(overlay.getSlopeTriangleCount(), 0u);
    EXPECT_TRUE(overlay.getTileLabels().empty());

    // on: the slopes get a mesh of their own, the labels appear
    settings.showSteepSlopes = true;
    settings.showTileLabels = true;
    overlay.update(world.sceneProject, world.camera.get(), {}, settings);
    EXPECT_GT(overlay.getSlopeTriangleCount(), 0u);
    EXPECT_GT(overlay.getObjectCount(), objects);
    EXPECT_FALSE(overlay.getTileLabels().empty());

    // off again: nothing drawn from them
    settings.showSteepSlopes = false;
    settings.showTileLabels = false;
    overlay.update(world.sceneProject, world.camera.get(), {}, settings);
    EXPECT_EQ(overlay.getSlopeTriangleCount(), 0u);
    EXPECT_TRUE(overlay.getTileLabels().empty());
}

TEST(NavigationOverlay, TileLabelsText){
    nav::NavTileReport report;
    report.milliseconds = 12.43;
    report.polys = 210;
    report.bytes = 18944;
    EXPECT_EQ(NavigationOverlay::tileLabelText(report), "12.4 ms \xC2\xB7 210 polys \xC2\xB7 18.5 KB");
    report.milliseconds = -1.0;
    EXPECT_EQ(NavigationOverlay::tileLabelText(report), "\xE2\x80\x93 ms \xC2\xB7 210 polys \xC2\xB7 18.5 KB");

    // a bake of this session: every visible tile labelled with its build time
    OverlayWorld world;
    NavigationOverlay overlay(&world.scene);
    NavOverlaySettings settings;
    settings.showTileLabels = true;
    overlay.update(world.sceneProject, world.camera.get(), {}, settings);
    const std::vector<NavTileLabel>& labels = overlay.getTileLabels();
    const std::vector<nav::NavTileReport> reports = world.system->getTileReports(world.surface);
    ASSERT_FALSE(reports.empty());
    ASSERT_EQ(labels.size(), std::min(reports.size(), NavigationOverlay::MAX_TILE_LABELS));
    for (const NavTileLabel& label : labels) {
        EXPECT_EQ(label.surface, world.surface);
        EXPECT_EQ(label.text.find("\xE2\x80\x93"), std::string::npos) << label.text << ": built in this session";
        EXPECT_NE(label.text.find(" polys "), std::string::npos) << label.text;
        EXPECT_FALSE(label.dirty);
    }
}

TEST(NavigationOverlay, IslandsGreyFromStart){
    // the tint rule: one colour per island; with a start, the other islands grey
    nav::NavDebugGeometry geometry;
    geometry.triangleAreas = {0, 0, 0, 0};
    geometry.triangleIslands = {0, 0, 1, -1};
    const std::vector<Vector4> free = NavigationOverlay::triangleColors(geometry, NavOverlaySettings::ColorBy::Islands, -1);
    ASSERT_EQ(free.size(), 4u);
    EXPECT_EQ(free[0], free[1]);
    EXPECT_NE(free[0], free[2]) << "two islands, two colours";
    const Vector4 grey = free[3];
    EXPECT_NE(free[2], grey);
    const std::vector<Vector4> fromStart = NavigationOverlay::triangleColors(geometry, NavOverlaySettings::ColorBy::Islands, 0);
    EXPECT_EQ(fromStart[0], free[0]);
    EXPECT_EQ(fromStart[2], grey) << "the start cannot reach island 1";
    // Color By Areas ignores the islands
    const std::vector<Vector4> areas = NavigationOverlay::triangleColors(geometry, NavOverlaySettings::ColorBy::Areas, 0);
    EXPECT_EQ(areas[0], areas[2]);

    // two floors 40 m apart are two islands; the start on the first greys the second
    OverlayWorld world({Vector3(0, 0, 0), Vector3(40, 0, 0)});
    ASSERT_EQ(world.system->getIslands(world.surface).size(), 2u);
    NavigationOverlay overlay(&world.scene);
    NavOverlaySettings settings;
    settings.colorBy = NavOverlaySettings::ColorBy::Islands;
    NavQueryDraw draw;
    draw.from = Vector3(0, 0, 0);
    draw.hasFrom = true;
    NavigationEditor::get().setQueryDraw(world.sceneProject.id, draw);
    overlay.update(world.sceneProject, world.camera.get(), {}, settings);
    const int startIsland = world.system->locateOn(world.surface, draw.from).island;
    ASSERT_GE(startIsland, 0);
    EXPECT_EQ(overlay.getStartIsland(world.surface), startIsland) << "the overlay keeps the start's island coloured";
    nav::NavDebugGeometry drawn;
    world.system->getDebugGeometry(world.surface, nullptr, NavigationOverlay::MAX_TRIANGLES, false, drawn, nav::DEBUG_ISLANDS);
    const std::vector<Vector4> colors = NavigationOverlay::triangleColors(drawn, NavOverlaySettings::ColorBy::Islands, startIsland);
    size_t greyed = 0;
    for (size_t t = 0; t < colors.size(); t++) greyed += drawn.triangleIslands[t] != startIsland && colors[t] == grey;
    EXPECT_GT(greyed, 0u) << "the second floor is grey";
    // without a start (or by areas) nothing is greyed
    NavigationEditor::get().clearQueryDraw(world.sceneProject.id);
    overlay.update(world.sceneProject, world.camera.get(), {}, settings);
    EXPECT_EQ(overlay.getStartIsland(world.surface), -1);
    EXPECT_EQ(overlay.getTriangleCount(), drawn.triangleAreas.size());
}
