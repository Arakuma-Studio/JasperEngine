// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The spline budgets (docs/specs/S18-splines.md R18-R22, AC16), docs/perf/splines.md:
//   je-bench-splines [--json <file>] [--count-allocs]
// Every figure is the median of repeated runs on one thread (Release); check_bench.py gates them
// with tools/perf/bench-thresholds.json. The terrain bake's (R20) is an editor test
// (TerrainSplineBakeBench, editor/tests/util).

#include "BenchUtil.h"
#include "AllocationCounter.h"
#include "JobsTestSystem.h"

#include "Engine.h"
#include "Scene.h"
#include "component/SplineAnimateComponent.h"
#include "component/SplineComponent.h"
#include "component/Transform.h"
#include "spline/SplineCenterline.h"
#include "spline/SplineEvaluator.h"
#include "spline/SplineFillField.h"
#include "spline/SplineFillMesher.h"
#include "spline/SplineProfile.h"
#include "spline/SplineStations.h"
#include "spline/SplineStripBuilder.h"
#include "spline/SplineTileLayout.h"
#include "subsystem/SplineSystem.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    // a gentle curve of `knots` knots over `length` metres along +X
    spline::SplineData meander(int knots, float length, float radius){
        spline::SplineData data;
        std::vector<spline::SplineKnot> list;
        for (int i = 0; i < knots; i++){
            spline::SplineKnot knot;
            const float x = length * static_cast<float>(i) / static_cast<float>(knots - 1);
            knot.position = Vector3(x, 0.0f, 12.0f * std::sin(x * 0.02f));
            knot.radius = radius;
            list.push_back(knot);
        }
        data.setKnots(std::move(list));
        return data;
    }

    template<typename Fn>
    double medianMs(int runs, Fn&& fn){
        std::vector<double> samples;
        for (int r = 0; r < runs; r++){
            const auto start = bench::Clock::now();
            fn();
            samples.push_back(bench::secondsSince(start) * 1.0e3);
        }
        return bench::percentile(samples, 0.5);
    }

    volatile float sink = 0.0f;

}

