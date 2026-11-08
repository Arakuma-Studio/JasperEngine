// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC26: what foliage puts in a bake. A bake's input does not depend on where the camera is: grass
// never enters, and a tree with navObstacle does, under every Use Geometry.

#include "../terrain/FoliageTestSupport.h"
#include "component/Transform.h"

#include "component/NavMeshModifierComponent.h"
#include "component/NavMeshSurfaceComponent.h"
#include "navigation/NavBakeCollector.h"
#include "navigation/NavBakeInput.h"

using namespace foliagetest;

namespace {

    std::shared_ptr<nav::NavBakeInput> collectAt(FoliageFixture& fixture, Entity surface, nav::NavCollectStats* stats = nullptr, bool mayBeEmpty = false) {
        std::string error;
        std::shared_ptr<nav::NavBakeInput> input = nav::NavBakeCollector::collect(fixture.scene.get(), surface, error, stats);
        EXPECT_TRUE(mayBeEmpty || input != nullptr) << error;
        return input;
    }

    Entity surfaceOn(FoliageFixture& fixture, NavUseGeometry geometry = NavUseGeometry::RENDER_MESHES) {
        const Entity surface = fixture.scene->createEntity();
        fixture.scene->addComponent<Transform>(surface);
        NavMeshSurfaceComponent component;
        component.useGeometry = geometry;
        fixture.scene->addComponent<NavMeshSurfaceComponent>(surface, component);
        return surface;
    }

    void obstacles(FoliageFixture& fixture, std::initializer_list<std::pair<float, float>> points) {
        withTrees(fixture, points);
        TerrainTreePrototype& prototype = fixture.terrain().treePrototypes.back();
        prototype.navObstacle = true;
        prototype.collisionRadius = 1.0f;
        prototype.collisionHeight = 6.0f;
        fixture.settle();
    }

}

TEST(NavBakeFoliage, TreesAreObstaclesAtEveryCamera) {
    FoliageFixture fixture;
    obstacles(fixture, {{10.0f, 10.0f}, {-20.0f, 5.0f}});
    const Entity surface = surfaceOn(fixture);
    nav::NavCollectStats nearStats;
    collectAt(fixture, surface, &nearStats);
    EXPECT_EQ(nearStats.instances, 2);

    fixture.setCamera(60.0f, 60.0f);
    fixture.settle();
    nav::NavCollectStats farStats;
    collectAt(fixture, surface, &farStats);
    EXPECT_EQ(farStats.instances, 2) << "a tree is in the bake wherever the camera is";

    // under every Use Geometry
    const Entity physicsSurface = surfaceOn(fixture, NavUseGeometry::PHYSICS_COLLIDERS);
    nav::NavCollectStats physics;
    collectAt(fixture, physicsSurface, &physics);
    EXPECT_EQ(physics.instances, 2);
}

TEST(NavBakeFoliage, GrassIsNeverCollected) {
    FoliageFixture fixture;
    fixture.settle();
    ASSERT_GT(fixture.foliage->getInstanceCount(fixture.entity, 0), 0u);
    const Entity surface = surfaceOn(fixture);
    nav::NavCollectStats stats;
    collectAt(fixture, surface, &stats);
    EXPECT_EQ(stats.instances, 0);
    EXPECT_EQ(stats.meshes, 0);
}

TEST(NavBakeFoliage, BakeDoesNotDependOnTheCamera) {
    // the red run for R34: with chunks, what the ring held near the camera went into the bake
    FoliageFixture fixture(16.0f);
    fixture.settle();
    obstacles(fixture, {{10.0f, 10.0f}});
    const Entity surface = surfaceOn(fixture);
    const std::shared_ptr<nav::NavBakeInput> first = collectAt(fixture, surface);
    fixture.setCamera(-60.0f, -60.0f);
    fixture.settle();
    const std::shared_ptr<nav::NavBakeInput> second = collectAt(fixture, surface);
    EXPECT_EQ(first->triangles.size(), second->triangles.size());
    EXPECT_EQ(first->vertices, second->vertices);
}

TEST(NavBakeFoliage, ModifierExcludesTrees) {
    FoliageFixture fixture;
    obstacles(fixture, {{10.0f, 10.0f}});
    NavMeshModifierComponent modifier;
    modifier.overrideInclusion = NavModifierInclusion::EXCLUDE;
    fixture.scene->addComponent<NavMeshModifierComponent>(fixture.entity, modifier);
    const Entity surface = surfaceOn(fixture);
    nav::NavCollectStats stats;
    // the modifier leaves the Terrain itself out, too: there may be nothing to bake
    collectAt(fixture, surface, &stats, true);
    EXPECT_EQ(stats.instances, 0) << "the Terrain's policy decides for its trees";
    EXPECT_GT(stats.excluded, 0);
}
