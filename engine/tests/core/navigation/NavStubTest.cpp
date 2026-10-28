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

TEST_F(NavStubTest, LogsOnce){
    test::LogCapture log;
    nav::NavMesh mesh;
    for (int i = 0; i < 5; i++)
        mesh.findPath(Vector3(), Vector3(1, 0, 0), nav::NavFilter(), Vector3(1, 1, 1));
    EXPECT_LE(log.count(D_LOG_ERROR, "JASPERENGINE_NAVIGATION is off"), 1u);
}

#endif
