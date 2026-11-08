// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R36 (AC33): a mesh as a signed distance field

#include "VoxelTestSupport.h"
#include "voxel/VoxelVoxelize.h"

#include <gtest/gtest.h>

using namespace voxeltest;

namespace {

    // A UV sphere, closed
    void sphereMesh(double radius, int rings, int segments, std::vector<float>& positions, std::vector<uint32_t>& indices) {
        for (int r = 0; r <= rings; r++) {
            const double phi = 3.14159265358979 * r / rings;
            for (int s = 0; s < segments; s++) {
                const double theta = 2.0 * 3.14159265358979 * s / segments;
                positions.push_back(static_cast<float>(radius * std::sin(phi) * std::cos(theta)));
                positions.push_back(static_cast<float>(radius * std::cos(phi)));
                positions.push_back(static_cast<float>(radius * std::sin(phi) * std::sin(theta)));
            }
        }
        for (int r = 0; r < rings; r++) {
            for (int s = 0; s < segments; s++) {
                const uint32_t a = static_cast<uint32_t>(r * segments + s);
                const uint32_t b = static_cast<uint32_t>(r * segments + (s + 1) % segments);
                const uint32_t c = a + static_cast<uint32_t>(segments);
                const uint32_t d = b + static_cast<uint32_t>(segments);
                indices.insert(indices.end(), {a, b, c, b, d, c});
            }
        }
    }

}

TEST(VoxelVoxelize, Sphere) {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    sphereMesh(6.0, 32, 48, positions, indices);
    VoxelizeOptions options;
    options.voxelSize = 1.0;
    options.bandVoxels = 4.0;
    const std::shared_ptr<StampField> field = voxelizeMesh(positions, indices, options);
    ASSERT_GT(field->size.x, 12);
    EXPECT_FALSE(field->nonWatertight);
    int checked = 0;
    for (int z = 0; z < field->size.z; z++) {
        for (int y = 0; y < field->size.y; y++) {
            for (int x = 0; x < field->size.x; x++) {
                const double px = x - field->originOffset[0], py = y - field->originOffset[1], pz = z - field->originOffset[2];
                const double analytic = std::sqrt(px * px + py * py + pz * pz) - 6.0;
                if (std::fabs(analytic) > 3.0) continue;
                const double stored = decodeSdf(field->sdf[static_cast<size_t>(x) + static_cast<size_t>(field->size.x) * (static_cast<size_t>(y) + static_cast<size_t>(field->size.y) * static_cast<size_t>(z))]);
                ASSERT_NEAR(stored, analytic, 0.05) << x << " " << y << " " << z;
                checked++;
            }
        }
    }
    EXPECT_GT(checked, 500);
}

TEST(VoxelVoxelize, NonWatertightFlagged) {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    sphereMesh(6.0, 16, 24, positions, indices);
    // A third of the triangles gone: rays disagree over far more than 1% of the samples
    indices.resize(indices.size() * 2 / 3 / 3 * 3);
    VoxelizeOptions options;
    options.bandVoxels = 2.0;
    EXPECT_TRUE(voxelizeMesh(positions, indices, options)->nonWatertight);
}

TEST(VoxelVoxelize, UsedAsAStampMatchesThePrimitive) {
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    sphereMesh(5.0, 32, 48, positions, indices);
    VoxelizeOptions options;
    const std::shared_ptr<StampField> field = voxelizeMesh(positions, indices, options);
    Store fromMesh(desc(64, 64, 64));
    Store primitive(desc(64, 64, 64));
    EditOp a;
    a.kind = EditKind::Add;
    a.shape.kind = ShapeKind::Mesh;
    a.shape.field = field;
    a.shape.center = Vec3d{32.0, 32.0, 32.0};
    const EditOp b = addOp(sphereShape(32.0, 32.0, 32.0, 5.0));
    ASSERT_TRUE(applyEdit(fromMesh, std::span<const EditOp>(&a, 1)).ok);
    ASSERT_TRUE(applyEdit(primitive, std::span<const EditOp>(&b, 1)).ok);
    for (int z = 26; z <= 38; z++) {
        for (int y = 26; y <= 38; y++) {
            for (int x = 26; x <= 38; x++) {
                ASSERT_NEAR(decodeSdf(fromMesh.sdfAt(0, x, y, z)), decodeSdf(primitive.sdfAt(0, x, y, z)), 0.06);
            }
        }
    }
}
