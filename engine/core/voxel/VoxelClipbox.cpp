// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VoxelClipbox.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine::voxel;

void Clipbox::configure(const Int3& dimensions, int levels, int lodRadius) {
    dims = dimensions;
    levelCount = std::clamp(levels, 1, MAX_LEVELS);
    radius = lodRadius;
    hasFocus = false;
}

bool Clipbox::update(double x, double y, double z) {
    focus[0] = x;
    focus[1] = y;
    focus[2] = z;
    bool moved = !hasFocus;
    const double position[3] = {x, y, z};
    for (int level = 0; level + 1 < levelCount; level++) {
        // The snap cell: one level-(k+1) block, in voxels
        const double snap = static_cast<double>(BLOCK_SIZE << (level + 1));
        Int3& centre = centres[static_cast<size_t>(level)];
        int* c[3] = {&centre.x, &centre.y, &centre.z};
        // Centres are in level-(k+1) blocks times 2, so in level-k blocks
        bool outside = !hasFocus;
        for (int axis = 0; axis < 3 && !outside; axis++) {
            const double centreVoxels = static_cast<double>(*c[axis]) * snap * 0.5;
            if (std::fabs(position[axis] - centreVoxels) > snap * 0.75) outside = true;
        }
        if (!outside) continue;
        for (int axis = 0; axis < 3; axis++) {
            *c[axis] = static_cast<int>(std::floor(position[axis] / snap + 0.5)) * 2;
        }
        moved = true;
    }
    hasFocus = true;
    if (moved) rebuildBoxes();
    return moved;
}

void Clipbox::rebuildBoxes() {
    const int top = levelCount - 1;
    boxes[static_cast<size_t>(top)] = Box{};
    boxes[static_cast<size_t>(top)].infinite = true;
    for (int level = top - 1; level >= 0; level--) {
        const Int3& centre = centres[static_cast<size_t>(level)];
        Box box;
        box.low = Int3{centre.x - radius, centre.y - radius, centre.z - radius};
        box.high = Int3{centre.x + radius, centre.y + radius, centre.z + radius};
        const Box& next = boxes[static_cast<size_t>(level + 1)];
        if (!next.infinite) {
            // Inside the next box shrunk by one of its blocks: every neighbour of a level-k block is
            // then level k or k+1, also across edges and corners
            box.low.x = std::max(box.low.x, (next.low.x + 1) * 2);
            box.low.y = std::max(box.low.y, (next.low.y + 1) * 2);
            box.low.z = std::max(box.low.z, (next.low.z + 1) * 2);
            box.high.x = std::min(box.high.x, (next.high.x - 1) * 2);
            box.high.y = std::min(box.high.y, (next.high.y - 1) * 2);
            box.high.z = std::min(box.high.z, (next.high.z - 1) * 2);
        }
        boxes[static_cast<size_t>(level)] = box;
    }
}

bool Clipbox::hiddenCoarse(const BlockKey& key) const {
    if (maxDraw <= 0.0 || key.level != levelCount - 1) return false;
    const double span = static_cast<double>(BLOCK_SIZE << key.level);
    const double low[3] = {key.x * span, key.y * span, key.z * span};
    double squared = 0.0;
    for (int axis = 0; axis < 3; axis++) {
        const double nearest = std::clamp(focus[axis], low[axis], low[axis] + span);
        squared += (nearest - focus[axis]) * (nearest - focus[axis]);
    }
    return squared > maxDraw * maxDraw;
}

void Clipbox::visibleBlocks(std::vector<VisibleBlock>& out) const {
    out.clear();
    for (int level = 0; level < levelCount; level++) {
        const int span = BLOCK_SIZE << level;
        const Int3 count{(dims.x + span - 1) / span, (dims.y + span - 1) / span, (dims.z + span - 1) / span};
        const Box& box = boxes[static_cast<size_t>(level)];
        if (box.emptyBox()) continue;
        const Int3 low = box.infinite ? Int3{0, 0, 0} : Int3{std::max(box.low.x, 0), std::max(box.low.y, 0), std::max(box.low.z, 0)};
        const Int3 high = box.infinite ? count : Int3{std::min(box.high.x, count.x), std::min(box.high.y, count.y), std::min(box.high.z, count.z)};
        const Box* finer = level > 0 ? &boxes[static_cast<size_t>(level - 1)] : nullptr;
        for (int z = low.z; z < high.z; z++) {
            for (int y = low.y; y < high.y; y++) {
                for (int x = low.x; x < high.x; x++) {
                    if (finer && !finer->emptyBox() && finer->contains(x * 2, y * 2, z * 2)) continue;
                    const BlockKey key{level, x, y, z};
                    if (hiddenCoarse(key)) continue;
                    VisibleBlock block;
                    block.key = key;
                    if (level + 1 < levelCount) {
                        const int offsets[SIDE_COUNT][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
                        for (int side = 0; side < SIDE_COUNT; side++) {
                            const int nx = x + offsets[side][0], ny = y + offsets[side][1], nz = z + offsets[side][2];
                            if (nx < 0 || ny < 0 || nz < 0 || nx >= count.x || ny >= count.y || nz >= count.z) continue;
                            if (box.contains(nx, ny, nz)) continue;
                            // The coarser neighbour: no mask for one hidden by the draw distance
                            const BlockKey coarser{level + 1, nx >> 1, ny >> 1, nz >> 1};
                            if (hiddenCoarse(coarser)) continue;
                            block.sideMask = static_cast<uint8_t>(block.sideMask | (1u << side));
                        }
                    }
                    out.push_back(block);
                }
            }
        }
    }
}