int main(int argc, char** argv){
    bool countAllocs = false;
    for (int i = 1; i < argc; i++){
        if (std::string(argv[i]) == "--count-allocs") countAllocs = true;
    }
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    bench::Results results("je-bench-splines");

    // ---- R18: the core queries on a 100-knot spline
    {
        spline::SplineData data = meander(100, 1000.0f, 4.0f);
        data.ensureCache();
        const float length = spline::length(data);
        constexpr int queries = 200000;
        // the inputs made beforehand: the loops time the queries, not their arguments
        std::vector<float> distances(queries);
        std::vector<float> params(queries);
        for (int q = 0; q < queries; q++){
            distances[size_t(q)] = length * static_cast<float>(q) / queries;
            params[size_t(q)] = static_cast<float>(q) / queries;
        }
        const double evalMs = medianMs(9, [&](){
            float acc = 0.0f;
            for (int q = 0; q < queries; q++){
                acc += spline::evaluateAtDistance(data, distances[size_t(q)]).position.x;
            }
            sink = acc;
        });
        results.add("evaluate_at_distance_ns", evalMs * 1.0e6 / queries, "ns");
        // its two halves: the distance to (segment, u), and the frame there
        const double locateMs = medianMs(9, [&](){
            float acc = 0.0f;
            for (int q = 0; q < queries; q++){
                acc += spline::distanceToT(data, distances[size_t(q)]);
            }
            sink = acc;
        });
        results.add("distance_to_t_ns", locateMs * 1.0e6 / queries, "ns");
        const double frameMs = medianMs(9, [&](){
            float acc = 0.0f;
            for (int q = 0; q < queries; q++){
                acc += spline::evaluate(data, params[size_t(q)]).position.x;
            }
            sink = acc;
        });
        results.add("evaluate_t_ns", frameMs * 1.0e6 / queries, "ns");

        constexpr int closest = 20000;
        const double closestMs = medianMs(9, [&](){
            float acc = 0.0f;
            for (int q = 0; q < closest; q++){
                const float x = 1000.0f * static_cast<float>(q) / closest;
                acc += spline::closestPoint(data, Vector3(x, 3.0f, 20.0f * std::cos(x * 0.05f))).distance;
            }
            sink = acc;
        });
        results.add("closest_point_100_knots_us", closestMs * 1.0e3 / closest, "us");

        constexpr int rebuilds = 2000;
        const double rebuildMs = medianMs(9, [&](){
            for (int r = 0; r < rebuilds; r++){
                data.markDirty();
                data.ensureCache();
            }
        });
        results.add("cache_rebuild_100_knots_us", rebuildMs * 1.0e3 / rebuilds, "us");
        results.add("cache_bytes_100_knots", static_cast<double>(data.cacheBytes()), "B");
        results.add("cache_budget_100_knots", 4096.0 + 64.0 * 100.0, "B");
    }

    // ---- R19: a 1 km Crown road (7-point profile, stations <= 1 m), the stations to the chunks
    {
        const spline::SplineData road = meander(11, 1000.0f, 4.0f);
        spline::SplineProfileParams profileParams;
        profileParams.shape = spline::SplineProfileShape::Crown;
        profileParams.width = 8.0f;
        const spline::SplineProfile profile = spline::buildProfile(profileParams);
        // the ring the sweep emits: OpenEngine's 5 Crown points, its 2 shoulder creases twice
        size_t ringVertices = 0;
        for (const spline::SplineProfilePoint& point : profile.points){
            ringVertices += point.hard ? 2u : 1u;
        }
        results.add("crown_profile_points", static_cast<double>(profile.points.size()), "points");
        results.add("crown_ring_vertices", static_cast<double>(ringVertices), "vertices");
        std::vector<spline::SplineStation> stations;
        spline::SplineStripMesh mesh;
        size_t vertices = 0;
        const double roadMs = medianMs(15, [&](){
            spline::SplineStationOptions options;
            options.stationSpacing = 1.0f;
            spline::buildStations(road, options, stations);
            spline::SplineStripParams params;
            params.uOriginMetres = 0.0f;
            params.tilesPerMetreU = 0.125f;
            const std::vector<spline::SplineStripChunkRange> chunks = spline::planSplineStripChunks(profile, stations, params);
            vertices = 0;
            for (const spline::SplineStripChunkRange& chunk : chunks){
                spline::buildSplineStripChunk(profile, stations, chunk, params, mesh);
                vertices += mesh.vertices.size();
            }
        });
        results.add("extrude_crown_1km_ms", roadMs, "ms");
        results.add("extrude_crown_1km_stations", static_cast<double>(stations.size()), "stations");
        results.add("extrude_crown_1km_vertices", static_cast<double>(vertices), "vertices");
    }

    // ---- R19: 2,000 pieces placed (Spacing, 2 m over 4 km)
    {
        const spline::SplineData row = meander(41, 4000.0f, 1.0f);
        std::vector<spline::SplineStation> stations;
        spline::SplineStationOptions options;
        spline::buildStations(row, options, stations);
        std::vector<spline::SplineCenterSample> center;
        const spline::SplinePiece piece = spline::makeSplinePiece(0, AABB(Vector3(-0.3f, 0.0f, -0.3f), Vector3(0.3f, 1.0f, 0.3f)));
        std::vector<spline::SplinePiecePose> poses;
        const double placeMs = medianMs(15, [&](){
            spline::centerlineFromStations(stations, center);
            spline::SplineTileLayoutParams params;
            params.mode = spline::SplineTileMode::Spacing;
            params.pieces = std::span<const spline::SplinePiece>(&piece, 1);
            params.spacing = 2.0f;
            params.maxPieces = 2000;
            params.yawJitterDegrees = 15.0f;
            params.dropoutChance = 0.0f;
            spline::buildTileLayout(center, params, poses);
        });
        results.add("instantiate_2000_ms", placeMs, "ms");
        results.add("instantiate_2000_pieces", static_cast<double>(poses.size()), "pieces");
    }

    // ---- R19: FitToBanks, a 300 m river on a 1025 x 1025 ground grid (1 m)
    {
        constexpr uint32_t n = 1025;
        std::vector<float> heights(static_cast<size_t>(n) * n);
        const spline::SplineData river = meander(7, 300.0f, 6.0f);
        // a valley 3 m deep along the river's course, climbing to 6 m away from it
        for (uint32_t z = 0; z < n; z++){
            for (uint32_t x = 0; x < n; x++){
                const float wx = static_cast<float>(x) - 362.0f;
                const float wz = static_cast<float>(z) - 512.0f;
                const float course = 12.0f * std::sin(wx * 0.02f);
                const float d = std::fabs(wz - course);
                heights[static_cast<size_t>(z) * n + x] = std::min(6.0f, -2.0f + 0.04f * d * d);
            }
        }
        spline::SplineGroundGrid ground;
        ground.originX = -362.0f;
        ground.originZ = -512.0f;
        ground.spacingX = 1.0f;
        ground.spacingZ = 1.0f;
        ground.countX = n;
        ground.countZ = n;
        ground.heights = heights;
        std::vector<spline::SplineStation> stations;
        spline::SplineStationOptions options;
        spline::buildStations(river, options, stations);
        size_t vertices = 0;
        const double fillMs = medianMs(9, [&](){
            spline::SplineFillParams params;
            params.maxHalfWidth = 12.0f;
            const spline::SplineFillResult field = spline::buildSplineFillField(stations, ground, params);
            vertices = 0;
            spline::SplineFillMeshParams meshParams;
            for (const spline::SplineFillChunkRange& range : spline::carveFillChunks(field)){
                vertices += spline::buildSplineFillMesh(field, ground, stations, range, meshParams).vertices.size();
            }
        });
        results.add("fit_to_banks_300m_1025_ms", fillMs, "ms");
        results.add("fit_to_banks_300m_1025_vertices", static_cast<double>(vertices), "vertices");
    }

    // ---- R21: a scene without spline components: SplineSystem per frame, and its allocations
    {
        Scene scene;
        auto splines = scene.getSystem<SplineSystem>();
        for (int i = 0; i < 100; i++) splines->update(1.0 / 60.0);
        const int frames = 10000;
        const auto start = bench::Clock::now();
        for (int i = 0; i < frames; i++) splines->update(1.0 / 60.0);
        results.add("empty_scene_frame_us", bench::secondsSince(start) * 1.0e6 / frames, "us");
        if (countAllocs){
            testsupport::AllocationCountScope scope;
            for (int i = 0; i < frames; i++) splines->update(1.0 / 60.0);
            results.add("empty_scene_allocations", static_cast<double>(scope.count()), "allocations");
        }
    }

    // ---- R21: Spline Animate per follower per frame (1,000 followers on one spline)
    {
        auto frameUs = [](int followers){
            Scene scene;
            const Entity spline = scene.createEntity();
            scene.addComponent<Transform>(spline, {});
            SplineComponent component;
            component.spline = meander(20, 200.0f, 1.0f);
            scene.addComponent<SplineComponent>(spline, component);
            for (int f = 0; f < followers; f++){
                const Entity entity = scene.createEntity();
                scene.addComponent<Transform>(entity, {});
                SplineAnimateComponent animate;
                animate.spline = spline;
                animate.duration = 10.0f;
                animate.startOffset = static_cast<float>(f) / static_cast<float>(followers + 1);
                scene.addComponent<SplineAnimateComponent>(entity, animate);
            }
            auto splines = scene.getSystem<SplineSystem>();
            for (int i = 0; i < 10; i++) splines->update(1.0 / 60.0);
            std::vector<double> samples;
            for (int i = 0; i < 200; i++){
                const auto start = bench::Clock::now();
                splines->update(1.0 / 60.0);
                samples.push_back(bench::secondsSince(start) * 1.0e6);
            }
            return bench::percentile(samples, 0.5);
        };
        const double base = frameUs(0);
        const double many = frameUs(1000);
        results.add("spline_animate_follower_us", std::max(0.0, many - base) / 1000.0, "us");
    }

    const char* path = bench::jsonPath(argc, argv, "je-bench-splines.json");
    if (!results.write(path)){
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::printf("wrote %s\n", path);
    return 0;
}
