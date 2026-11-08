// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Selection over a whole Terrain World (T4 R7-R10, R36, D4; AC2-AC5)

#include "TerrainWorldTestSupport.h"
#include "terrain/world/TerrainWorldSelect.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <random>

using namespace terrainworldtest;

namespace {

    struct SelectWorld {
        WorldLattice lattice = smallLattice(256, 1.0f);
        WorldOccupancy occupancy;
        std::map<uint32_t, std::shared_ptr<TileData>> pages;
        std::shared_ptr<TileData> flat;

        SelectWorld() {
            flat = std::make_shared<TileData>();
            initTile(*flat, lattice, TileCoord(0, 0), 20000);
        }

        void fill(int low, int high) {
            for (int z = low; z <= high; z++) {
                for (int x = low; x <= high; x++) occupancy.setTile(TileCoord(x, z), true, 20000, 20000);
            }
        }

        void residentAround(int low, int high) {
            for (int z = low; z <= high; z++) {
                for (int x = low; x <= high; x++) pages[TileCoord(x, z).key()] = flat;
            }
        }

        WorldSelectParams params(float x, float y, float z) const {
            WorldSelectParams p;
            p.lattice = lattice;
            p.maxHeight = 400.0f;
            p.farClip = 20000.0f;
            p.eyeX = x;
            p.eyeY = y;
            p.eyeZ = z;
            p.page = [this](TileCoord tile) -> const TileData* {
                const auto it = pages.find(tile.key());
                return it == pages.end() ? nullptr : it->second.get();
            };
            return p;
        }
    };

    // Every point of the occupied area falls in exactly one selected node (nodes overlap only on
    // their edges), sampled on a grid of cell centres
    void expectCoversOnce(const SelectWorld& world, const WorldSelection& selection, int low, int high, float cell) {
        std::vector<WorldSelectNode> all = selection.overviewFull;
        all.insert(all.end(), selection.overviewHalf.begin(), selection.overviewHalf.end());
        for (const WorldTileDraws& draws : selection.near) {
            all.insert(all.end(), draws.full.begin(), draws.full.end());
            all.insert(all.end(), draws.half.begin(), draws.half.end());
        }
        const float size = world.lattice.tileSize();
        const float from = static_cast<float>(low) * size, to = static_cast<float>(high + 1) * size;
        int checked = 0;
        for (float z = from + cell * 0.5f; z < to; z += cell) {
            for (float x = from + cell * 0.5f; x < to; x += cell) {
                if (!world.occupancy.present(TileCoord(tileOfLocal(x, size), tileOfLocal(z, size)))) continue;
                int hits = 0;
                for (const WorldSelectNode& node : all) {
                    const float half = node.size * 0.5f;
                    if (x > node.x - half && x < node.x + half && z > node.z - half && z < node.z + half) hits++;
                }
                ASSERT_EQ(hits, 1) << "point " << x << ", " << z;
                checked++;
            }
        }
        EXPECT_GT(checked, 0);
        // and nothing is drawn where there is no tile
        for (const WorldSelectNode& node : all) {
            const int tx = tileOfLocal(node.x, size), tz = tileOfLocal(node.z, size);
            EXPECT_TRUE(world.occupancy.present(TileCoord(tx, tz))) << node.x << ", " << node.z;
        }
    }

}

TEST(TerrainWorldSelect, DoesNotScaleWithWorld) {
    // the same eye over filled 8 x 8, 64 x 64 and 256 x 256 worlds, looking along the ground with no
    // frustum test. A level more expands the eye's node and the neighbours in its range, so the
    // bound is the small world's count plus PER_DOUBLING per doubling of the side (spec Deviations:
    // R7's "4" counted one node a level)
    std::map<int, int> visited;
    for (int side : {8, 64, 256}) {
        SelectWorld world;
        world.lattice = smallLattice(64, 1.0f);
        world.fill(-side / 2, side / 2 - 1);
        WorldSelectParams params = world.params(10.0f, 30.0f, 10.0f);
        params.farClip = 1000.0f;
        WorldSelection selection;
        selectWorld(params, world.occupancy, selection);
        visited[side] = selection.visited;
        EXPECT_FALSE(selection.overflow);
    }
    constexpr int PER_DOUBLING = 40;
    std::printf("[bench] terrain_world_select_visited 8:%d 64:%d 256:%d\n", visited[8], visited[64], visited[256]);
    EXPECT_LE(visited[64], visited[8] + PER_DOUBLING * 3);
    EXPECT_LE(visited[256], visited[8] + PER_DOUBLING * 5);
    // a walk over every tile would visit at least 65,536 nodes on the big one
    EXPECT_LT(visited[256], 2000);
}

