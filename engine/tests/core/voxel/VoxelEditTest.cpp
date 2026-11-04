// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T6 R8, R26-R32 (AC7, AC25, AC26, AC27, AC29, AC31): operations, validation, determinism, queries

#include "VoxelTestSupport.h"

#include <gtest/gtest.h>

using namespace voxeltest;

namespace {

    constexpr double ONE_256TH = 1.0 / 256.0;

    double sampleVoxels(Store& store, int x, int y, int z) {
        return decodeSdf(store.sdfAt(0, x, y, z));
    }

    template<class Fn>
    void expectFieldMatches(Store& store, const IntBox& box, Fn expected) {
        for (int z = box.min.z; z <= box.max.z; z++) {
            for (int y = box.min.y; y <= box.max.y; y++) {
                for (int x = box.min.x; x <= box.max.x; x++) {
                    const double wanted = std::clamp(expected(x, y, z), -SDF_BAND_VOXELS, SDF_BAND_VOXELS);
                    ASSERT_NEAR(sampleVoxels(store, x, y, z), wanted, ONE_256TH) << x << " " << y << " " << z;
                }
            }
        }
    }

    bool sameResult(const EditResult& a, const EditResult& b) {
        return a.ok == b.ok && a.error == b.error && a.changedVoxels.min == b.changedVoxels.min && a.changedVoxels.max == b.changedVoxels.max
            && a.voxelsChanged == b.voxelsChanged && a.surfaceVoxelsChanged == b.surfaceVoxelsChanged && a.blocksTouched == b.blocksTouched
            && a.blocksAllocated == b.blocksAllocated && a.blocksCollapsed == b.blocksCollapsed && a.memoryDelta == b.memoryDelta
            && a.clampedVoxels == b.clampedVoxels && a.removedVolume == b.removedVolume && a.addedVolume == b.addedVolume
            && a.removedByMaterial == b.removedByMaterial && a.contentHashAfter == b.contentHashAfter;
    }

    Shape boxShape(double x, double y, double z, double hx, double hy, double hz) {
        Shape shape;
        shape.kind = ShapeKind::Box;
        shape.center = Vec3d{x, y, z};
        shape.halfExtents = Vec3d{hx, hy, hz};
        return shape;
    }

    Shape capsuleShape(const Vec3d& a, const Vec3d& b, double radius) {
        Shape shape;
        shape.kind = ShapeKind::Capsule;
        shape.from = a;
        shape.to = b;
        shape.radius = radius;
        return shape;
    }

    std::shared_ptr<StampField> sphereField(double radius, int size) {
        auto field = std::make_shared<StampField>();
        field->size = Int3{size, size, size};
        field->voxelSize = 1.0;
        field->sdf.resize(static_cast<size_t>(size) * size * size);
        const double middle = (size - 1) * 0.5;
        size_t i = 0;
        for (int z = 0; z < size; z++) {
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    const double dx = x - middle, dy = y - middle, dz = z - middle;
                    field->sdf[i++] = encodeSdf(std::sqrt(dx * dx + dy * dy + dz * dz) - radius);
                }
            }
        }
        field->originOffset[0] = field->originOffset[1] = field->originOffset[2] = middle;
        return field;
    }

}

