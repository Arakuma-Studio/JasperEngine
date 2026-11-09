// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R10, R11, V5, V6: seams between blocks are bit-exact (AC9), and the audit proves it

#include "VoxelTestSupport.h"
#include "voxel/VoxelClipbox.h"

#include <gtest/gtest.h>

#include <map>
#include <tuple>

using namespace voxeltest;

namespace {

    using PositionKey = std::tuple<int32_t, int32_t, int32_t>;

    std::map<PositionKey, const MeshVertex*> onPlane(const MeshOut& mesh, int axis, int32_t fixedPlane) {
        std::map<PositionKey, const MeshVertex*> out;
        for (const MeshVertex& v : mesh.vertices) {
            if (v.position[axis] == fixedPlane) out[PositionKey(v.position[0], v.position[1], v.position[2])] = &v;
        }
        return out;
    }

    void expectSameVertices(const std::map<PositionKey, const MeshVertex*>& a, const std::map<PositionKey, const MeshVertex*>& b) {
        ASSERT_EQ(a.size(), b.size());
        for (const auto& item : a) {
            const auto found = b.find(item.first);
            ASSERT_NE(found, b.end());
            EXPECT_EQ(std::memcmp(item.second->normal, found->second->normal, sizeof(float) * 3), 0);
        }
    }

    // Every mesh of a clipbox at this focus, audited as one surface
    AuditResult auditFocus(Store& store, Clipbox& clipbox, double x, double y, double z, std::vector<std::shared_ptr<const MeshOut>>* keep = nullptr) {
        clipbox.update(x, y, z);
        std::vector<VisibleBlock> visible;
        clipbox.visibleBlocks(visible);
        std::vector<std::shared_ptr<const MeshOut>> meshes;
        std::vector<const MeshOut*> raw;
        for (const VisibleBlock& block : visible) {
            meshes.push_back(meshOf(store, block.key, block.sideMask));
            raw.push_back(meshes.back().get());
        }
        const AuditResult result = auditWatertight(raw);
        if (keep) *keep = std::move(meshes);
        return result;
    }

}

TEST(VoxelSeam, SharedVerticesBitEqual) {
    Store store(desc(64, 64, 64));
    store.setBase(sphereBase(31.3, 29.7, 33.1, 19.3));
    const int32_t plane = 32 * 256;
    const struct {
        BlockKey a, b;
        int axis;
    } pairs[] = {
        {BlockKey{0, 1, 1, 1}, BlockKey{0, 2, 1, 1}, 0},
        {BlockKey{0, 1, 1, 1}, BlockKey{0, 1, 2, 1}, 1},
        {BlockKey{0, 1, 1, 1}, BlockKey{0, 1, 1, 2}, 2},
    };
    bool any = false;
    for (const auto& pair : pairs) {
        const auto a = meshOf(store, pair.a);
        const auto b = meshOf(store, pair.b);
        const auto va = onPlane(*a, pair.axis, plane);
        const auto vb = onPlane(*b, pair.axis, plane);
        any = any || !va.empty();
        expectSameVertices(va, vb);
    }
    EXPECT_TRUE(any);
}

