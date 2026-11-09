// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R16 (AC14): the visible blocks partition the volume, 26-neighbours differ by at most one level

#include "VoxelTestSupport.h"
#include "voxel/VoxelClipbox.h"

#include <gtest/gtest.h>

#include <map>

using namespace voxeltest;

namespace {

    void checkPartition(const Clipbox& clipbox, const Int3& dim, int levels) {
        std::vector<VisibleBlock> visible;
        clipbox.visibleBlocks(visible);
        // One owner per 16-voxel cell of the volume
        const Int3 cells{dim.x / 16, dim.y / 16, dim.z / 16};
        std::vector<int8_t> owner(static_cast<size_t>(cells.x) * cells.y * cells.z, -1);
        const auto at = [&](int x, int y, int z) { return static_cast<size_t>(x) + static_cast<size_t>(cells.x) * (static_cast<size_t>(y) + static_cast<size_t>(cells.y) * static_cast<size_t>(z)); };
        for (const VisibleBlock& block : visible) {
            const int width = 1 << block.key.level;
            for (int z = block.key.z * width; z < std::min((block.key.z + 1) * width, cells.z); z++) {
                for (int y = block.key.y * width; y < std::min((block.key.y + 1) * width, cells.y); y++) {
                    for (int x = block.key.x * width; x < std::min((block.key.x + 1) * width, cells.x); x++) {
                        ASSERT_EQ(owner[at(x, y, z)], -1) << "overlap at " << x << " " << y << " " << z;
                        owner[at(x, y, z)] = static_cast<int8_t>(block.key.level);
                    }
                }
            }
        }
        for (int z = 0; z < cells.z; z++) {
            for (int y = 0; y < cells.y; y++) {
                for (int x = 0; x < cells.x; x++) {
                    const int mine = owner[at(x, y, z)];
                    ASSERT_NE(mine, -1) << "hole at " << x << " " << y << " " << z;
                    ASSERT_LT(mine, levels);
                    for (int dz = -1; dz <= 1; dz++) {
                        for (int dy = -1; dy <= 1; dy++) {
                            for (int dx = -1; dx <= 1; dx++) {
                                const int nx = x + dx, ny = y + dy, nz = z + dz;
                                if (nx < 0 || ny < 0 || nz < 0 || nx >= cells.x || ny >= cells.y || nz >= cells.z) continue;
                                ASSERT_LE(std::abs(mine - owner[at(nx, ny, nz)]), 1);
                            }
                        }
                    }
                }
            }
        }
        // Masks: exactly the sides whose neighbour is one level coarser
        for (const VisibleBlock& block : visible) {
            const int width = 1 << block.key.level;
            const int offsets[SIDE_COUNT][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
            for (int side = 0; side < SIDE_COUNT; side++) {
                const int x = block.key.x * width + (offsets[side][0] > 0 ? width : offsets[side][0]);
                const int y = block.key.y * width + (offsets[side][1] > 0 ? width : offsets[side][1]);
                const int z = block.key.z * width + (offsets[side][2] > 0 ? width : offsets[side][2]);
                const bool expectMask = x >= 0 && y >= 0 && z >= 0 && x < cells.x && y < cells.y && z < cells.z && owner[at(x, y, z)] > block.key.level;
                ASSERT_EQ((block.sideMask & (1u << side)) != 0, expectMask) << "side " << side;
            }
        }
    }

}

TEST(VoxelClipbox, Partition) {
    std::mt19937 random(3);
    const struct {
        Int3 dim;
        int levels, radius;
    } cases[] = {
        {{256, 256, 256}, 4, 4},
        {{256, 128, 256}, 4, 4},
        {{512, 128, 512}, 4, 6},
        {{1024, 256, 1024}, 4, 8},
        {{64, 64, 64}, 3, 4},
        {{32, 32, 32}, 2, 4},
    };
    for (const auto& c : cases) {
        const int levels = levelsFor(c.dim, c.levels);
        Clipbox clipbox;
        clipbox.configure(c.dim, levels, c.radius);
        for (int i = 0; i < 1700; i++) {
            const double x = random() % (c.dim.x + 400) - 200.0, y = random() % (c.dim.y + 400) - 200.0, z = random() % (c.dim.z + 400) - 200.0;
            clipbox.update(x, y, z);
            checkPartition(clipbox, c.dim, levels);
            if (testing::Test::HasFatalFailure()) return;
        }
    }
}

TEST(VoxelClipbox, Hysteresis) {
    Clipbox clipbox;
    clipbox.configure(Int3{256, 256, 256}, 3, 4);
    EXPECT_TRUE(clipbox.update(128.0, 128.0, 128.0));
    // The snap cell of level 0 is 32 voxels: inside 0.75 of it nothing moves
    for (int i = 0; i < 100; i++) {
        const double wobble = (i % 2 == 0) ? 20.0 : -20.0;
        EXPECT_FALSE(clipbox.update(128.0 + wobble, 128.0 - wobble, 128.0 + wobble));
    }
    EXPECT_TRUE(clipbox.update(128.0 + 25.0, 128.0, 128.0));
}

TEST(VoxelClipbox, DrawDistanceHidesOnlyCoarsest) {
    Clipbox clipbox;
    clipbox.configure(Int3{1024, 128, 1024}, 4, 4);
    clipbox.setMaxDrawDistance(300.0);
    clipbox.update(64.0, 64.0, 64.0);
    std::vector<VisibleBlock> visible;
    clipbox.visibleBlocks(visible);
    bool anyFine = false;
    for (const VisibleBlock& block : visible) {
        if (block.key.level == 3) {
            const double near = std::max(0.0, block.key.x * 128.0 - 64.0);
            EXPECT_LE(near, 300.0);
        } else {
            anyFine = true;
        }
    }
    EXPECT_TRUE(anyFine);
    clipbox.setMaxDrawDistance(0.0);
    std::vector<VisibleBlock> all;
    clipbox.visibleBlocks(all);
    EXPECT_GT(all.size(), visible.size());
}
