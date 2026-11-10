// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// je-bench-picking: the budgets of triangle picking (docs/specs/S26-scene-view.md R28-R31, AC17).
//
//   je-bench-picking [--json <file>] [--workers <n>]      default file: je-bench-picking.json
//
// Pick latency (p99 of 10k random rays after the builds) on two 1M-triangle scenes, raycastAll,
// the scalar path; BVH build rate, gather time, memory and disk load of a 1M-triangle mesh;
// SceneBvh build and refits over 100k leaves; posing a 50k-vertex skinned mesh; the per-frame
// bookkeeping. The thresholds are in tools/perf/bench-thresholds.json (tools/perf/check_bench.py
// compares); wall-clock budgets hold on the benchmark server E only.

#include "Engine.h"
#include "component/Transform.h"
#include "Scene.h"
#include "component/MeshComponent.h"
#include "jobs/JobSystem.h"
#include "object/Primitive.h"
#include "picking/MeshBvh.h"
#include "picking/MeshPose.h"
#include "picking/MeshRaycast.h"
#include "picking/PickCache.h"
#include "picking/PickGeometry.h"
#include "picking/SceneBvh.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/RenderSystem.h"
#include "util/Angle.h"

#include "BenchUtil.h"
#include "JobsTestSystem.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace jasperengine;

namespace {

    int kWorkers = 8;

    void settle(Scene& scene){
        scene.getSystem<MeshSystem>()->update(0);
        scene.getSystem<RenderSystem>()->update(0);
    }

    double millisecondsSince(bench::Clock::time_point start){
        return bench::secondsSince(start) * 1000.0;
    }

    struct BenchRay { Vector3 origin; Vector3 direction; };

    std::vector<BenchRay> raysThrough(uint32_t count, uint32_t seed, Vector3 mn, Vector3 mx, bool fromAbove){
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::vector<BenchRay> rays;
        for (uint32_t i = 0; i < count; i++){
            const Vector3 target(mn.x + (mx.x - mn.x) * unit(rng), mn.y + (mx.y - mn.y) * unit(rng), mn.z + (mx.z - mn.z) * unit(rng));
            Vector3 dir = fromAbove ? Vector3(unit(rng) - 0.5f, -1.0f, unit(rng) - 0.5f) : Vector3(unit(rng) - 0.5f, unit(rng) - 0.5f, unit(rng) - 0.5f);
            if (dir.squaredLength() < 1e-6f) dir = Vector3(0, -1, 0);
            dir.normalize();
            rays.push_back({target - dir * 400.0f, dir});
        }
        return rays;
    }

    // p99 of one closest (or all) query per ray, ms
    double pickP99(Scene& scene, const std::vector<BenchRay>& rays, bool all, size_t& hits){
        std::vector<double> times;
        times.reserve(rays.size());
        hits = 0;
        for (const BenchRay& ray : rays){
            const auto start = bench::Clock::now();
            if (all){
                hits += MeshRaycast::raycastAll(&scene, ray.origin, ray.direction, 1000.0f, LAYER_MASK_EVERYTHING).size();
            }else{
                hits += MeshRaycast::raycast(&scene, ray.origin, ray.direction, 1000.0f, LAYER_MASK_EVERYTHING).hit ? 1 : 0;
            }
            times.push_back(millisecondsSince(start));
        }
        return bench::percentile(times, 0.99);
    }

    // BF1/S02 style: `warmups` untimed runs, then `reps` timed ones (ms each)
    template<class Run>
    std::vector<double> repeatTimed(int warmups, int reps, Run&& run){
        for (int i = 0; i < warmups; i++){
            run();
        }
        std::vector<double> times;
        times.reserve((size_t)reps);
        for (int i = 0; i < reps; i++){
            const auto start = bench::Clock::now();
            run();
            times.push_back(millisecondsSince(start));
        }
        return times;
    }

    // the gated median as `name`, the spread as name_min and name_max (scaled, e.g. per 1M)
    void addTimed(bench::Results& results, const std::string& name, const std::vector<double>& times, double scale, const char* unit){
        results.add(name, bench::percentile(times, 0.5) * scale, unit);
        results.add(name + "_min", bench::percentile(times, 0.0) * scale, unit);
        results.add(name + "_max", bench::percentile(times, 1.0) * scale, unit);
    }