TEST(VoxelSeam, TransitionMatchesCoarse) {
    // Every side and every combination of masks: the low-resolution face of the fine block carries
    // exactly the coarse block's vertices on that plane, position and normal
    for (int side = 0; side < SIDE_COUNT; side++) {
        const int axis = side / 2;
        const bool high = side % 2 == 1;
        Store store(desc(128, 128, 128, 2));
        store.setBase(sphereBase(64.3, 63.7, 65.1, 41.3));
        // The fine block touching the sphere's surface, with the coarse neighbour across `side`
        for (int attempt = 0; attempt < 16 * 16; attempt++) {
            BlockKey fine{0, 0, 0, 0};
            int coord[3];
            coord[axis] = high ? 3 : 4;
            const int otherA = (axis + 1) % 3, otherB = (axis + 2) % 3;
            coord[otherA] = attempt % 8;
            coord[otherB] = attempt / 8 % 8;
            // The coarse neighbour: its side of the plane is outside this pair of fine blocks
            fine.x = coord[0];
            fine.y = coord[1];
            fine.z = coord[2];
            const int coarseOffset = high ? 1 : -1;
            BlockKey coarse{1, fine.x >> 1, fine.y >> 1, fine.z >> 1};
            if (axis == 0) coarse.x += coarseOffset;
            if (axis == 1) coarse.y += coarseOffset;
            if (axis == 2) coarse.z += coarseOffset;
            const int32_t planeFixed = (high ? (coord[axis] + 1) : coord[axis]) * 16 * 256;
            const auto fineMesh = meshOf(store, fine, static_cast<uint8_t>(1u << side));
            const auto coarseMesh = meshOf(store, coarse);
            // The coarse vertices on the shared face's rectangle
            int checked = 0;
            for (const MeshVertex& c : coarseMesh->vertices) {
                if (c.position[axis] != planeFixed) continue;
                const int32_t aLow = coord[otherA] * 16 * 256, aHigh = (coord[otherA] + 1) * 16 * 256;
                const int32_t bLow = coord[otherB] * 16 * 256, bHigh = (coord[otherB] + 1) * 16 * 256;
                if (c.position[otherA] < aLow || c.position[otherA] > aHigh || c.position[otherB] < bLow || c.position[otherB] > bHigh) continue;
                bool matched = false;
                for (const MeshVertex& f : fineMesh->vertices) {
                    if (f.position[0] == c.position[0] && f.position[1] == c.position[1] && f.position[2] == c.position[2]
                        && std::memcmp(f.normal, c.normal, sizeof(float) * 3) == 0) {
                        matched = true;
                        break;
                    }
                }
                EXPECT_TRUE(matched) << "side " << side << " at " << c.position[0] << " " << c.position[1] << " " << c.position[2];
                checked++;
            }
            if (checked > 0) break;
        }
    }
}

TEST(VoxelSeam, ClipboxSurfaceIsWatertight) {
    for (const int levels : {2, 3, 4}) {
        Store store(desc(256, 256, 256, levels));
        store.setBase(sphereBase(127.3, 121.7, 131.1, 80.3));
        Clipbox clipbox;
        clipbox.configure(store.desc().dimensions, store.levels(), 4);
        for (const auto& focus : std::vector<std::array<double, 3>>{{128, 128, 128}, {60, 200, 128}, {128, 50, 40}, {205, 205, 205}}) {
            const AuditResult audit = auditFocus(store, clipbox, focus[0], focus[1], focus[2]);
            EXPECT_TRUE(audit.watertight) << "levels " << levels << " focus " << focus[0] << " " << focus[1] << " " << focus[2] << ": " << audit.unbalancedEdges << " of " << audit.edgesChecked;
            EXPECT_GT(audit.edgesChecked, 0);
        }
    }
}

TEST(VoxelSeam, WavyFieldIsWatertightAcrossLevels) {
    // Noise-like: masks at corners and edges, crossings that differ between levels
    Store store(desc(128, 128, 128, 3));
    store.setBase(wavyBase(55.0));
    Clipbox clipbox;
    clipbox.configure(store.desc().dimensions, store.levels(), 4);
    std::mt19937 random(11);
    for (int i = 0; i < 6; i++) {
        const AuditResult audit = auditFocus(store, clipbox, random() % 128, 40 + random() % 40, random() % 128);
        EXPECT_TRUE(audit.watertight) << i << ": " << audit.unbalancedEdges << " of " << audit.edgesChecked;
    }
}

TEST(VoxelSeam, AuditCatchesACrack) {
    // Red control of the audit itself: leave out one visible mesh
    Store store(desc(128, 128, 128, 2));
    store.setBase(sphereBase(64.3, 63.7, 65.1, 41.3));
    Clipbox clipbox;
    clipbox.configure(store.desc().dimensions, store.levels(), 4);
    clipbox.update(64, 64, 64);
    std::vector<VisibleBlock> visible;
    clipbox.visibleBlocks(visible);
    std::vector<std::shared_ptr<const MeshOut>> meshes;
    std::vector<const MeshOut*> raw;
    for (const VisibleBlock& block : visible) {
        meshes.push_back(meshOf(store, block.key, block.sideMask));
        if (!meshes.back()->empty()) raw.push_back(meshes.back().get());
    }
    ASSERT_GT(raw.size(), 2u);
    raw.erase(raw.begin() + 1);
    EXPECT_FALSE(auditWatertight(raw).watertight);
}
