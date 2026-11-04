// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// A build without Recast/Detour (JASPERENGINE_NAVIGATION=OFF, docs/specs/S19-navigation.md R1,
// AC1): every navmesh bake, query and crowd fails and logs once; grids keep working.

#include "navigation/NavTypes.h"

#if !JASPERENGINE_NAVIGATION

#include "NavTestUtil.h"

#include "LogCapture.h"
#include "navigation/NavCrowd.h"
#include "navigation/NavMesh.h"
#include "object/navigation/NavMeshAgent.h"

using namespace jasperengine;
using namespace jasperengine::navtest;

namespace {
    class NavStubTest : public NavSceneTest {};
}

TEST_F(NavStubTest, QueriesFail){
    EXPECT_FALSE(nav::isNavMeshAvailable());
    const Entity surface = planeSurface();
    resolveTransforms(*scene);
    std::string error;
    EXPECT_FALSE(nav->bake(surface, NavBakeMode::FULL, error));
    EXPECT_NE(error.find("JASPERENGINE_NAVIGATION"), std::string::npos) << error;
    EXPECT_EQ(nav->getNavMesh(surface), nullptr);
    EXPECT_EQ(nav->calculatePath(Vector3(-5, 0, 0), Vector3(5, 0, 0)).status, nav::PathStatus::Invalid);
    nav::NavHit hit;
    EXPECT_FALSE(nav->samplePosition(Vector3(), 2.0f, hit));
    EXPECT_FALSE(nav->findClosestEdge(Vector3(), hit));
    // S55's queries fail the same way
    EXPECT_FALSE(nav->findClosestEdge(Vector3(), hit, {}, 5.0f));
    EXPECT_FALSE(nav->randomPoint(hit));
    EXPECT_FALSE(nav->moveAlongSurface(Vector3(), Vector3(1, 0, 0), hit));
    nav::PathOptions crossings;
    crossings.areaCrossings = true;
    EXPECT_EQ(nav->calculatePath(Vector3(-5, 0, 0), Vector3(5, 0, 0), {}, crossings).status, nav::PathStatus::Invalid);

    nav::NavMesh mesh;
    nav::NavMeshData data;
    EXPECT_FALSE(mesh.create(data, false, error));
    nav::NavCrowd crowd;
    EXPECT_FALSE(crowd.init(mesh, 8, 0.5f));

    // an agent does not move, and nothing crashes
    const Entity walker = agent(*scene, Vector3());
    steps(5);
    EXPECT_FALSE(NavMeshAgent(scene.get(), walker).isOnNavMesh());
}

// S54 R18 (AC8): the diagnostics answer Not baked, no islands and an empty location
TEST_F(NavStubTest, DiagnosticsStubs){
    test::LogCapture log;
    const Entity surface = planeSurface();
    resolveTransforms(*scene);
    const nav::NavPath path = nav->calculatePath(Vector3(-5, 0, 0), Vector3(5, 0, 0));
    EXPECT_EQ(path.status, nav::PathStatus::Invalid);
    EXPECT_EQ(path.failure, nav::PathFailure::NotBaked);
    nav::PathFailure why = nav::PathFailure::None;
    EXPECT_FALSE(nav->isReachable(Vector3(-5, 0, 0), Vector3(5, 0, 0), {}, &why));
    EXPECT_EQ(why, nav::PathFailure::NotBaked);
    EXPECT_TRUE(nav->getIslands(surface).empty());
    EXPECT_EQ(nav->getCoverage(surface).islandCount, 0);
    const nav::NavLocation location = nav->locate(Vector3());
    EXPECT_FALSE(location.found);
    EXPECT_FALSE(location.onMesh);

    nav::NavMesh mesh;
    EXPECT_EQ(mesh.findPath(Vector3(), Vector3(1, 0, 0), nav::NavFilter(), Vector3(1, 1, 1)).failure, nav::PathFailure::NotBaked);
    EXPECT_TRUE(mesh.getIslands().empty());
    EXPECT_FALSE(mesh.locate(Vector3(), nav::NavFilter(), Vector3(1, 1, 1), 5.0f).found);
    EXPECT_FALSE(mesh.setPolyFlags(1, 0));
    for (int i = 0; i < 5; i++)
        mesh.locate(Vector3(), nav::NavFilter(), Vector3(1, 1, 1), 5.0f);
    EXPECT_LE(log.count(D_LOG_ERROR, "JASPERENGINE_NAVIGATION is off"), 1u);
    // the bake says why
    const NavBakeResult result = nav->bakeNow(surface, NavBakeMode::FULL);
    EXPECT_EQ(result.errorCode, nav::NavBakeError::Unavailable);
}

TEST_F(NavStubTest, LogsOnce){
    test::LogCapture log;
    nav::NavMesh mesh;
    nav::NavRandom random;
    nav::NavHit hit;
    for (int i = 0; i < 5; i++) {
        mesh.findPath(Vector3(), Vector3(1, 0, 0), nav::NavFilter(), Vector3(1, 1, 1));
        mesh.findRandomPoint(nav::NavFilter(), random, -1, hit);
        mesh.findRandomPointAround(Vector3(), 2.0f, nav::NavFilter(), Vector3(1, 1, 1), random, -1, hit);
        mesh.moveAlongSurface(Vector3(), Vector3(1, 0, 0), nav::NavFilter(), Vector3(1, 1, 1), hit);
    }
    EXPECT_LE(log.count(D_LOG_ERROR, "JASPERENGINE_NAVIGATION is off"), 1u);
}

#endif
