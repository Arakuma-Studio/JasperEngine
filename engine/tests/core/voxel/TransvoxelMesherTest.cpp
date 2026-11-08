// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R9-R14: the regular cells, normals, materials and the 16-bit rule (AC8, AC11, AC12)

#include "VoxelTestSupport.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <map>
#include <set>

using namespace voxeltest;

namespace {

    double sphereDistance(double x, double y, double z, double cx, double cy, double cz, double r) {
        return std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy) + (z - cz) * (z - cz)) - r;
    }

    std::vector<std::shared_ptr<const MeshOut>> meshAllLevel0(Store& store, MeshMode mode = MeshMode::Full) {
        std::vector<std::shared_ptr<const MeshOut>> meshes;
        const Int3 count = store.blockCount(0);
        for (int z = 0; z < count.z; z++) {
            for (int y = 0; y < count.y; y++) {
                for (int x = 0; x < count.x; x++) meshes.push_back(meshOf(store, BlockKey{0, x, y, z}, 0, mode));
            }
        }
        return meshes;
    }

}

TEST(TransvoxelMesher, Sphere) {
    Store store(desc(64, 64, 64));
    store.setBase(sphereBase(32.0, 32.0, 32.0, 20.0));
    const auto meshes = meshAllLevel0(store);

    int64_t triangles = 0, facingAir = 0;
    for (const auto& mesh : meshes) {
        for (const MeshVertex& v : mesh->vertices) {
            const double x = v.position[0] / 256.0, y = v.position[1] / 256.0, z = v.position[2] / 256.0;
            EXPECT_LE(std::fabs(sphereDistance(x, y, z, 32, 32, 32, 20)), 0.02);
            const double len = std::sqrt((x - 32) * (x - 32) + (y - 32) * (y - 32) + (z - 32) * (z - 32));
            const double dotted = (v.normal[0] * (x - 32) + v.normal[1] * (y - 32) + v.normal[2] * (z - 32)) / len;
            EXPECT_GE(dotted, std::cos(2.0 * 3.14159265 / 180.0));
        }
        for (size_t i = 0; i + 2 < mesh->indices.size(); i += 3) {
            const MeshVertex& a = mesh->vertices[mesh->indices[i]];
            const MeshVertex& b = mesh->vertices[mesh->indices[i + 1]];
            const MeshVertex& c = mesh->vertices[mesh->indices[i + 2]];
            const double ux = b.position[0] - a.position[0], uy = b.position[1] - a.position[1], uz = b.position[2] - a.position[2];
            const double vx = c.position[0] - a.position[0], vy = c.position[1] - a.position[1], vz = c.position[2] - a.position[2];
            const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            const double mx = (a.position[0] + b.position[0] + c.position[0]) / 768.0 - 32.0;
            const double my = (a.position[1] + b.position[1] + c.position[1]) / 768.0 - 32.0;
            const double mz = (a.position[2] + b.position[2] + c.position[2]) / 768.0 - 32.0;
            // A zero-area triangle faces nowhere; V6 lets them be
            if (nx == 0.0 && ny == 0.0 && nz == 0.0) continue;
            triangles++;
            if (nx * mx + ny * my + nz * mz > 0.0) facingAir++;
        }
    }
    ASSERT_GT(triangles, 1000);
    EXPECT_GE(static_cast<double>(facingAir) / static_cast<double>(triangles), 0.999);

    std::vector<const MeshOut*> raw;
    for (const auto& mesh : meshes) raw.push_back(mesh.get());
    EXPECT_TRUE(auditWatertight(raw).watertight);
}

TEST(TransvoxelMesher, CollisionOnlyGivesTheSameTriangles) {
    Store store(desc(32, 32, 32));
    store.setBase(sphereBase(16.0, 16.0, 16.0, 9.5));
    const auto full = meshAllLevel0(store);
    const auto collision = meshAllLevel0(store, MeshMode::CollisionOnly);
    ASSERT_EQ(full.size(), collision.size());
    for (size_t i = 0; i < full.size(); i++) {
        EXPECT_EQ(sortedTriangles(*full[i]), sortedTriangles(*collision[i]));
    }
}

TEST(TransvoxelMesher, ClosedVolumeMeshesClosed) {
    // Solid to the shell: R8's outer layer of air closes the surface
    Store store(desc(32, 32, 32));
    store.setBase(std::make_shared<FunctionBase>([](double, double, double) { return -8.0; }));
    const auto meshes = meshAllLevel0(store);
    std::vector<const MeshOut*> raw;
    int64_t triangles = 0;
    for (const auto& mesh : meshes) {
        raw.push_back(mesh.get());
        triangles += mesh->triangleCount;
    }
    EXPECT_GT(triangles, 0);
    EXPECT_TRUE(auditWatertight(raw).watertight);
}