TEST(TerrainWorldSelect, NodeNeverCrossesTile) {
    SelectWorld world;
    world.fill(-3, 3);
    world.residentAround(-1, 1);
    WorldSelection selection;
    selectWorld(world.params(30.0f, 60.0f, -40.0f), world.occupancy, selection);
    ASSERT_FALSE(selection.near.empty());
    const float size = world.lattice.tileSize();
    for (const WorldTileDraws& draws : selection.near) {
        for (const auto* grid : {&draws.full, &draws.half}) {
            for (const WorldSelectNode& node : *grid) {
                EXPECT_LE(node.level, world.lattice.tileLevel());
                const float half = node.size * 0.5f;
                EXPECT_EQ(tileOfLocal(node.x - half, size), draws.tile.x);
                EXPECT_EQ(tileOfLocal(node.x + half - 0.001f, size), draws.tile.x);
                EXPECT_EQ(tileOfLocal(node.z - half, size), draws.tile.z);
                EXPECT_EQ(tileOfLocal(node.z + half - 0.001f, size), draws.tile.z);
            }
        }
    }
    // overview nodes are at level T or above, whole tiles
    for (const auto* grid : {&selection.overviewFull, &selection.overviewHalf}) {
        for (const WorldSelectNode& node : *grid) {
            EXPECT_GE(node.level, world.lattice.tileLevel());
            const float origin = node.x - node.size * 0.5f;
            EXPECT_EQ(std::fmod(origin, size), 0.0f);
        }
    }
}

TEST(TerrainWorldSelect, CoversEachPointOnce) {
    for (int state = 0; state < 4; state++) {
        SCOPED_TRACE(state);
        SelectWorld world;
        world.fill(-4, 3);
        // checkerboard, all, none and one of four resident
        for (int z = -4; z <= 3; z++) {
            for (int x = -4; x <= 3; x++) {
                const bool resident = state == 0 ? ((x + z) & 1) == 0 : (state == 1 ? true : (state == 2 ? false : ((x & 1) == 0 && (z & 1) == 0)));
                if (resident) world.pages[TileCoord(x, z).key()] = world.flat;
            }
        }
        // a sparse corner, so partly occupied nodes are on the way
        world.occupancy.setTile(TileCoord(3, 3), false);
        world.occupancy.setTile(TileCoord(-4, 2), false);
        WorldSelection selection;
        selectWorld(world.params(50.0f, 40.0f, 70.0f), world.occupancy, selection);
        expectCoversOnce(world, selection, -4, 3, 7.0f);
    }
}

TEST(TerrainWorldSelect, DrawBound) {
    for (int maxNear : {16, 6}) {
        SelectWorld world;
        world.lattice = smallLattice(64, 1.0f);
        world.fill(-20, 19);
        world.residentAround(-20, 19);
        std::mt19937 random(maxNear);
        std::uniform_real_distribution<float> place(-1200.0f, 1200.0f), height(1.0f, 300.0f);
        for (int trial = 0; trial < 1000; trial++) {
            WorldSelectParams params = world.params(place(random), height(random), place(random));
            params.maxNearTiles = maxNear;
            WorldSelection selection;
            selectWorld(params, world.occupancy, selection);
            ASSERT_LE(selection.drawItems(), 2 * maxNear + 2) << trial;
            ASSERT_LE(static_cast<int>(selection.near.size()), maxNear);
        }
    }
}

