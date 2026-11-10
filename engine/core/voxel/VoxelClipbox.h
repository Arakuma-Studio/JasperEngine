// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_CLIPBOX_H
#define VOXEL_CLIPBOX_H

// R16: which blocks draw at which level, around one focus. A pure function of the focus's history, so
// the partition and the at-most-one-level rule are tested without a GPU.

#include "Export.h"
#include "VoxelTypes.h"

#include <array>
#include <vector>

namespace jasperengine::voxel {

    struct VisibleBlock {
        BlockKey key;
        uint8_t sideMask = 0;   // bit per Side: that neighbour is one level coarser (R10)
        bool operator==(const VisibleBlock& other) const { return key == other.key && sideMask == other.sideMask; }
    };

    class JASPERENGINE_API Clipbox {
    public:
        // lodRadius is even and at least 4, in level-k blocks
        void configure(const Int3& dimensions, int levels, int lodRadius);

        // The focus in voxels. True when a box moved; the first call always moves them. The centre of
        // a level moves only after the focus goes 0.75 of a snap cell from it (hysteresis).
        bool update(double x, double y, double z);

        // A coarsest-level block hidden by maxDrawDistance, in voxels from the focus; 0 for none. Its
        // neighbours keep no mask for it.
        void setMaxDrawDistance(double voxels) { maxDraw = voxels; }

        // The visible blocks partition the volume (less the hidden ones), level then z, y, x.
        void visibleBlocks(std::vector<VisibleBlock>& out) const;

        struct Box {
            Int3 low, high;      // level-k blocks, high exclusive; may reach past the volume
            bool infinite = false;
            bool emptyBox() const { return !infinite && (high.x <= low.x || high.y <= low.y || high.z <= low.z); }
            bool contains(int x, int y, int z) const {
                return infinite || (x >= low.x && x < high.x && y >= low.y && y < high.y && z >= low.z && z < high.z);
            }
        };
        const Box& box(int level) const { return boxes[static_cast<size_t>(level)]; }
        int levels() const { return levelCount; }

    private:
        void rebuildBoxes();
        bool hiddenCoarse(const BlockKey& key) const;

        Int3 dims{0, 0, 0};
        int levelCount = 1;
        int radius = 4;
        bool hasFocus = false;
        double maxDraw = 0.0;
        double focus[3] = {0.0, 0.0, 0.0};
        std::array<Int3, MAX_LEVELS> centres{};   // in level-k blocks, on the level-(k+1) block grid
        std::array<Box, MAX_LEVELS> boxes{};
    };

}

#endif