TEST(TransvoxelMesher, UniformBlockHasNoMesh) {
    Store store(desc(64, 64, 64));
    EXPECT_TRUE(meshOf(store, BlockKey{0, 1, 1, 1})->empty());
    store.setBase(std::make_shared<FunctionBase>([](double, double, double) { return -8.0; }));
    EXPECT_TRUE(meshOf(store, BlockKey{0, 1, 1, 1})->empty());
}

TEST(TransvoxelMesher, ZeroGradientIsUp) {
    // A vertex whose corners' gradients cancel: two flat plateaus either side of one sample
    Store store(desc(32, 32, 32));
    store.setBase(std::make_shared<FunctionBase>([](double x, double y, double z) {
        return (x == 8 && y == 8 && z == 8) ? -1.0 : 1.0;
    }));
    const auto mesh = meshOf(store, BlockKey{0, 0, 0, 0});
    ASSERT_FALSE(mesh->empty());
    for (const MeshVertex& v : mesh->vertices) {
        EXPECT_NEAR(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2], 1.0f, 1e-4f);
    }
}

TEST(VoxelMesher, MaterialSetsPerCell) {
    Store store(desc(32, 32, 32));
    const auto material = [](int x, int y, int z) -> uint16_t {
        const int a = (x / 3 + z / 5) % 16;
        const int b = (y / 2 + x / 7 + 3) % 16;
        return packMaterial(a, b, (x * 37 + z * 11) % 200);
    };
    store.setBase(std::make_shared<FunctionBase>([](double x, double y, double z) { return std::sqrt((x - 16) * (x - 16) + (y - 16) * (y - 16) + (z - 16) * (z - 16)) - 9.5; }, material));
    const auto first = meshOf(store, BlockKey{0, 0, 0, 0});
    const auto second = meshOf(store, BlockKey{0, 0, 0, 0});
    ASSERT_FALSE(first->empty());
    ASSERT_EQ(first->vertices.size(), second->vertices.size());
    EXPECT_EQ(0, std::memcmp(first->vertices.data(), second->vertices.data(), first->vertices.size() * sizeof(MeshVertex)));
    int multi = 0;
    for (const MeshVertex& v : first->vertices) {
        EXPECT_EQ(v.weights[0] + v.weights[1] + v.weights[2] + v.weights[3], 255);
        if (v.spare > 1) multi++;
        EXPECT_LE(v.spare, 4);
        EXPECT_GE(v.spare, 1);
    }
    EXPECT_GT(multi, 0);
    // Duplicated vertices: same position, different set, never the same set twice
    std::set<std::tuple<int32_t, int32_t, int32_t, uint16_t, uint16_t>> seen;
    for (const MeshVertex& v : first->vertices) {
        EXPECT_TRUE(seen.emplace(v.position[0], v.position[1], v.position[2], v.slots, v.spare).second);
    }
}

TEST(VoxelMesher, IndexLimitFallback) {
    // Adversarial sign and material noise. No real block passes 65,535 at 16^3 with per-cell sets
    // only because cells are few: the limit is lowered so the path R14 promises runs
    Store store(desc(32, 32, 32));
    std::mt19937 random(7);
    std::vector<int16_t> noise(32 * 32 * 32);
    std::vector<uint16_t> noiseMaterial(noise.size());
    for (size_t i = 0; i < noise.size(); i++) {
        noise[i] = static_cast<int16_t>(static_cast<int>(random() % 8000) - 4000);
        noiseMaterial[i] = packMaterial(random() % 16, random() % 16, random() % 255);
    }
    store.setBase(std::make_shared<FunctionBase>(
        [&](double x, double y, double z) { return noise[static_cast<size_t>(x) + 32 * (static_cast<size_t>(y) + 32 * static_cast<size_t>(z))] / 4096.0; },
        [&](int x, int y, int z) { return noiseMaterial[static_cast<size_t>(x) + 32 * (static_cast<size_t>(y) + 32 * static_cast<size_t>(z))]; }));

    const auto view = store.readView(BlockKey{0, 0, 0, 0}, 0);
    MeshRequest request;
    request.key = BlockKey{0, 0, 0, 0};
    MeshOut normal;
    ASSERT_TRUE(meshBlock(*view, request, normal));
    EXPECT_FALSE(normal.usedBlockMaterialSet);
    EXPECT_LE(normal.vertices.size(), 65535u);

    MeshOptions options;
    options.maxVerticesPerCellSets = 2000;
    MeshOut fallback;
    ASSERT_TRUE(meshBlock(*view, request, fallback, options));
    EXPECT_TRUE(fallback.usedBlockMaterialSet);
    EXPECT_LE(fallback.vertices.size(), 15000u);
    std::set<uint16_t> codes;
    for (const MeshVertex& v : fallback.vertices) codes.insert(v.slots);
    EXPECT_EQ(codes.size(), 1u);
    EXPECT_EQ(sizeof(MeshVertex), 32u);
    EXPECT_EQ(sortedTriangles(normal), sortedTriangles(fallback));
}
