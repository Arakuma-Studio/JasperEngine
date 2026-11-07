// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Scene view's Path Tester (docs/specs/S75-navigation-diagnostics.md R25, R26, AC13), without
// a window or a GPU: Shift+click sets the start and a click the end, a press near a handle drags
// it and every drag step queries again, each mode answers what NavigationSystem answers, the
// answer is what the overlay's query draw shows, and Locate asks the walkability probe.

#include "JasperEngine.h"
#include "Project.h"
#include "navigation/NavPathTester.h"
#include "navigation/NavigationEditor.h"
#include "navigation/NavWalkabilityProbe.h"

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

using namespace jasperengine;
using namespace jasperengine::editor;

namespace {

    // the world transforms the collector reads, as a frame's update leaves them (parents first)
    void resolveTesterTransforms(Scene& scene) {
        auto transforms = scene.getComponentArray<Transform>();
        for (int pass = 0; pass < 2; pass++) {
            for (size_t i = 0; i < transforms->size(); i++) {
                Transform& transform = transforms->getComponentFromIndex(i);
                transform.needUpdate = true;
                scene.getSystem<RenderSystem>()->updateTransform(transform, transforms->getEntity(i));
            }
        }
    }

    // a 20 x 20 m floor with a baked NavMesh Surface, as one scene of a project
    struct TesterWorld {
        Scene scene;
        std::unique_ptr<Primitive> floor;
        SceneProject sceneProject;
        NavigationSystem* system = nullptr;

        TesterWorld() {
            floor = std::make_unique<Primitive>(&scene);
            floor->createPlane(20.0f, 20.0f);
            scene.addComponent<NavMeshSurfaceComponent>(floor->getEntity(), NavMeshSurfaceComponent());
            // an id of its own: the editor keeps per-scene caches (two open scenes never share one)
            static uint32_t nextId = 4242;
            sceneProject.id = ++nextId;
            sceneProject.scene = &scene;
            sceneProject.sceneType = SceneType::SCENE_3D;
            system = NavigationEditor::systemOf(sceneProject);
            resolveTesterTransforms(scene);
            const NavBakeResult result = system->bakeNow(floor->getEntity(), NavBakeMode::FULL);
            EXPECT_TRUE(result.ok) << result.error;
        }
        ~TesterWorld() { NavigationEditor::get().clearQueryDraw(sceneProject.id); }
    };

    // the view: world x, z -> 10 pixels a metre around (500, 500)
    bool topView(const Vector3& world, Vector2& screen) {
        screen = Vector2(500.0f + world.x * 10.0f, 500.0f + world.z * 10.0f);
        return true;
    }

    Vector2 screenOf(const Vector3& world) {
        Vector2 screen;
        topView(world, screen);
        return screen;
    }

}

TEST(NavPathTester, ShiftClickStartClickEnd){
    TesterWorld world;
    NavPathTester tester;
    tester.start(world.sceneProject);
    EXPECT_TRUE(tester.isActive(world.sceneProject.id));
    EXPECT_FALSE(tester.getResult().ran);

    const Vector3 a(-6.0f, 0.0f, -6.0f), b(6.0f, 0.0f, 5.0f);
    tester.press(world.sceneProject, a, screenOf(a), true, topView);
    EXPECT_TRUE(tester.hasStart());
    EXPECT_FALSE(tester.hasEnd());
    EXPECT_FALSE(tester.getResult().ran) << "a path needs both ends";

    tester.press(world.sceneProject, b, screenOf(b), false, topView);
    ASSERT_TRUE(tester.hasEnd());
    EXPECT_EQ(tester.getStart(), a);
    EXPECT_EQ(tester.getEnd(), b);
    const nav::NavPath& path = tester.getResult().path;
    ASSERT_TRUE(tester.getResult().ran);
    EXPECT_EQ(path.status, nav::PathStatus::Complete);
    EXPECT_EQ(path.failure, nav::PathFailure::None);
    EXPECT_NEAR(path.length, (b - a).length(), 0.5f);

    // Shift+click again moves the start, a click the end; the other end stays
    const Vector3 c(0.0f, 0.0f, -8.0f);
    tester.press(world.sceneProject, c, screenOf(c), true, topView);
    EXPECT_EQ(tester.getStart(), c);
    EXPECT_EQ(tester.getEnd(), b);

    // an end off the mesh: Invalid with its reason
    const Vector3 off(40.0f, 0.0f, 40.0f);
    tester.press(world.sceneProject, off, screenOf(off), false, topView);
    EXPECT_EQ(tester.getResult().path.status, nav::PathStatus::Invalid);
    EXPECT_EQ(tester.getResult().path.failure, nav::PathFailure::EndOffMesh);

    tester.stop();
    EXPECT_FALSE(tester.isActive());
}