TEST(VoxelEdit, OpsMatchAnalytic) {
    // Sphere: add into air
    {
        Store store(desc(64, 64, 64));
        const EditOp op = addOp(sphereShape(32.0, 30.0, 34.0, 7.5));
        ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1)).ok);
        expectFieldMatches(store, IntBox{Int3{20, 20, 20}, Int3{44, 44, 44}}, [](int x, int y, int z) {
            return std::sqrt((x - 32.0) * (x - 32.0) + (y - 30.0) * (y - 30.0) + (z - 34.0) * (z - 34.0)) - 7.5;
        });
    }
    // Sphere: dig out of a solid
    {
        Store store(desc(64, 64, 64));
        store.setBase(std::make_shared<FunctionBase>([](double, double, double) { return -9.0; }));
        const EditOp op = digOp(sphereShape(32.0, 32.0, 32.0, 6.0));
        ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1)).ok);
        expectFieldMatches(store, IntBox{Int3{22, 22, 22}, Int3{42, 42, 42}}, [](int x, int y, int z) {
            return 6.0 - std::sqrt((x - 32.0) * (x - 32.0) + (y - 32.0) * (y - 32.0) + (z - 32.0) * (z - 32.0));
        });
    }
    // Box with rotation: 90 degrees about y swaps the x and z extents
    {
        Store store(desc(64, 64, 64));
        EditOp op = addOp(boxShape(32.0, 32.0, 32.0, 3.0, 5.0, 8.0));
        const double half = std::sqrt(0.5);
        op.shape.rotation = Quatd{half, 0.0, half, 0.0};
        ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1)).ok);
        expectFieldMatches(store, IntBox{Int3{20, 22, 20}, Int3{44, 42, 44}}, [](int x, int y, int z) {
            const double qx = std::fabs(z - 32.0) - 3.0, qy = std::fabs(y - 32.0) - 5.0, qz = std::fabs(x - 32.0) - 8.0;
            const double ox = std::max(qx, 0.0), oy = std::max(qy, 0.0), oz = std::max(qz, 0.0);
            return std::sqrt(ox * ox + oy * oy + oz * oz) + std::min(std::max(qx, std::max(qy, qz)), 0.0);
        });
    }
    // Capsule
    {
        Store store(desc(64, 64, 64));
        const EditOp op = addOp(capsuleShape(Vec3d{20.0, 32.0, 32.0}, Vec3d{44.0, 32.0, 32.0}, 4.0));
        ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1)).ok);
        expectFieldMatches(store, IntBox{Int3{10, 22, 22}, Int3{54, 42, 42}}, [](int x, int y, int z) {
            const double h = std::clamp((x - 20.0) / 24.0, 0.0, 1.0);
            const double dx = x - (20.0 + 24.0 * h), dy = y - 32.0, dz = z - 32.0;
            return std::sqrt(dx * dx + dy * dy + dz * dz) - 4.0;
        });
    }
    // Stamp: a grid holding a sphere equals the sphere primitive, away from where the grid ends
    {
        Store stamp(desc(64, 64, 64));
        Store primitive(desc(64, 64, 64));
        EditOp a = addOp(sphereShape(32.0, 32.0, 32.0, 6.0));
        EditOp b;
        b.kind = EditKind::Add;
        b.shape.kind = ShapeKind::Stamp;
        b.shape.center = Vec3d{32.0, 32.0, 32.0};
        b.shape.field = sphereField(6.0, 31);
        ASSERT_TRUE(applyEdit(primitive, std::span<const EditOp>(&a, 1)).ok);
        ASSERT_TRUE(applyEdit(stamp, std::span<const EditOp>(&b, 1)).ok);
        for (int z = 22; z <= 42; z++) {
            for (int y = 22; y <= 42; y++) {
                for (int x = 22; x <= 42; x++) {
                    ASSERT_NEAR(sampleVoxels(stamp, x, y, z), sampleVoxels(primitive, x, y, z), ONE_256TH);
                }
            }
        }
        // Uniform scale: a 2x stamp of radius 6 is a sphere of radius 12
        Store scaled(desc(64, 64, 64));
        EditOp c = b;
        c.shape.scale = 2.0;
        ASSERT_TRUE(applyEdit(scaled, std::span<const EditOp>(&c, 1)).ok);
        EXPECT_NEAR(sampleVoxels(scaled, 32, 32, 32 + 12), 0.0, 0.05);
        EXPECT_LT(sampleVoxels(scaled, 32, 32, 32 + 11), 0.0);
    }
    // Smooth: lowers a spike's curvature, keeps a plane, within the shape only
    {
        Store store(desc(64, 64, 64));
        store.setBase(std::make_shared<FunctionBase>([](double, double y, double) { return y - 32.0; }));
        EditOp op;
        op.kind = EditKind::Smooth;
        op.shape = sphereShape(32.0, 32.0, 32.0, 6.0);
        op.amount = 1.0;
        op.iterations = 2;
        ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1)).ok);
        // A linear field is a fixed point of the blur
        EXPECT_NEAR(sampleVoxels(store, 32, 33, 32), 1.0, ONE_256TH);
        const EditOp bump = addOp(sphereShape(32.0, 32.0, 32.0, 3.0));
        ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&bump, 1)).ok);
        const double peak = sampleVoxels(store, 32, 34, 32);
        const double outside = sampleVoxels(store, 50, 50, 50);
        ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1)).ok);
        EXPECT_GT(sampleVoxels(store, 32, 34, 32), peak);
        EXPECT_EQ(sampleVoxels(store, 50, 50, 50), outside);
    }
    // Paint: sets slot and weight, SDF untouched, absolute
    {
        Store store(desc(64, 64, 64));
        store.setBase(wavyBase(32.0));
        const uint64_t before = [&] {
            uint64_t hash = FNV_OFFSET;
            for (int i = 0; i < 64; i++) {
                const int16_t value = store.sdfAt(0, i, i, 32);
                hash = fnv1a(&value, 2, hash);
            }
            return hash;
        }();
        EditOp op;
        op.kind = EditKind::Paint;
        op.shape = sphereShape(32.0, 32.0, 32.0, 10.0);
        op.material = 5;
        op.weight = 0.5;
        ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1)).ok);
        const uint64_t after = [&] {
            uint64_t hash = FNV_OFFSET;
            for (int i = 0; i < 64; i++) {
                const int16_t value = store.sdfAt(0, i, i, 32);
                hash = fnv1a(&value, 2, hash);
            }
            return hash;
        }();
        EXPECT_EQ(before, after);
        uint16_t painted = store.materialAt(0, 32, 32, 32);
        EXPECT_EQ(materialSlotB(painted), 5);
        EXPECT_EQ(materialWeightB(painted), 128);
        EXPECT_EQ(store.materialAt(0, 60, 32, 32), 0);
        // A second identical call changes nothing
        const EditResult again = applyEdit(store, std::span<const EditOp>(&op, 1));
        EXPECT_EQ(again.voxelsChanged, 0);
        op.weight = 1.0;
        ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1)).ok);
        painted = store.materialAt(0, 32, 32, 32);
        EXPECT_EQ(materialSlotA(painted), 5);
        EXPECT_EQ(materialWeightB(painted), 0);
    }
    // Blend: a smooth union sits lower than the hard one, only near the join
    {
        Store hard(desc(64, 64, 64));
        Store soft(desc(64, 64, 64));
        hard.setBase(std::make_shared<FunctionBase>([](double, double y, double) { return y - 20.0; }));
        soft.setBase(std::make_shared<FunctionBase>([](double, double y, double) { return y - 20.0; }));
        const EditOp a = addOp(sphereShape(32.0, 22.0, 32.0, 6.0));
        EditOp b = a;
        b.blend = 3.0;
        ASSERT_TRUE(applyEdit(hard, std::span<const EditOp>(&a, 1)).ok);
        ASSERT_TRUE(applyEdit(soft, std::span<const EditOp>(&b, 1)).ok);
        EXPECT_LT(sampleVoxels(soft, 38, 21, 32), sampleVoxels(hard, 38, 21, 32));
        EXPECT_EQ(sampleVoxels(soft, 32, 10, 32), sampleVoxels(hard, 32, 10, 32));
    }
}

