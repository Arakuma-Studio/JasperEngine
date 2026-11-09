// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Voxel Volume budgets (docs/specs/T6-voxel-volumes.md R55, R56; AC41, AC42):
//   je-bench-voxel [--json <file>]
// Medians on one thread (Release); check_bench.py gates them with tools/perf/bench-thresholds.json on
// a quiet run. Latency is in game frames, not wall time: with JE_JOB_WORKERS=7, 2, 0 it is R56's table.

#include "BenchUtil.h"
#include "JobsTestSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/RigidbodyComponent.h"
#include "component/Transform.h"
#include "component/VoxelVolumeComponent.h"
#include "physics/JoltShapeBuilder.h"
#include "subsystem/PhysicsSystem.h"
#include "subsystem/VoxelSystem.h"
#include "voxel/TransvoxelMesher.h"
#include "voxel/VoxelCodec.h"
#include "voxel/VoxelEdit.h"
#include "voxel/VoxelFile.h"
#include "voxel/VoxelGenerator.h"
#include "voxel/VoxelStore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::voxel;

namespace {

    template<typename Fn>
    double medianMs(int runs, Fn&& fn) {
        std::vector<double> samples;
        for (int r = 0; r < runs; r++) {
            const auto start = bench::Clock::now();
            fn();
            samples.push_back(bench::secondsSince(start) * 1.0e3);
        }
        return bench::percentile(samples, 0.5);
    }

    class TerrainLike final : public BaseSource {
    public:
        void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const override {
            for (int z = 0; z < BLOCK_SIZE; z++) {
                for (int y = 0; y < BLOCK_SIZE; y++) {
                    for (int x = 0; x < BLOCK_SIZE; x++) {
                        const double px = origin.x + x * spacing, py = origin.y + y * spacing, pz = origin.z + z * spacing;
                        const int index = sampleIndex(x, y, z);
                        sdf[index] = encodeSdf(py - 24.0 - 2.5 * std::sin(px * 0.21) * std::cos(pz * 0.17));
                        material[index] = 0;
                    }
                }
            }
        }
    };

    class SignNoise final : public BaseSource {
    public:
        void evaluate(const Int3& origin, int spacing, int16_t* sdf, uint16_t* material) const override {
            for (int z = 0; z < BLOCK_SIZE; z++) {
                for (int y = 0; y < BLOCK_SIZE; y++) {
                    for (int x = 0; x < BLOCK_SIZE; x++) {
                        const int px = origin.x + x * spacing, py = origin.y + y * spacing, pz = origin.z + z * spacing;
                        sdf[sampleIndex(x, y, z)] = (px ^ py ^ pz) & 1 ? 2048 : -2048;
                        material[sampleIndex(x, y, z)] = packMaterial(px % 16, py % 16, (pz * 7) % 255);
                    }
                }
            }
        }
    };

    Store& filled(Store& store, std::shared_ptr<const BaseSource> base) {
        store.setBase(std::move(base));
        const Int3 count = store.blockCount(0);
        for (int z = 0; z < count.z; z++) {
            for (int y = 0; y < count.y; y++) {
                for (int x = 0; x < count.x; x++) store.content(BlockKey{0, x, y, z});
            }
        }
        return store;
    }

    VolumeDesc descOf(int size) {
        VolumeDesc desc;
        desc.dimensions = Int3{size, size, size};
        desc.levels = 2;
        return desc;
    }

    // The frames from a dig to live collision (R56), on a real system with a real physics world
    int latencyFrames() {
        Scene scene;
        Engine::setFixedDeltatime(1.0f / 60.0f);
        scene.getSystem<PhysicsSystem>()->setGravity3D(0.0f, -9.81f, 0.0f);
        GeneratorDesc generator;
        generator.kind = "solid_ground";
        generator.height = 32.0;
        const std::shared_ptr<VoxelAsset> asset = VoxelAssets::create("mem://latency", descOf(128), generator, 1);
        const Entity focus = scene.createEntity();
        scene.addComponent<Transform>(focus, {});
        scene.getComponent<Transform>(focus).worldPosition = Vector3(64.0f, 34.0f, 64.0f);
        const Entity owner = scene.createEntity();
        scene.addComponent<Transform>(owner, {});
        VoxelVolumeComponent component;
        component.volumeData.path = "mem://latency";
        component.quality = VoxelQuality::High;
        component.focus = focus;
        scene.addComponent<VoxelVolumeComponent>(owner, component);
        const std::shared_ptr<VoxelSystem> system = scene.getSystem<VoxelSystem>();
        for (int i = 0; i < 60; i++) {
            scene.fixedUpdate(1.0f / 60.0f);
            scene.update(1.0f / 60.0f);
        }
        system->finishPendingWork();
        int updates = 0;
        scene.getComponent<VoxelVolumeComponent>(owner).onCollisionUpdated.add("bench", [&updates](AABB) { updates++; });
        EditOp op;
        op.kind = EditKind::Dig;
        op.shape.center = Vec3d{64.0, 32.0, 64.0};
        op.shape.radius = 3.0;
        system->edit(owner, std::span<const EditOp>(&op, 1), EditOptions{});
        for (int frame = 1; frame < 240; frame++) {
            scene.fixedUpdate(1.0f / 60.0f);
            scene.update(1.0f / 60.0f);
            jobs::pump(4.0);
            if (updates > 0) return frame;
        }
        return 240;
    }

}