TEST(NavPathTester, DragHandleRequeries){
    TesterWorld world;
    NavPathTester tester;
    tester.start(world.sceneProject);
    const Vector3 a(-6.0f, 0.0f, 0.0f), b(6.0f, 0.0f, 0.0f);
    tester.setStart(world.sceneProject, a);
    tester.setEnd(world.sceneProject, b);
    const float before = tester.getResult().path.length;

    // a press 6 pixels from the start's handle grabs it and does not move it
    const Vector3 near(a.x + 0.6f, 0.0f, a.z);
    tester.press(world.sceneProject, near, screenOf(near), false, topView);
    ASSERT_EQ(tester.getDragged(), NavPathTester::Handle::Start);
    EXPECT_EQ(tester.getStart(), a);
    EXPECT_EQ(tester.getEnd(), b);

    // every drag step queries again
    const Vector3 step1(-3.0f, 0.0f, 0.0f), step2(-3.0f, 0.0f, 7.0f);
    tester.drag(world.sceneProject, step1);
    EXPECT_EQ(tester.getStart(), step1);
    EXPECT_NEAR(tester.getResult().path.length, (b - step1).length(), 0.5f);
    EXPECT_LT(tester.getResult().path.length, before);
    tester.drag(world.sceneProject, step2);
    EXPECT_NEAR(tester.getResult().path.length, (b - step2).length(), 0.5f);
    tester.release(world.sceneProject);
    EXPECT_EQ(tester.getDragged(), NavPathTester::Handle::None);

    // the end's handle too; a press 30 pixels from both is a click (it sets the end)
    tester.press(world.sceneProject, b, screenOf(b), true, topView);
    EXPECT_EQ(tester.getDragged(), NavPathTester::Handle::End) << "a press on a handle drags it, Shift or not";
    tester.release(world.sceneProject);
    const Vector3 far(6.0f, 0.0f, 3.0f);
    tester.press(world.sceneProject, far, screenOf(far), false, topView);
    EXPECT_EQ(tester.getDragged(), NavPathTester::Handle::None);
    EXPECT_EQ(tester.getEnd(), far);
}

TEST(NavPathTester, OpsMatchTheSystem){
    TesterWorld world;
    NavPathTester tester;
    tester.start(world.sceneProject);
    const Vector3 a(-7.0f, 0.0f, -2.0f), b(5.0f, 0.0f, 6.0f);
    tester.setStart(world.sceneProject, a);
    tester.setEnd(world.sceneProject, b);
    nav::QueryFilter filter;
    filter.areaMask = (uint16_t)(nav::ALL_AREAS & ~(1u << nav::AREA_JUMP));
    tester.setAreaMask(world.sceneProject, filter.areaMask);
    EXPECT_EQ(tester.filter().areaMask, filter.areaMask);

    const nav::NavPath path = world.system->calculatePath(a, b, filter);
    EXPECT_EQ(tester.getResult().path.status, path.status);
    EXPECT_EQ(tester.getResult().path.corners, path.corners);
    EXPECT_FLOAT_EQ(tester.getResult().path.length, path.length);

    tester.setMode(world.sceneProject, NavPathTester::Mode::Raycast);
    nav::NavHit hit;
    const bool blocked = world.system->raycast(a, b, hit, filter);
    EXPECT_EQ(tester.getResult().blocked, blocked);
    EXPECT_EQ(tester.getResult().hit.position, hit.position);
    // a ray off the floor's edge stops at it (the navmesh ends a radius in, whole voxels: 9-9.5 m)
    const Vector3 out(30.0f, 0.0f, -2.0f);
    tester.setEnd(world.sceneProject, out);
    EXPECT_TRUE(tester.getResult().blocked);
    EXPECT_GT(tester.getResult().hit.position.x, 9.0f);
    EXPECT_LE(tester.getResult().hit.position.x, 9.5f);

    tester.setMode(world.sceneProject, NavPathTester::Mode::ClosestEdge);
    EXPECT_FALSE(tester.usesEnd());
    nav::NavHit edge;
    EXPECT_EQ(tester.getResult().found, world.system->findClosestEdge(a, edge, filter));
    EXPECT_EQ(tester.getResult().hit.position, edge.position);
    // a is 3 m from the floor's left edge, 2-2.5 m from the navmesh's
    EXPECT_GT(tester.getResult().hit.distance, 2.0f);
    EXPECT_LE(tester.getResult().hit.distance, 2.5f);

    tester.setMode(world.sceneProject, NavPathTester::Mode::Locate);
    const nav::NavLocation location = world.system->locate(a, filter);
    EXPECT_EQ(tester.getResult().location.onMesh, location.onMesh);
    EXPECT_EQ(tester.getResult().location.surface, location.surface);
    EXPECT_EQ(tester.getResult().location.position, location.position);

    // the equivalent query_navigation call names the op, the ends, the agent type and the mask
    tester.setMode(world.sceneProject, NavPathTester::Mode::Path);
    const std::string json = tester.queryJson();
    EXPECT_NE(json.find("\"op\": \"path\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"from\": [-7, 0, -2]"), std::string::npos) << json;
    EXPECT_NE(json.find("\"to\": [30, 0, -2]"), std::string::npos) << json;
    EXPECT_NE(json.find("\"area_mask\": " + std::to_string(filter.areaMask)), std::string::npos) << json;
}

