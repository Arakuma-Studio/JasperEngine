// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T3 R23, R24: a terrain's holes in the bake (AC31). Apart from NavBakeTest.cpp, whose fixtures have a
// function named terrain, which the namespace of terrain/TerrainHoles.h would make ambiguous.

#include "NavFixtures.h"
#include "component/TerrainComponent.h"
#include "NavTestUtil.h"
#include "navigation/NavBakeCollector.h"

#include "object/TerrainEdit.h"
#include "terrain/TerrainHoles.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    constexpr int HOLE_TERRAIN_SAMPLES = 33;
    constexpr float HOLE_TERRAIN_SIZE = 32.0f;

    class NavBake : public navtest::NavSceneTest {
    protected:
        // whether a world point has walkable navmesh within `reach`
        bool walkable(const Vector3& point, float reach = 0.3f) {
            nav::NavHit hit;
            return nav->samplePosition(point, reach, hit) && hit.position.distance(point) <= reach;
        }
    };

    // A flat 32 m terrain (a cell per metre) whose hole map is a wall of holes across x = -2..2,
    // z = -16..10, which leaves a gap at the far end for a path to go round
    struct HoleTerrain {
        std::vector<uint8_t> heights;
        std::vector<uint8_t> holes;

        Entity add(Scene& scene, const std::string& name) {
            heights.assign((size_t)HOLE_TERRAIN_SAMPLES * HOLE_TERRAIN_SAMPLES, 64);
            holes.assign((size_t)HOLE_TERRAIN_SAMPLES * HOLE_TERRAIN_SAMPLES, terrain::SOLID_VALUE);
            for (int y = 0; y < HOLE_TERRAIN_SAMPLES; y++) {
                for (int x = 0; x < HOLE_TERRAIN_SAMPLES; x++) {
                    const float localX = (x + 0.5f) / HOLE_TERRAIN_SAMPLES * HOLE_TERRAIN_SIZE - HOLE_TERRAIN_SIZE * 0.5f;
                    const float localZ = (y + 0.5f) / HOLE_TERRAIN_SAMPLES * HOLE_TERRAIN_SIZE - HOLE_TERRAIN_SIZE * 0.5f;
                    if (std::fabs(localX) < 2.0f && localZ < 10.0f)
                        holes[(size_t)y * HOLE_TERRAIN_SAMPLES + x] = terrain::HOLE_VALUE;
                }
            }
            const Entity entity = navtest::transformEntity(scene, Vector3());
            TerrainComponent component;
            component.terrainSize = HOLE_TERRAIN_SIZE;
            component.maxHeight = 4.0f;
            component.heightMap.setData(name + "-height", TextureData(HOLE_TERRAIN_SAMPLES, HOLE_TERRAIN_SAMPLES, (unsigned int)heights.size(),
                                                                      ColorFormat::RED, 1, heights.data()));
            component.holeMap.setData(name + "-holes", TextureData(HOLE_TERRAIN_SAMPLES, HOLE_TERRAIN_SAMPLES, (unsigned int)holes.size(),
                                                                   ColorFormat::RED, 1, holes.data()));
            scene.addComponent<TerrainComponent>(entity, component);
            return entity;
        }
    };

}

TEST_F(NavBake, TerrainHoleExcluded) {
    const Entity surface = navtest::transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    navtest::addSurface(*scene, surface, 0.25f, 16);
    HoleTerrain fixture;
    const Entity entity = fixture.add(*scene, "nav-hole-excluded");

    navtest::resolveTransforms(*scene);
    std::string error;
    std::shared_ptr<nav::NavBakeInput> input = nav::NavBakeCollector::collect(scene.get(), surface, error, nullptr);
    ASSERT_TRUE(input) << error;
    ASSERT_EQ(input->terrains.size(), 1u);
    const nav::NavTerrainInput& lattice = input->terrains[0];
    ASSERT_FALSE(lattice.holeCells.empty());
    EXPECT_TRUE(lattice.isHoleCell(lattice.samples / 2, 2));
    EXPECT_FALSE(lattice.isHoleCell(2, 2));

    bake(surface);
    const float ground = 4.0f * 64.0f / 255.0f;
    EXPECT_TRUE(walkable(Vector3(-8.0f, ground, 0.0f)));
    EXPECT_TRUE(walkable(Vector3(8.0f, ground, 0.0f)));
    EXPECT_FALSE(walkable(Vector3(0.0f, ground, 0.0f))) << "the middle of the hole has no navmesh";

    // no path crosses: the route goes round the far end, and every corner stays on drawn ground
    const nav::NavPath path = nav->calculatePath(Vector3(-8.0f, ground, 0.0f), Vector3(8.0f, ground, 0.0f));
    ASSERT_GE(path.corners.size(), 3u);
    TerrainComponent& component = scene->getComponent<TerrainComponent>(entity);
    float farthest = -16.0f;
    for (size_t c = 0; c < path.corners.size(); c++) {
        EXPECT_FALSE(querySurface(component, path.corners[c].x, path.corners[c].z).hole) << "corner " << c;
        farthest = std::max(farthest, path.corners[c].z);
    }
    EXPECT_GE(farthest, 10.0f) << "it must go round the end of the hole";

    // every navmesh point is on drawn ground: a grid of samples that find the navmesh
    int found = 0;
    for (float z = -15.5f; z < 16.0f; z += 0.5f) {
        for (float x = -15.5f; x < 16.0f; x += 0.5f) {
            nav::NavHit hit;
            if (nav->samplePosition(Vector3(x, ground, z), 0.3f, hit) && hit.position.distance(Vector3(x, ground, z)) <= 0.3f) {
                found++;
                EXPECT_FALSE(querySurface(component, hit.position.x, hit.position.z).hole) << hit.position.x << ", " << hit.position.z;
            }
        }
    }
    EXPECT_GT(found, 1000);
}

TEST_F(NavBake, HoleChangesTileHash) {
    const Entity surface = navtest::transformEntity(*scene, Vector3(), NULL_ENTITY, "Surface");
    navtest::addSurface(*scene, surface, 0.25f, 16);
    HoleTerrain fixture;
    const Entity entity = fixture.add(*scene, "nav-hole-hash");
    navtest::resolveTransforms(*scene);
    bake(surface);
    EXPECT_EQ(nav->countDirtyTiles(surface), 0);

    // a new hole in one corner: only the tiles under it rebake
    TerrainComponent& component = scene->getComponent<TerrainComponent>(entity);
    TextureData& data = component.holeMap.getData();
    uint8_t* texels = static_cast<uint8_t*>(data.getData());
    texels[0] = terrain::HOLE_VALUE;
    const int dirty = nav->countDirtyTiles(surface);
    EXPECT_GT(dirty, 0);
    EXPECT_LT(dirty, 4) << "only touched tiles rebake";

    // a change of a texel that changes no cell's state leaves every tile clean
    texels[0] = terrain::SOLID_VALUE;
    EXPECT_EQ(nav->countDirtyTiles(surface), 0);
}