int main(int argc, char** argv) {
    bench::Results results("je-bench-voxel");

    Store plain(descOf(64));
    filled(plain, std::make_shared<TerrainLike>());
    const BlockKey surfaceKey{0, 1, 1, 1};
    const auto surfaceView = plain.readView(surfaceKey, 0);
    MeshOut mesh;
    MeshRequest regular;
    regular.key = surfaceKey;
    const double meshMs = medianMs(101, [&] { meshBlock(*surfaceView, regular, mesh); });
    results.add("mesh_block_regular_ms", meshMs, "ms");
    results.add("mesh_block_regular_triangles", mesh.triangleCount, "");

    const auto maskedView = plain.readView(surfaceKey, 1u << SIDE_NEG_X);
    MeshRequest masked = regular;
    masked.sideMask = 1u << SIDE_NEG_X;
    MeshOut maskedMesh;
    results.add("mesh_block_masked_side_ms", medianMs(101, [&] { meshBlock(*maskedView, masked, maskedMesh); }) - meshMs, "ms");

    MeshRequest collisionOnly = regular;
    collisionOnly.mode = MeshMode::CollisionOnly;
    MeshOut collisionMesh;
    const double collisionMs = medianMs(101, [&] { meshBlock(*surfaceView, collisionOnly, collisionMesh); });
    results.add("mesh_block_collision_ratio", meshMs > 0.0 ? collisionMs / meshMs : 0.0, "");

    Store noisy(descOf(32));
    filled(noisy, std::make_shared<SignNoise>());
    const auto noisyView = noisy.readView(BlockKey{0, 0, 0, 0}, 0);
    MeshRequest worst;
    worst.key = BlockKey{0, 0, 0, 0};
    MeshOut worstMesh;
    results.add("mesh_block_worst_ms", medianMs(31, [&] { meshBlock(*noisyView, worst, worstMesh); }), "ms");

    {
        std::vector<Vector3> vertices;
        for (const MeshVertex& v : collisionMesh.vertices) vertices.emplace_back(fixedToLocal(v.position[0], 1.0f), fixedToLocal(v.position[1], 1.0f), fixedToLocal(v.position[2], 1.0f));
        const std::vector<uint32_t> indices(collisionMesh.indices.begin(), collisionMesh.indices.end());
        std::string error;
        results.add("mesh_shape_typical_ms", medianMs(51, [&] { JoltShapeBuilder::buildMeshShape(vertices, indices, &error, nullptr); }), "ms");
    }

    const auto digAt = [&](double radius) {
        Store store(descOf(128));
        filled(store, std::make_shared<TerrainLike>());
        EditOp op;
        op.kind = EditKind::Dig;
        op.shape.center = Vec3d{64.0, 24.0, 64.0};
        op.shape.radius = radius;
        EditOptions dry;
        dry.dryRun = true;
        return medianMs(21, [&] { applyEdit(store, std::span<const EditOp>(&op, 1), dry); });
    };
    results.add("dig_sphere_4_ms", digAt(4.0), "ms");
    results.add("dig_sphere_16_ms", digAt(16.0), "ms");

    // Decode one block; evaluate one flat base block
    {
        const BlockContent content = plain.content(surfaceKey);
        std::vector<uint8_t> bytes;
        drle16Encode(reinterpret_cast<const uint16_t*>(content.data->sdf()), BLOCK_SAMPLES, bytes);
        uint16_t out[BLOCK_SAMPLES];
        results.add("decode_block_ms", medianMs(201, [&] { drle16Decode(bytes.data(), bytes.size(), out, BLOCK_SAMPLES); }), "ms");
        const std::shared_ptr<const BaseSource> flat = makeSolidGroundBase(24.0, 1.0, 0);
        int16_t sdf[BLOCK_SAMPLES];
        uint16_t material[BLOCK_SAMPLES];
        results.add("evaluate_base_flat_ms", medianMs(201, [&] { flat->evaluate(Int3{0, 16, 0}, 1, sdf, material); }), "ms");
    }

    // The idle system
    {
        Scene scene;
        const std::shared_ptr<VoxelAsset> asset = VoxelAssets::create("mem://idle", descOf(128), GeneratorDesc{}, 1);
        const Entity owner = scene.createEntity();
        scene.addComponent<Transform>(owner, {});
        VoxelVolumeComponent component;
        component.volumeData.path = "mem://idle";
        component.collision = false;
        scene.addComponent<VoxelVolumeComponent>(owner, component);
        const std::shared_ptr<VoxelSystem> system = scene.getSystem<VoxelSystem>();
        scene.update(1.0 / 60.0);
        system->finishPendingWork();
        results.add("system_update_idle_ms", medianMs(501, [&] { system->update(1.0 / 60.0); }), "ms");
    }

    results.add("dig_to_collision_frames", latencyFrames(), "frames");
    const char* path = bench::jsonPath(argc, argv, "je-bench-voxel.json");
    if (!results.write(path)) {
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    return 0;
}