    void waitForBuilds(Scene& scene){
        MeshRaycast::prepare(&scene);
        auto meshes = scene.getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            if (PickCache::EntryPtr entry = scene.getPickBvh().meshEntry(meshes->getEntity(i), true)){
                PickCache::get().wait(entry);
            }
        }
    }

    // a 50k-vertex grid, every vertex on 4 bones
    // a side x side vertex grid in the y = 0 plane ((side - 1)^2 * 2 triangles, 32-bit indices),
    // `spacing` apart from `origin`; skinned: 4 influences per vertex over 32 bones
    Entity makeGrid(Scene& scene, uint32_t side, bool skinned, float spacing, Vector3 origin){
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity);
        scene.addComponent<MeshComponent>(entity);
        MeshComponent& mesh = scene.getComponent<MeshComponent>(entity);
        mesh.buffer.addAttribute(AttributeType::POSITION, 3);
        if (skinned){
            mesh.buffer.addAttribute(AttributeType::BONEIDS, 4);
            mesh.buffer.addAttribute(AttributeType::BONEWEIGHTS, 4);
        }
        Attribute* position = mesh.buffer.getAttribute(AttributeType::POSITION);
        Attribute* ids = skinned ? mesh.buffer.getAttribute(AttributeType::BONEIDS) : nullptr;
        Attribute* weights = skinned ? mesh.buffer.getAttribute(AttributeType::BONEWEIGHTS) : nullptr;
        for (uint32_t z = 0; z < side; z++){
            for (uint32_t x = 0; x < side; x++){
                mesh.buffer.addVector3(position, origin + Vector3((float)x * spacing, 0.0f, (float)z * spacing));
                if (skinned){
                    const float b = (float)((x / 16) % 28);
                    mesh.buffer.addVector4(ids, Vector4(b, b + 1, b + 2, b + 3));
                    mesh.buffer.addVector4(weights, Vector4(0.4f, 0.3f, 0.2f, 0.1f));
                }
            }
        }
        // 32-bit indices in an external buffer, as a glTF primitive has them
        mesh.numExternalBuffers = 1;
        ExternalBuffer& indices = mesh.eBuffers[0];
        indices.setName("indices32");
        indices.setType(BufferType::INDEX_BUFFER);
        indices.setRenderAttributes(false);
        std::vector<uint32_t> list;
        for (uint32_t z = 0; z + 1 < side; z++){
            for (uint32_t x = 0; x + 1 < side; x++){
                const uint32_t a = z * side + x, b = a + 1, c = a + side, d = c + 1;
                list.insert(list.end(), {a, c, b, b, c, d});
            }
        }
        indices.importData(list.data(), list.size() * sizeof(uint32_t));
        indices.copyToOwnData();
        mesh.numSubmeshes = 1;
        Submesh& submesh = mesh.submeshes[0];
        submesh.hasSkinning = skinned;
        auto attribute = [&](AttributeType type, const Attribute& source){
            Attribute copy = source;
            copy.setBufferName("vertices");
            submesh.attributes[type] = copy;
        };
        attribute(AttributeType::POSITION, *position);
        if (skinned){
            attribute(AttributeType::BONEIDS, *ids);
            attribute(AttributeType::BONEWEIGHTS, *weights);
        }
        submesh.attributes[AttributeType::INDEX] = Attribute(AttributeDataType::UNSIGNED_INT, "indices32", 1, 0, (unsigned int)list.size(), false, false);
        for (int b = 0; b < 32; b++){
            mesh.bonesMatrix[(size_t)b] = Matrix4();
        }
        scene.getSystem<MeshSystem>()->calculateMeshAABB(mesh);
        return entity;
    }

}