TEST(TerrainWorldSelect, NearestTilesFirst) {
    SelectWorld world;
    world.lattice = smallLattice(64, 1.0f);
    world.fill(-10, 9);
    world.residentAround(-10, 9);
    WorldSelectParams params = world.params(5.0f, 10.0f, 5.0f);
    params.maxNearTiles = 4;
    WorldSelection selection;
    selectWorld(params, world.occupancy, selection);
    ASSERT_EQ(selection.near.size(), 4u);
    EXPECT_EQ(selection.near[0].tile, TileCoord(0, 0)) << "the eye's tile first";
    EXPECT_GT(selection.fellBack, 0) << "past D, tiles draw from the overview";
}

TEST(TerrainWorldSelect, FallsBackWhenNotResident) {
    SelectWorld world;
    world.fill(-2, 1);
    // nothing resident: every tile in the near band is a level T overview half grid, no morph
    WorldSelection selection;
    selectWorld(world.params(10.0f, 50.0f, 10.0f), world.occupancy, selection);
    EXPECT_TRUE(selection.near.empty());
    EXPECT_GT(selection.fellBack, 0);
    EXPECT_EQ(selection.missingPages, selection.fellBack);
    int levelT = 0;
    for (const WorldSelectNode& node : selection.overviewHalf) {
        if (node.level == world.lattice.tileLevel() && node.range == WORLD_INFINITE_RANGE) levelT++;
    }
    EXPECT_EQ(levelT, selection.fellBack);
    // an error tile is just a tile whose page never comes: the same
    world.residentAround(-2, 1);
    world.pages.erase(TileCoord(0, 0).key());
    selectWorld(world.params(10.0f, 50.0f, 10.0f), world.occupancy, selection);
    for (const WorldTileDraws& draws : selection.near) EXPECT_NE(draws.tile, TileCoord(0, 0));
    EXPECT_EQ(selection.missingPages, 1);
    bool found = false;
    for (const WorldSelectNode& node : selection.overviewHalf) {
        found = found || (node.x == 128.0f && node.z == 128.0f && node.level == world.lattice.tileLevel() && node.range == WORLD_INFINITE_RANGE);
    }
    EXPECT_TRUE(found) << "the missing page's tile is drawn by the overview";
}

TEST(TerrainWorldSelect, FrustumCulls) {
    SelectWorld world;
    world.lattice = smallLattice(64, 1.0f);
    world.fill(-30, 29);
    world.residentAround(-30, 29);
    WorldSelectParams params = world.params(0.0f, 20.0f, 0.0f);
    WorldSelection all;
    selectWorld(params, world.occupancy, all);
    // only what lies at x >= 0
    params.planes[0][0] = 1.0f;
    WorldSelection half;
    selectWorld(params, world.occupancy, half);
    EXPECT_LT(half.selectedNodes(), all.selectedNodes());
    for (const WorldTileDraws& draws : half.near) EXPECT_GE(draws.tile.x, -1);
}

TEST(TerrainWorldSelect, OccupancyCountsAndBounds) {
    WorldOccupancy occupancy;
    occupancy.setTile(TileCoord(-256, -256), true, 10, 20);
    occupancy.setTile(TileCoord(255, 255), true, 5, 40);
    occupancy.setTile(TileCoord(3, -7), true, 7, 9);
    EXPECT_EQ(occupancy.tileCount(), 3);
    EXPECT_EQ(occupancy.count(9, -256, -256), 3u);
    EXPECT_EQ(occupancy.minMax(9, -256, -256).min, 5);
    EXPECT_EQ(occupancy.minMax(9, -256, -256).max, 40);
    TileCoord low, high;
    ASSERT_TRUE(occupancy.bounds(low, high));
    EXPECT_EQ(low, TileCoord(-256, -256));
    EXPECT_EQ(high, TileCoord(255, 255));
    occupancy.setTile(TileCoord(255, 255), false);
    EXPECT_EQ(occupancy.count(9, -256, -256), 2u);
    EXPECT_EQ(occupancy.minMax(9, -256, -256).max, 20);
    EXPECT_EQ(occupancy.presentTiles().size(), 2u);
}