TEST(VoxelEdit, ClosedShellStaysAir) {
    Store store(desc(32, 32, 32));
    const EditOp op = addOp(boxShape(16.0, 16.0, 16.0, 100.0, 100.0, 100.0));
    const EditResult result = applyEdit(store, std::span<const EditOp>(&op, 1));
    ASSERT_TRUE(result.ok);
    for (int i = 0; i < 32; i++) {
        EXPECT_GT(store.sdfAt(0, 0, i, 16), 0);
        EXPECT_GT(store.sdfAt(0, 31, i, 16), 0);
        EXPECT_GT(store.sdfAt(0, i, 0, 16), 0);
        EXPECT_GT(store.sdfAt(0, i, 31, 16), 0);
    }
    EXPECT_LT(store.sdfAt(0, 1, 16, 16), 0);
    // 32^3 samples, 30^3 inner: the shell's samples are the clamped ones
    EXPECT_EQ(result.clampedVoxels, 32 * 32 * 32 - 30 * 30 * 30);
    EXPECT_EQ(result.voxelsChanged, 30 * 30 * 30);
}

TEST(VoxelEdit, OpenMarginStaysBase) {
    Store store(desc(32, 32, 32, 1, BoundsMode::Open));
    store.setBase(std::make_shared<FunctionBase>([](double, double y, double) { return y - 16.0; }));
    const EditOp op = digOp(boxShape(16.0, 10.0, 16.0, 100.0, 4.0, 100.0));
    const EditResult result = applyEdit(store, std::span<const EditOp>(&op, 1));
    ASSERT_TRUE(result.ok);
    EXPECT_GT(result.clampedVoxels, 0);
    for (int i = 0; i < 32; i++) {
        EXPECT_EQ(store.sdfAt(0, 0, 10, i), encodeSdf(-6.0));
        EXPECT_EQ(store.sdfAt(0, 1, 10, i), encodeSdf(-6.0));
        EXPECT_EQ(store.sdfAt(0, 31, 10, i), encodeSdf(-6.0));
    }
    EXPECT_GT(store.sdfAt(0, 2, 10, 16), 0);
}