int main(int argc, char** argv){
    const int hardware = (int)std::thread::hardware_concurrency();
    if (hardware > 0){
        kWorkers = std::max(1, std::min(8, hardware - 1));
    }
    for (int i = 1; i + 1 < argc; i++){
        if (std::string(argv[i]) == "--workers"){
            kWorkers = std::max(1, std::atoi(argv[i + 1]));
        }
    }
    jobs::Config config;
    config.workers = kWorkers;
    jobs::initialize(config);
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    bench::Results results("je-bench-picking");
    std::printf("je-bench-picking: %d workers, simd %s\n", jobs::system().getWorkerCount(), pickSimdPathName(getPickSimdPath()));

    // ---- R28: 1,000 entities x ~1k triangles
    {
        auto scene = std::make_unique<Scene>();
        std::vector<std::unique_ptr<Primitive>> objects;
        std::mt19937 rng(1);
        std::uniform_real_distribution<float> position(-100.0f, 100.0f);
        std::uniform_real_distribution<float> scale(0.5f, 3.0f);
        size_t triangles = 0;
        for (int i = 0; i < 1000; i++){
            auto sphere = std::make_unique<Primitive>(scene.get());
            sphere->createSphere(1.0f, 20 + (i % 10), 25);
            sphere->setPosition(position(rng), position(rng), position(rng));
            sphere->setScale(scale(rng));
            objects.push_back(std::move(sphere));
        }
        settle(*scene);
        waitForBuilds(*scene);
        auto meshes = scene->getComponentArray<MeshComponent>();
        for (size_t i = 0; i < meshes->size(); i++){
            PickGeometryLayout layout;
            if (picking::resolveSources(meshes->getComponentFromIndex(i), layout)) triangles += picking::countTriangles(layout);
        }
        const std::vector<BenchRay> rays = raysThrough(10000, 2, Vector3(-100, -100, -100), Vector3(100, 100, 100), false);
        size_t hits = 0;
        pickP99(*scene, rays, false, hits); // warm
        results.add("pick_p99_ms_1000x1k", pickP99(*scene, rays, false, hits), "ms");
        results.add("pick_hits_1000x1k", (double)hits, "rays");
        results.add("scene_triangles_1000x1k", (double)triangles, "tris");
        results.add("raycast_all_p99_ms", pickP99(*scene, rays, true, hits), "ms");
        const PickSimdPath native = getPickSimdPath();
        setPickSimdPath(PickSimdPath::SCALAR);
        const double scalarA = pickP99(*scene, rays, false, hits);
        setPickSimdPath(native);
        results.add("pick_scalar_p99_ms_1000x1k", scalarA, "ms");

        // R31: what a frame costs picking when nothing is queried (the dirty marks of 1,000
        // moved meshes)
        std::vector<double> frames;
        for (int frame = 0; frame < 200; frame++){
            const auto start = bench::Clock::now();
            for (size_t i = 0; i < meshes->size(); i++){
                scene->markPickDirty(meshes->getEntity(i));
            }
            frames.push_back(millisecondsSince(start));
        }
        results.add("frame_bookkeeping_1000_dirty_ms", bench::percentile(frames, 0.5), "ms");
        objects.clear();
    }

    // ---- R28, R29: one 1M-triangle mesh and 500 small ones
    {
        auto scene = std::make_unique<Scene>();
        // 708 x 708 vertices: 999,698 triangles over 200 x 200 units
        const Entity groundEntity = makeGrid(*scene, 708, false, 200.0f / 707.0f, Vector3(-100.0f, 0.0f, -100.0f));
        std::vector<std::unique_ptr<Primitive>> boxes;
        std::mt19937 rng(3);
        std::uniform_real_distribution<float> position(-95.0f, 95.0f);
        for (int i = 0; i < 500; i++){
            auto box = std::make_unique<Primitive>(scene.get());
            box->createBox(1, 1, 1);
            box->setPosition(position(rng), 0.5f, position(rng));
            boxes.push_back(std::move(box));
        }
        settle(*scene);

        MeshComponent& groundMesh = scene->getComponent<MeshComponent>(groundEntity);
        PickGeometry geometry;
        const std::vector<double> gathers = repeatTimed(2, 15, [&](){ picking::gather(groundMesh, geometry); });
        const double triangles = (double)geometry.getTriangleCount();
        results.add("ground_triangles", triangles, "tris");
        addTimed(results, "gather_ms_per_1m_tris", gathers, 1.0e6 / triangles, "ms");

        MeshBvh bvh;
        const std::vector<double> buildTimes = repeatTimed(1, 5, [&](){ bvh = MeshBvh::build(geometry); });
        // a throughput: the median build's, and the slowest and fastest builds'
        results.add("bvh_build_tris_per_s", triangles / (bench::percentile(buildTimes, 0.5) / 1000.0), "tris/s");
        results.add("bvh_build_tris_per_s_min", triangles / (bench::percentile(buildTimes, 1.0) / 1000.0), "tris/s");
        results.add("bvh_build_tris_per_s_max", triangles / (bench::percentile(buildTimes, 0.0) / 1000.0), "tris/s");
        results.add("bvh_bytes_per_triangle", (double)bvh.memoryBytes() / triangles, "B");
        results.add("bvh_nodes_per_1k_triangles", (double)bvh.getNodeCount() * 1000.0 / triangles, "nodes");
        {
            PickGeometryLayout layout;
            const auto resolveStart = bench::Clock::now();
            picking::resolveSources(groundMesh, layout);
            results.add("resolve_sources_ms", millisecondsSince(resolveStart), "ms");
        }

        // the disk cache's load: the file read back and deserialized against the geometry
        std::vector<uint8_t> bytes;
        bvh.serialize(bytes);
        const std::filesystem::path file = std::filesystem::temp_directory_path() / "je-bench-picking.bvh";
        {
            std::ofstream out(file, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        }
        // each load: the file read into a fresh buffer and deserialized into a fresh tree
        std::vector<double> reads, deserializes;
        bool ok = true;
        const std::vector<double> loads = repeatTimed(2, 15, [&](){
            const auto readStart = bench::Clock::now();
            std::vector<uint8_t> read(std::filesystem::file_size(file));
            {
                std::ifstream in(file, std::ios::binary);
                in.read(reinterpret_cast<char*>(read.data()), (std::streamsize)read.size());
            }
            reads.push_back(millisecondsSince(readStart));
            MeshBvh loaded;
            const auto deserializeStart = bench::Clock::now();
            ok = loaded.deserialize(read, geometry) && ok;
            deserializes.push_back(millisecondsSince(deserializeStart));
        });
        results.add("disk_read_ms_1m", bench::percentile(reads, 0.5) * 1.0e6 / triangles, "ms");
        results.add("deserialize_ms_1m", bench::percentile(deserializes, 0.5) * 1.0e6 / triangles, "ms");
        if (ok){
            addTimed(results, "disk_load_ms_1m", loads, 1.0e6 / triangles, "ms");
        }else{
            results.add("disk_load_ms_1m", 1.0e9, "ms");
        }
        std::filesystem::remove(file);

        waitForBuilds(*scene);
        const std::vector<BenchRay> rays = raysThrough(10000, 4, Vector3(-100, 0, -100), Vector3(100, 1, 100), true);
        size_t hits = 0;
        pickP99(*scene, rays, false, hits);
        results.add("pick_p99_ms_1m_plus_500", pickP99(*scene, rays, false, hits), "ms");
        results.add("pick_hits_1m_plus_500", (double)hits, "rays");
        const PickSimdPath native = getPickSimdPath();
        setPickSimdPath(PickSimdPath::SCALAR);
        const double scalarB = pickP99(*scene, rays, false, hits);
        setPickSimdPath(native);
        results.add("pick_scalar_p99_ms_1m_plus_500", scalarB, "ms");
        boxes.clear();
    }

    // ---- R29: SceneBvh over 100k leaves
    {
        std::mt19937 rng(5);
        std::uniform_real_distribution<float> position(-500.0f, 500.0f);
        std::uniform_real_distribution<float> size(0.1f, 3.0f);
        std::vector<AABB> boxes(100000);
        for (AABB& box : boxes){
            const Vector3 c(position(rng), position(rng), position(rng));
            const Vector3 h(size(rng), size(rng), size(rng));
            box = AABB(c - h, c + h);
        }
        SceneBvhTree tree;
        const std::vector<double> builds = repeatTimed(2, 15, [&](){ tree.build(boxes); });
        addTimed(results, "scene_bvh_build_ms_100k", builds, 1.0, "ms");

        std::vector<double> serial, parallel, dirty;
        std::vector<uint32_t> hundred;
        for (uint32_t i = 0; i < 100; i++){
            hundred.push_back(i * 997u % 100000u);
        }
        for (int i = 0; i < 20; i++){
            for (uint32_t leaf = 0; leaf < boxes.size(); leaf++){
                tree.setLeafBox(leaf, boxes[leaf]);
            }
            auto start = bench::Clock::now();
            tree.refitAll(false);
            serial.push_back(millisecondsSince(start));
            start = bench::Clock::now();
            tree.refitAll(true);
            parallel.push_back(millisecondsSince(start));
            for (uint32_t leaf : hundred){
                const Vector3 delta(0.01f * (float)i, 0.0f, 0.0f);
                tree.setLeafBox(leaf, AABB(boxes[leaf].getMinimum() + delta, boxes[leaf].getMaximum() + delta));
            }
            start = bench::Clock::now();
            tree.refit(hundred);
            dirty.push_back(millisecondsSince(start));
        }
        results.add("scene_bvh_refit_ms_100k_serial", bench::percentile(serial, 0.5), "ms");
        results.add("scene_bvh_refit_ms_100k_parallel", bench::percentile(parallel, 0.5), "ms");
        results.add("scene_bvh_refit_ms_100_dirty", bench::percentile(dirty, 0.5), "ms");
    }

    // ---- R30: a 50k-vertex, 4-influence skinned mesh
    {
        auto scene = std::make_unique<Scene>();
        const Entity character = makeGrid(*scene, 224, true, 0.1f, Vector3(0, 0, 0)); // 50,176 vertices
        settle(*scene);
        SceneBvh& bvh = scene->getPickBvh();
        const PickCache::EntryPtr entry = bvh.meshEntry(character, true);
        const MeshBvh* bindPose = entry ? PickCache::get().wait(entry) : nullptr;
        MeshComponent& mesh = scene->getComponent<MeshComponent>(character);
        std::vector<double> poses, repeats, statics;
        for (int i = 0; bindPose && i < 30; i++){
            for (int b = 0; b < 32; b++){
                mesh.bonesMatrix[(size_t)b] = Matrix4::rotateYMatrix(Angle::degToDefault(0.5f * (float)(i + 1) * (float)b));
            }
            auto start = bench::Clock::now();
            bvh.posedTree(character, *bindPose, entry->hash);
            poses.push_back(millisecondsSince(start));
            start = bench::Clock::now();
            const MeshBvh* posed = bvh.posedTree(character, *bindPose, entry->hash);
            MeshBvhRay ray;
            ray.origin = Vector3(11.0f, 10.0f, 11.0f);
            ray.direction = Vector3(0.01f, -1.0f, 0.02f);
            MeshBvhHit hit;
            posed->raycast(ray, hit);
            repeats.push_back(millisecondsSince(start));
            start = bench::Clock::now();
            bindPose->raycast(ray, hit);
            statics.push_back(millisecondsSince(start));
        }
        // 30 poses, the first a cold one: the median
        addTimed(results, "skinned_pose_refit_ms_50k", poses, 1.0, "ms");
        results.add("skinned_repeat_pick_ms", bench::percentile(repeats, 0.5), "ms");
        results.add("static_pick_ms", bench::percentile(statics, 0.5), "ms");
        // the two halves of a pose: the vertices, then the refit
        if (bindPose){
            PickGeometry geometry;
            PickGeometryLayout layout;
            picking::gather(mesh, geometry, &layout);
            std::vector<Vector3> posedPositions;
            std::vector<double> skin, refit;
            MeshBvh copy = *bindPose;
            const std::vector<uint32_t> corners = copy.leafCorners(geometry.indices);
            for (int i = 0; i < 20; i++){
                auto start = bench::Clock::now();
                picking::poseGeometry(mesh, layout, geometry.positions, posedPositions);
                skin.push_back(millisecondsSince(start));
                start = bench::Clock::now();
                copy.refitCorners(posedPositions, corners);
                refit.push_back(millisecondsSince(start));
            }
            results.add("skinned_pose_vertices_ms_50k", bench::percentile(skin, 0.5), "ms");
            results.add("skinned_refit_ms_50k", bench::percentile(refit, 0.5), "ms");
        }
    }

    const char* path = bench::jsonPath(argc, argv, "je-bench-picking.json");
    if (!results.write(path)){
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::printf("wrote %s\n", path);
    jobs::shutdown();
    return 0;
}
