// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Physics Placement on a Terrain World (T4 R23): the fine chunks its context reaches are built from
// the tiles, read there and then, sampled as the World's own collider samples them, so crates dropped
// on either side of a tile border, and across it, come to rest on the drawn surface.

#include <gtest/gtest.h>

#include "PhysicsTestScene.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/TerrainWorldComponent.h"
#include "physics/PlacementWorld.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "subsystem/TerrainWorldSystem.h"
#include "terrain/TerrainWorldTestSupport.h"

#include "TempDir.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::physicstest;

#ifdef JASPERENGINE_PHYSICS_3D

namespace {

    constexpr float RAMP_MAX_HEIGHT = 40.0f;

    // a plane rising along x: y = 20 + 0.1 x, at world sample (sx, sz) = (x, z) m
    uint16_t placementRamp(int sx, int) {
        return static_cast<uint16_t>(std::lround((20.0 + 0.1 * sx) / RAMP_MAX_HEIGHT * 65535.0));
    }

    float distanceToRamp(const Vector3& point) {
        return (point.y - 20.0f - 0.1f * point.x) / std::sqrt(1.01f);
    }

}

TEST(TerrainWorldPlacement, CratesRestOnTheSurfaceAcrossBorders) {
    jasperengine::test::TempDir files;
    ASSERT_TRUE(terrainworldtest::writePlayerWorld(files.path() / "Ramp.terrainworld", terrainworldtest::smallLattice(64, 1.0f), RAMP_MAX_HEIGHT, -1, 0,
                                                   placementRamp));
    TestScene t;
    const Entity world = t.entity();
    t.scene.addComponent<MeshComponent>(world, {});
    t.scene.addComponent<TerrainComponent>(world, {});
    TerrainWorldComponent component;
    component.worldData.path = (files.path() / "Ramp.terrainworld").generic_string();
    component.useCameraFocus = false;
    t.scene.addComponent<TerrainWorldComponent>(world, component);
    std::string error;
    TerrainWorldRuntime* runtime = t.scene.getSystem<TerrainWorldSystem>()->ensureRuntime(world, &error);
    ASSERT_NE(runtime, nullptr) << error;

    // in one tile, astride the x = 0 border, astride the z = 0 border, and at the four-tile corner
    const std::vector<Vector3> drops = {Vector3(-30.0f, 0.0f, 20.0f), Vector3(0.0f, 0.0f, 20.0f), Vector3(30.0f, 0.0f, 0.0f), Vector3(0.0f, 0.0f, 0.0f)};
    std::vector<Entity> crates;
    AABB region;
    for (const Vector3& drop : drops) {
        const Vector3 at(drop.x, 22.0f + 0.1f * drop.x, drop.z);
        crates.push_back(t.box(Vector3(1, 1, 1), at));
        region.merge(AABB(at.x - 2.0f, 0.0f, at.z - 2.0f, at.x + 2.0f, 40.0f, at.z + 2.0f));
    }
    t.resolve();

    PlacementWorld placement(&t.scene, PlacementWorldSettings());
    for (Entity crate : crates) ASSERT_TRUE(placement.addMover(crate));
    placement.ensureContext(region);
    // the chunks round each crate, never the whole World (64 chunks at 32 quads; 4 here at most each)
    EXPECT_GT(placement.getContextBodyCount(), 0u);
    EXPECT_LE(placement.getContextBodyCount(), 16u);
    int steps = 0;
    while (!placement.allAsleep() && steps < 1500) {
        placement.step();
        steps++;
    }
    ASSERT_TRUE(placement.allAsleep()) << "the crates came to rest";
    for (size_t i = 0; i < crates.size(); i++) {
        const PlacementBodyState state = placement.getState(crates[i]);
        // R23: on the drawn surface within 1 cm, plus a 16-bit quantum and Jolt's own 1 cm
        EXPECT_NEAR(distanceToRamp(state.pose.position), 0.5f, 0.025f) << "crate " << i << " at " << state.pose.position.x << ", "
                                                                       << state.pose.position.z;
        ASSERT_FALSE(state.restingOn.empty()) << "crate " << i;
        EXPECT_EQ(state.restingOn[0], world) << "crate " << i;
    }
}

#endif