TEST(VoxelEdit, DryRunEqualsApply) {
    std::mt19937 random(9);
    Store store(desc(64, 64, 64, 3));
    store.setBase(wavyBase(30.0));
    for (int i = 0; i < 200; i++) {
        Shape shape;
        switch (random() % 3) {
            case 0: shape = sphereShape(random() % 64, random() % 64, random() % 64, 1.0 + random() % 6); break;
            case 1: shape = boxShape(random() % 64, random() % 64, random() % 64, 1.0 + random() % 5, 1.0 + random() % 5, 1.0 + random() % 5); break;
            default: shape = capsuleShape(Vec3d{static_cast<double>(random() % 64), static_cast<double>(random() % 64), static_cast<double>(random() % 64)}, Vec3d{static_cast<double>(random() % 64), static_cast<double>(random() % 64), static_cast<double>(random() % 64)}, 1.0 + random() % 3); break;
        }
        EditOp op;
        op.kind = static_cast<EditKind>(random() % 4);
        op.shape = shape;
        op.material = op.kind == EditKind::Paint ? static_cast<int>(random() % 16) : (random() % 2 ? static_cast<int>(random() % 16) : -1);
        op.blend = random() % 2 ? 1.0 : 0.0;
        op.iterations = 1 + random() % 2;
        EditOptions dry;
        dry.dryRun = true;
        dry.hash = true;
        const uint64_t before = contentHash(store);
        const int64_t memoryBefore = store.memory().hotBytes;
        const EditResult preview = applyEdit(store, std::span<const EditOp>(&op, 1), dry);
        ASSERT_EQ(contentHash(store), before);
        ASSERT_EQ(store.memory().hotBytes, memoryBefore);
        EditOptions real;
        real.hash = true;
        const EditResult applied = applyEdit(store, std::span<const EditOp>(&op, 1), real);
        ASSERT_TRUE(sameResult(preview, applied)) << "op " << i;
        if (applied.ok) {
            ASSERT_EQ(applied.contentHashAfter, contentHash(store));
        }
    }
}

TEST(VoxelEdit, ValidationAllOrNothing) {
    Store store(desc(64, 64, 64));
    store.setBase(wavyBase(30.0));
    const uint64_t before = contentHash(store);
    const EditOp good = digOp(sphereShape(32.0, 30.0, 32.0, 5.0));

    const auto refused = [&](const EditOp& bad, EditError expected, const EditOptions& options = {}) {
        const EditOp ops[] = {good, bad};
        const EditResult result = applyEdit(store, ops, options);
        EXPECT_FALSE(result.ok);
        EXPECT_EQ(result.error, expected);
        EXPECT_EQ(contentHash(store), before);
    };

    refused(digOp(sphereShape(500.0, 500.0, 500.0, 5.0)), EditError::OutOfBounds);
    refused(digOp(sphereShape(32.0, 32.0, 32.0, -1.0)), EditError::InvalidShape);
    EditOp iterations;
    iterations.kind = EditKind::Smooth;
    iterations.shape = sphereShape(32.0, 32.0, 32.0, 3.0);
    iterations.iterations = 9;
    refused(iterations, EditError::InvalidShape);
    refused(addOp(sphereShape(32.0, 32.0, 32.0, 3.0), 16), EditError::InvalidMaterial);
    EditOp slots = addOp(sphereShape(32.0, 32.0, 32.0, 3.0), 3);
    EditOptions threeSlots;
    threeSlots.slotCount = 3;
    refused(slots, EditError::InvalidMaterial, threeSlots);
    EditOp paintNothing;
    paintNothing.kind = EditKind::Paint;
    paintNothing.shape = sphereShape(32.0, 32.0, 32.0, 3.0);
    refused(paintNothing, EditError::InvalidMaterial);
    EditOp missing;
    missing.shape.kind = ShapeKind::Stamp;
    refused(missing, EditError::MissingAsset);
    EditOptions budget;
    budget.maxVoxels = 100;
    refused(digOp(sphereShape(32.0, 30.0, 32.0, 20.0)), EditError::BudgetExceeded, budget);
}