TEST(NavPathTester, PublishesQueryDraw){
    TesterWorld world;
    NavPathTester tester;
    const uint32_t id = world.sceneProject.id;
    tester.start(world.sceneProject);
    const Vector3 a(-6.0f, 0.0f, -6.0f), b(6.0f, 0.0f, 6.0f);
    tester.setStart(world.sceneProject, a);
    const NavQueryDraw* draw = NavigationEditor::get().getQueryDraw(id);
    ASSERT_NE(draw, nullptr) << "the start shows before the end is set";
    EXPECT_TRUE(draw->hasFrom);
    EXPECT_FALSE(draw->hasTo);
    tester.setEnd(world.sceneProject, b);
    draw = NavigationEditor::get().getQueryDraw(id);
    ASSERT_NE(draw, nullptr);
    EXPECT_EQ(draw->from, a);
    EXPECT_EQ(draw->to, b);
    EXPECT_EQ(draw->path.status, nav::PathStatus::Complete);
    EXPECT_EQ(draw->path.corners, tester.getResult().path.corners);

    // a blocked ray draws as a Partial path to where it stopped
    tester.setEnd(world.sceneProject, Vector3(30.0f, 0.0f, 6.0f));
    tester.setMode(world.sceneProject, NavPathTester::Mode::Raycast);
    draw = NavigationEditor::get().getQueryDraw(id);
    ASSERT_NE(draw, nullptr);
    EXPECT_EQ(draw->path.status, nav::PathStatus::Partial);
    ASSERT_EQ(draw->path.corners.size(), 2u);
    EXPECT_EQ(draw->path.corners[1], tester.getResult().hit.position);

    tester.stop();
    EXPECT_EQ(NavigationEditor::get().getQueryDraw(id), nullptr) << "ending it takes its drawing away";
}

TEST(NavPathTester, LocateRunsTheProbe){
    TesterWorld world;
    NavPathTester tester;
    tester.start(world.sceneProject);
    tester.setMode(world.sceneProject, NavPathTester::Mode::Locate);

    // on the floor: on the mesh
    const Vector3 on(1.0f, 0.0f, 2.0f);
    tester.press(world.sceneProject, on, screenOf(on), false, topView);
    ASSERT_TRUE(tester.getResult().ran);
    EXPECT_TRUE(tester.getResult().location.onMesh);
    EXPECT_TRUE(tester.getResult().probed);
    EXPECT_EQ(tester.getResult().probeSurface, world.floor->getEntity());

    // 3 m past the floor's edge: off the mesh, found within 5 m, and the probe says why
    const Vector3 off(13.0f, 0.0f, 0.0f);
    tester.press(world.sceneProject, off, screenOf(off), false, topView);
    const NavPathTester::Result& result = tester.getResult();
    EXPECT_FALSE(result.location.onMesh);
    EXPECT_TRUE(result.location.found);
    ASSERT_TRUE(result.probed);
    EXPECT_EQ(result.probeSurface, world.floor->getEntity());
    const NavProbeResult direct = NavWalkabilityProbe::explain(&world.scene, *world.system, world.floor->getEntity(), off, tester.filter());
    ASSERT_FALSE(direct.reasons.empty());
    ASSERT_EQ(result.probe.reasons.size(), direct.reasons.size());
    for (size_t i = 0; i < direct.reasons.size(); i++) EXPECT_EQ(result.probe.reasons[i].code, direct.reasons[i].code);

    // a drag locates every step and asks the probe once it ends
    tester.press(world.sceneProject, off, screenOf(off), false, topView);
    ASSERT_EQ(tester.getDragged(), NavPathTester::Handle::Start);
    tester.drag(world.sceneProject, on);
    EXPECT_TRUE(tester.getResult().location.onMesh);
    EXPECT_EQ(tester.getResult().probe.reasons.size(), direct.reasons.size()) << "the last probe stays while dragging";
    tester.release(world.sceneProject);
    EXPECT_TRUE(tester.getResult().probe.reasons.empty()) << "on the plain floor nothing applies";
}
