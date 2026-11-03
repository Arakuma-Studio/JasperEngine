// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The editor draws its overlays (the navmesh itself, gizmos) as meshes in the scene they annotate:
// the bake never collects them, so a fresh bake is not stale once its overlay shows (MCP
// test_bake_job_and_info read every tile dirty right after an editor bake).

#include "JasperEngine.h"

#include <gtest/gtest.h>

using namespace jasperengine;

TEST(NavigationCollect, EditorOverlaysNotCollected){
    Scene scene;
    Primitive floor(&scene);
    floor.createPlane(20.0f, 20.0f);
    NavMeshSurfaceComponent surface;
    scene.addComponent<NavMeshSurfaceComponent>(floor.getEntity(), surface);
    auto system = scene.getSystem<NavigationSystem>();
    system->setKeepBakeData(true);
    const NavBakeResult result = system->bakeNow(floor.getEntity(), NavBakeMode::FULL);
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(system->countDirtyTiles(floor.getEntity()), 0);
    // a big box over the floor: collected, the bake is stale
    Primitive overlay(&scene);
    overlay.createBox(30.0f, 1.0f, 30.0f);
    overlay.setPosition(0.0f, 0.5f, 0.0f);
    EXPECT_GT(system->countDirtyTiles(floor.getEntity()), 0);
    // the same box as an editor overlay: not the level
    scene.getSystem<RenderSystem>()->setEditorOverlay(overlay.getEntity(), true);
    EXPECT_EQ(system->countDirtyTiles(floor.getEntity()), 0);
}