TEST(VoxelDeterminism, GoldenHash) {
    // A fixed script: the bytes must be the same on every target (R28). The constant was recorded
    // once on x86-64 with no contraction.
    Store store(desc(64, 64, 64, 3));
    store.setBase(std::make_shared<FunctionBase>([](double, double y, double) { return (y - 30.0) * 0.75; }));
    EditOp ops[6];
    ops[0] = digOp(sphereShape(20.5, 30.25, 20.75, 7.125));
    ops[1] = addOp(sphereShape(44.5, 33.0, 40.25, 6.5), 3);
    ops[2].kind = EditKind::Smooth;
    ops[2].shape = sphereShape(32.0, 30.0, 32.0, 9.0);
    ops[2].amount = 0.6;
    ops[2].iterations = 3;
    ops[3].kind = EditKind::Paint;
    ops[3].shape = sphereShape(30.0, 30.0, 30.0, 12.0);
    ops[3].material = 7;
    ops[3].weight = 0.7;
    ops[3].falloff = 0.5;
    ops[4] = digOp(capsuleShape(Vec3d{10.0, 28.0, 50.0}, Vec3d{50.0, 31.0, 12.0}, 2.75));
    ops[4].blend = 1.5;
    ops[5] = addOp(boxShape(50.0, 34.0, 50.0, 3.0, 4.0, 5.0), 9);
    const double half = 0.8660254037844386;
    ops[5].shape.rotation = Quatd{half, 0.0, 0.5, 0.0};
    const EditResult result = applyEdit(store, ops);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(contentHash(store), 0x659ad177d96702d6ull) << std::hex << contentHash(store);
}

TEST(VoxelSample, GetValueAtMatchesAnalytic) {
    Store store(desc(64, 64, 64, 1, BoundsMode::Closed, 0.5f));
    store.setBase(std::make_shared<FunctionBase>([](double, double y, double) { return y - 30.25; }));
    // Metres, negative inside; trilinear between samples
    EXPECT_NEAR(valueAt(store, Vec3d{16.0, 16.0, 16.0}), (32.0 - 30.25) * 0.5, 1e-3);
    EXPECT_NEAR(valueAt(store, Vec3d{16.25, 15.25, 16.25}), (30.5 - 30.25) * 0.5, 1e-3);
    EXPECT_NEAR(valueAt(store, Vec3d{-5.0, 16.0, 16.0}), SDF_BAND_VOXELS * 0.5, 1e-9);
}

TEST(VoxelSample, RaycastMatchesMesh) {
    Store store(desc(64, 64, 64));
    store.setBase(sphereBase(32.0, 32.0, 32.0, 20.0));
    std::mt19937 random(13);
    std::vector<std::shared_ptr<const MeshOut>> meshes;
    for (int z = 0; z < 4; z++) {
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) meshes.push_back(meshOf(store, BlockKey{0, x, y, z}));
        }
    }
    int hits = 0;
    for (int i = 0; i < 200; i++) {
        const Vec3d origin{2.0 + random() % 60, 60.0, 2.0 + random() % 60};
        const Vec3d direction{(static_cast<int>(random() % 200) - 100) * 0.002, -1.0, (static_cast<int>(random() % 200) - 100) * 0.002};
        const RaycastHit hit = raycast(store, origin, direction, 200.0);
        // Against the triangles: Moller-Trumbore, nearest
        double best = 1e9;
        const double length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
        const Vec3d d{direction.x / length, direction.y / length, direction.z / length};
        for (const auto& mesh : meshes) {
            for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3) {
                const auto point = [&](size_t k) {
                    const MeshVertex& v = mesh->vertices[mesh->indices[t + k]];
                    return Vec3d{v.position[0] / 256.0, v.position[1] / 256.0, v.position[2] / 256.0};
                };
                const Vec3d a = point(0), b = point(1), c = point(2);
                const Vec3d e1{b.x - a.x, b.y - a.y, b.z - a.z}, e2{c.x - a.x, c.y - a.y, c.z - a.z};
                const Vec3d p{d.y * e2.z - d.z * e2.y, d.z * e2.x - d.x * e2.z, d.x * e2.y - d.y * e2.x};
                const double det = e1.x * p.x + e1.y * p.y + e1.z * p.z;
                if (std::fabs(det) < 1e-12) continue;
                const double inv = 1.0 / det;
                const Vec3d s{origin.x - a.x, origin.y - a.y, origin.z - a.z};
                const double u = (s.x * p.x + s.y * p.y + s.z * p.z) * inv;
                if (u < -1e-9 || u > 1.0 + 1e-9) continue;
                const Vec3d q{s.y * e1.z - s.z * e1.y, s.z * e1.x - s.x * e1.z, s.x * e1.y - s.y * e1.x};
                const double v = (d.x * q.x + d.y * q.y + d.z * q.z) * inv;
                if (v < -1e-9 || u + v > 1.0 + 1e-9) continue;
                const double tHit = (e2.x * q.x + e2.y * q.y + e2.z * q.z) * inv;
                if (tHit > 0.0 && tHit < best) best = tHit;
            }
        }
        EXPECT_EQ(hit.hit, best < 1e8);
        if (hit.hit && best < 1e8) {
            hits++;
            // Measured along the surface's normal: on a grazing ray the same 1/64 voxel of surface
            // offset (R32) stretches along the ray
            const double facing = std::fabs(hit.normal.x * d.x + hit.normal.y * d.y + hit.normal.z * d.z);
            EXPECT_LE(std::fabs(hit.distance - best) * facing, 1.0 / 64.0) << i;
        }
    }
    EXPECT_GT(hits, 50);
}

TEST(VoxelSample, GetMaterialAfterPaint) {
    Store store(desc(64, 64, 64));
    EditOp op;
    op.kind = EditKind::Paint;
    op.shape = sphereShape(32.0, 32.0, 32.0, 5.0);
    op.material = 4;
    op.weight = 0.75;
    ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&op, 1)).ok);
    const MaterialSample sample = materialAtPoint(store, Vec3d{32.0, 32.0, 32.0});
    EXPECT_EQ(sample.slot, 4);
    EXPECT_EQ(sample.second, 0);
    EXPECT_EQ(sample.weight, 255 - 191);
    EXPECT_EQ(materialAtPoint(store, Vec3d{5.0, 5.0, 5.0}).slot, 0);
}

TEST(VoxelSample, HashIsIndependentOfRepresentation) {
    Store a(desc(64, 64, 64));
    a.setBase(sphereBase(32.0, 32.0, 32.0, 20.0));
    const uint64_t lazily = contentHash(a);
    for (int z = 0; z < 4; z++) {
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) a.content(BlockKey{0, x, y, z});
        }
    }
    EXPECT_EQ(contentHash(a), lazily);
}

TEST(VoxelSample, ConnectivityTunnelAndSealedCavity) {
    Store store(desc(64, 64, 64));
    store.setBase(std::make_shared<FunctionBase>([](double, double y, double) { return y - 50.0; }));
    const EditOp cavity = digOp(sphereShape(32.0, 20.0, 32.0, 5.0));
    ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&cavity, 1)).ok);
    const Int3 chamber{32, 20, 32}, sky{32, 60, 32};
    ConnectivityResult sealed = connectivity(store, &chamber, &sky);
    EXPECT_TRUE(sealed.fromInAir);
    EXPECT_TRUE(sealed.toInAir);
    EXPECT_FALSE(sealed.connected);
    EXPECT_EQ(sealed.components.size(), 2u);
    int64_t cavityVoxels = 0;
    for (const AirComponent& component : sealed.components) {
        if (!component.openToSky) cavityVoxels = component.voxels;
    }
    EXPECT_GT(cavityVoxels, 400);
    EXPECT_LT(cavityVoxels, 600);
    Shape tunnel;
    tunnel.kind = ShapeKind::Capsule;
    tunnel.from = Vec3d{32.0, 20.0, 32.0};
    tunnel.to = Vec3d{32.0, 55.0, 32.0};
    tunnel.radius = 2.5;
    const EditOp dig = digOp(tunnel);
    ASSERT_TRUE(applyEdit(store, std::span<const EditOp>(&dig, 1)).ok);
    ConnectivityResult open = connectivity(store, &chamber, &sky);
    EXPECT_TRUE(open.connected);
    EXPECT_EQ(open.components.size(), 1u);
    EXPECT_TRUE(open.components[0].openToSky);
}

TEST(VoxelRuntime, EventOrderAndDebrisDeterministic) {
    Store a(desc(64, 64, 64));
    Store b(desc(64, 64, 64));
    a.setBase(std::make_shared<FunctionBase>([](double, double y, double) { return y - 40.0; }));
    b.setBase(std::make_shared<FunctionBase>([](double, double y, double) { return y - 40.0; }));
    EditOptions options;
    options.debrisCount = 16;
    options.debrisSeed = 42;
    const EditOp op = digOp(sphereShape(32.0, 38.0, 32.0, 5.0));
    const EditResult ra = applyEdit(a, std::span<const EditOp>(&op, 1), options);
    const EditResult rb = applyEdit(b, std::span<const EditOp>(&op, 1), options);
    ASSERT_FALSE(ra.debrisPoints.empty());
    EXPECT_LE(ra.debrisPoints.size(), 16u);
    ASSERT_EQ(ra.debrisPoints.size(), rb.debrisPoints.size());
    for (size_t i = 0; i < ra.debrisPoints.size(); i++) {
        EXPECT_EQ(ra.debrisPoints[i].x, rb.debrisPoints[i].x);
        EXPECT_EQ(ra.debrisPoints[i].y, rb.debrisPoints[i].y);
        EXPECT_EQ(ra.debrisPoints[i].z, rb.debrisPoints[i].z);
        // On a removed sample: it was solid in the base, inside the dig
        EXPECT_LT(ra.debrisPoints[i].y, 40.0);
    }
    options.debrisSeed = 43;
    Store c(desc(64, 64, 64));
    c.setBase(std::make_shared<FunctionBase>([](double, double y, double) { return y - 40.0; }));
    const EditResult rc = applyEdit(c, std::span<const EditOp>(&op, 1), options);
    bool differs = false;
    for (size_t i = 0; i < std::min(ra.debrisPoints.size(), rc.debrisPoints.size()); i++) {
        differs = differs || ra.debrisPoints[i].x != rc.debrisPoints[i].x || ra.debrisPoints[i].z != rc.debrisPoints[i].z;
    }
    EXPECT_TRUE(differs);
    EXPECT_GT(ra.removedVolume, 0.0);
    EXPECT_EQ(ra.addedVolume, 0.0);
    EXPECT_DOUBLE_EQ(ra.removedByMaterial[0], ra.removedVolume);
}

TEST(VoxelEditCmd, DeltaIsExactAndCountsMemory) {
    Store store(desc(64, 64, 64, 2));
    store.setBase(wavyBase(30.0));
    const uint64_t original = contentHash(store);
    EditOptions options;
    options.captureDelta = true;
    std::vector<std::vector<SlotChange>> history;
    std::vector<uint64_t> hashes;
    std::mt19937 random(21);
    for (int i = 0; i < 100; i++) {
        const EditOp op = (random() % 2) ? digOp(sphereShape(random() % 64, 20 + random() % 20, random() % 64, 1.0 + random() % 5)) : addOp(sphereShape(random() % 64, 20 + random() % 20, random() % 64, 1.0 + random() % 5), static_cast<int>(random() % 4));
        EditResult result = applyEdit(store, std::span<const EditOp>(&op, 1), options);
        ASSERT_TRUE(result.ok);
        history.push_back(std::move(result.delta));
        hashes.push_back(contentHash(store));
    }
    for (size_t i = history.size(); i > 0; i--) {
        applyDelta(store, history[i - 1], true);
        ASSERT_EQ(contentHash(store), i > 1 ? hashes[i - 2] : original);
    }
    for (size_t i = 0; i < history.size(); i++) {
        applyDelta(store, history[i], false);
        ASSERT_EQ(contentHash(store), hashes[i]);
    }
}
