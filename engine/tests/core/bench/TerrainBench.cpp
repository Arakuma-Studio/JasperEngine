// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The terrain engine's budgets (docs/specs/T1-terrain-authoring.md R47, R48, R50-R52), docs/perf/terrain.md:
//   je-bench-terrain [--json <file>]
// check_bench.py gates the file with tools/perf/bench-thresholds.json. Times are this machine's, in the
// build's optimization; R49's delta step is the editor's and has its own suite (TerrainDeltaBench).
// R50's "no more than 5 % over today's cost" compares with a copy of the code the surface query
// replaced (namespace today below, from MeshSystem.cpp before 40d74d630), timed in the same run.

#include "BenchUtil.h"
#include "JobsTestSystem.h"
#include "Engine.h"
#include "Scene.h"
#include "component/CameraComponent.h"
#include "component/MeshComponent.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "jobs/JobSystem.h"
#include "math/Vector2.h"
#include "math/Vector3.h"
#include "object/TerrainEdit.h"
#include "subsystem/MeshSystem.h"
#include "subsystem/PhysicsSystem.h"
#include "terrain/OverlayMapStore.h"
#include "terrain/RawMapStore.h"
#include "terrain/TerrainOps.h"
#include "terrain/TerrainSurface.h"
#include "texture/TextureData.h"

#ifdef JASPERENGINE_PHYSICS_3D
#include "physics/JoltShapeBuilder.h"
#include "physics/JoltTerrainRefresh.h"
#include "Jolt/Physics/Collision/Shape/HeightFieldShape.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    constexpr MapId HEIGHT{MapKind::Height, 0};
    constexpr float MAX_HEIGHT = 100.0f;

    // Rolling hills in 16 bits: every op and query has slopes and both directions to work with
    std::vector<uint8_t> hills(int size) {
        std::vector<uint8_t> bytes(static_cast<size_t>(size) * size * 2);
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                const double h = 0.5 + 0.2 * std::sin(x * 0.021) * std::cos(y * 0.017) + 0.05 * std::sin((x + 2 * y) * 0.093);
                const unsigned value = static_cast<unsigned>(std::lround(std::clamp(h, 0.0, 1.0) * 65535.0));
                const size_t index = (static_cast<size_t>(y) * size + x) * 2;
                bytes[index] = static_cast<uint8_t>(value & 0xFF);
                bytes[index + 1] = static_cast<uint8_t>(value >> 8);
            }
        }
        return bytes;
    }

    // A map the size of the terrain in units: one texel per unit, so radii in texels read as units
    std::unique_ptr<RawMapStore> hillStore(int size) {
        auto store = std::make_unique<RawMapStore>(static_cast<float>(size), MAX_HEIGHT);
        store->addMap(HEIGHT, size, size, 1, 16, hills(size));
        return store;
    }

    double msOf(bench::Clock::time_point start) { return bench::secondsSince(start) * 1.0e3; }

    // ---------------------------------------------------------------- R47: one stamp of a stroke

    // A brush stamp as TerrainStrokeSession runs it: in place on the stroke's overlay, a click's deposit
    Op stampOp(OpType type, float radius) {
        Op op;
        op.type = type;
        op.shape.type = ShapeType::Circle;
        op.shape.radius = radius;
        op.shape.nearestTexel = true;
        if (type == OpType::Raise) op.delta = MAX_HEIGHT;
        op.strength = 0.3f * 1.5f / 60.0f;
        return op;
    }

    // The median stamp of a stroke at R33's spacing (radius / 4), rows of it two radii apart: what a drag
    // costs per stamp, tile copies on first touch included
    double stampMs(int mapSize, Op op, int stamps) {
        auto store = hillStore(mapSize);
        OverlayMapStore stroke(*store);
        OpOptions options;
        options.inPlace = true;
        const float radius = op.shape.radius;
        const float spacing = radius * 0.25f;
        const float usable = static_cast<float>(mapSize) * 0.5f - radius;
        const int perRow = std::max(1, static_cast<int>(2.0f * usable / spacing));
        std::vector<double> samples;
        for (int i = 0; i < stamps; i++) {
            op.shape.center = Vector2(-usable + spacing * static_cast<float>(i % perRow),
                                      std::min(usable, -usable + 2.0f * radius * static_cast<float>(i / perRow)));
            const auto begin = bench::Clock::now();
            const TerrainOpResult result = applyOps(stroke, std::span<const Op>(&op, 1), options);
            samples.push_back(msOf(begin));
            if (!result.ok) {
                std::fprintf(stderr, "stamp failed: %s\n", result.error.c_str());
                return -1.0;
            }
        }
        return bench::percentile(samples, 0.5);
    }

    // ---------------------------------------------------------------- R48: whole-map ops and a commit

    double setHeightWholeMs(int size) {
        auto store = hillStore(size);
        std::vector<double> samples;
        for (int run = 0; run < 3; run++) {
            Op op;
            op.type = OpType::SetHeight;
            op.height = MAX_HEIGHT * (run % 2 == 0 ? 0.25f : 0.75f);
            const auto begin = bench::Clock::now();
            const TerrainOpResult result = applyOps(*store, std::span<const Op>(&op, 1));
            samples.push_back(msOf(begin));
            if (!result.ok) return -1.0;
        }
        return bench::percentile(samples, 0.5);
    }

    double paintByRulesMs(int size) {
        auto store = hillStore(size);
        std::vector<double> samples;
        // the first run makes the blend map; the timed ones repaint it
        for (int run = 0; run < 4; run++) {
            Op op;
            op.type = OpType::PaintByRules;
            PaintRule ground;
            ground.layer = 1;
            PaintRule cliffs;
            cliffs.layer = 2;
            cliffs.slope = FilterRange{25.0f, 90.0f};
            PaintRule peaks;
            peaks.layer = 3;
            peaks.height = FilterRange{MAX_HEIGHT * 0.6f, MAX_HEIGHT};
            peaks.noise = 0.3f;
            peaks.opacity = run % 2 == 0 ? 1.0f : 0.8f;
            op.rules = {ground, cliffs, peaks};
            const auto begin = bench::Clock::now();
            const TerrainOpResult result = applyOps(*store, std::span<const Op>(&op, 1));
            if (run > 0) samples.push_back(msOf(begin));
            if (!result.ok) {
                std::fprintf(stderr, "paint_by_rules failed: %s\n", result.error.c_str());
                return -1.0;
            }
        }
        return bench::percentile(samples, 0.5);
    }

    // An overlay holding 64 changed tiles writes them to its base
    double overlayCommit64Ms() {
        auto store = hillStore(1024);
        std::vector<double> samples;
        constexpr int SIDE = 8 * OverlayMapStore::TILE_SIZE;
        std::vector<float> values(static_cast<size_t>(SIDE) * SIDE);
        for (int run = 0; run < 20; run++) {
            OverlayMapStore overlay(*store);
            std::fill(values.begin(), values.end(), run % 2 == 0 ? 0.3f : 0.6f);
            overlay.write(HEIGHT, TexelRect{128, 128, SIDE, SIDE}, values);
            if (overlay.tileCount() != 64) return -1.0;
            const auto begin = bench::Clock::now();
            if (!overlay.commit().ok) return -1.0;
            samples.push_back(msOf(begin));
        }
        return bench::percentile(samples, 0.5);
    }

    // ---------------------------------------------------------------- R50: the surface query

    // The code the one surface query replaced (MeshSystem::sampleTerrainHeightTexels,
    // sampleTerrainHeight, sampleTerrainSurface and raycastTerrainSurface before T1), over plain pixels
    namespace today {

        float texelCoord(float local, float terrainSize, int size) {
            return ((local + terrainSize * 0.5f) / terrainSize) * static_cast<float>(size) - 0.5f;
        }

        float sampleTexels(const unsigned char* pixels, int width, int height, int channels, int bytesPerChannel, float texelX, float texelY) {
            if (!pixels || width <= 0 || height <= 0 || channels <= 0) return 0.0f;
            texelX = std::clamp(texelX, 0.0f, static_cast<float>(width - 1));
            texelY = std::clamp(texelY, 0.0f, static_cast<float>(height - 1));
            const int lowerX = std::clamp(static_cast<int>(std::floor(texelX)), 0, width - 1);
            const int lowerY = std::clamp(static_cast<int>(std::floor(texelY)), 0, height - 1);
            const int upperX = std::min(lowerX + 1, width - 1);
            const int upperY = std::min(lowerY + 1, height - 1);
            const float blendX = texelX - static_cast<float>(lowerX);
            const float blendY = texelY - static_cast<float>(lowerY);
            auto texel = [&](int x, int y) {
                const size_t texelIndex = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
                const size_t index = texelIndex * static_cast<size_t>(channels) * static_cast<size_t>(bytesPerChannel);
                if (bytesPerChannel >= 2) {
                    const unsigned int value = static_cast<unsigned int>(pixels[index]) | (static_cast<unsigned int>(pixels[index + 1]) << 8);
                    return static_cast<float>(value) / 65535.0f;
                }
                return pixels[index] / 255.0f;
            };
            const float lower = texel(lowerX, lowerY) + (texel(upperX, lowerY) - texel(lowerX, lowerY)) * blendX;
            const float upper = texel(lowerX, upperY) + (texel(upperX, upperY) - texel(lowerX, upperY)) * blendX;
            return lower + (upper - lower) * blendY;
        }

        float sampleHeight(const SurfaceView& view, float localX, float localZ) {
            if (view.size <= std::numeric_limits<float>::epsilon()) return 0.0f;
            return view.maxHeight * sampleTexels(view.pixels, view.width, view.height, view.channels, view.bytesPerChannel,
                                                 texelCoord(localX, view.size, view.width), texelCoord(localZ, view.size, view.height));
        }

        void sampleSurface(const SurfaceView& view, float localX, float localZ, float& height, Vector3& normal) {
            const float delta = view.size / static_cast<float>(view.width);
            height = sampleHeight(view, localX, localZ);
            normal = Vector3(height - sampleHeight(view, localX + delta, localZ), delta, height - sampleHeight(view, localX, localZ + delta)).normalized();
        }

        // The 160-step march with 12 bisections over origin + direction * t, t in [0, 1]
        bool raycast(const SurfaceView& view, const Vector3& origin, const Vector3& direction, Vector3& hit) {
            const float halfSize = view.size * 0.5f;
            auto heightAt = [&](float x, float z, float& out) {
                if (x < -halfSize || x > halfSize || z < -halfSize || z > halfSize) return false;
                out = sampleHeight(view, x, z);
                return true;
            };
            const float surfaceMinHeight = std::min(0.0f, view.maxHeight);
            const float surfaceMaxHeight = std::max(0.0f, view.maxHeight);
            float rayEntry = 0.0f;
            float rayExit = 1.0f;
            auto clipAxis = [&](float axisOrigin, float axisDirection, float minValue, float maxValue) {
                if (std::abs(axisDirection) <= std::numeric_limits<float>::epsilon()) return axisOrigin >= minValue && axisOrigin <= maxValue;
                float nearParameter = (minValue - axisOrigin) / axisDirection;
                float farParameter = (maxValue - axisOrigin) / axisDirection;
                if (nearParameter > farParameter) std::swap(nearParameter, farParameter);
                rayEntry = std::max(rayEntry, nearParameter);
                rayExit = std::min(rayExit, farParameter);
                return rayEntry <= rayExit;
            };
            if (!clipAxis(origin.x, direction.x, -halfSize, halfSize) || !clipAxis(origin.y, direction.y, surfaceMinHeight, surfaceMaxHeight) ||
                !clipAxis(origin.z, direction.z, -halfSize, halfSize)) {
                return false;
            }
            auto signedDistance = [&](float t, float& distance, Vector3& point) {
                point = origin + direction * t;
                float sampled = 0.0f;
                if (!heightAt(point.x, point.z, sampled)) return false;
                distance = point.y - sampled;
                return true;
            };
            const int raycastSteps = 160;
            const float surfaceEpsilon = std::max(0.001f, std::abs(surfaceMaxHeight - surfaceMinHeight) * 0.0005f);
            float previousParameter = rayEntry;
            float previousDistance = 0.0f;
            Vector3 previousPoint;
            if (!signedDistance(previousParameter, previousDistance, previousPoint)) return false;
            float closestDistance = previousDistance;
            Vector3 closestPoint = previousPoint;
            if (std::abs(previousDistance) <= surfaceEpsilon) closestDistance = 0.0f;
            for (int sampleIndex = 1; sampleIndex <= raycastSteps; sampleIndex++) {
                const float t = rayEntry + (rayExit - rayEntry) * (static_cast<float>(sampleIndex) / static_cast<float>(raycastSteps));
                float currentDistance = 0.0f;
                Vector3 currentPoint;
                if (!signedDistance(t, currentDistance, currentPoint)) continue;
                if (std::abs(currentDistance) < std::abs(closestDistance)) {
                    closestDistance = currentDistance;
                    closestPoint = currentPoint;
                }
                const bool crossed = (previousDistance <= 0.0f && currentDistance >= 0.0f) || (previousDistance >= 0.0f && currentDistance <= 0.0f);
                if (crossed) {
                    float lower = previousParameter;
                    float upper = t;
                    float lowerDistance = previousDistance;
                    for (int refine = 0; refine < 12; refine++) {
                        const float mid = (lower + upper) * 0.5f;
                        float midDistance = 0.0f;
                        Vector3 midPoint;
                        if (!signedDistance(mid, midDistance, midPoint)) break;
                        const bool sameSide = (lowerDistance <= 0.0f && midDistance <= 0.0f) || (lowerDistance >= 0.0f && midDistance >= 0.0f);
                        if (sameSide) {
                            lower = mid;
                            lowerDistance = midDistance;
                        } else {
                            upper = mid;
                        }
                    }
                    closestPoint = origin + direction * ((lower + upper) * 0.5f);
                    closestDistance = 0.0f;
                    break;
                }
                previousParameter = t;
                previousDistance = currentDistance;
            }
            float localHeight = 0.0f;
            if (std::abs(closestDistance) > surfaceEpsilon * 4.0f || !heightAt(closestPoint.x, closestPoint.z, localHeight)) return false;
            hit = Vector3(closestPoint.x, localHeight, closestPoint.z);
            return true;
        }

    }

    // Points spread over the terrain, the same for both sides of a comparison
    std::vector<Vector2> queryPoints(float size, int count) {
        std::vector<Vector2> points(static_cast<size_t>(count));
        uint32_t state = 12345u;
        auto next = [&]() {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 8) / static_cast<float>(1u << 24);
        };
        for (Vector2& point : points) point = Vector2((next() - 0.5f) * size, (next() - 0.5f) * size);
        return points;
    }

    // Best of rounds, the two sides interleaved, so a shared machine's load lands on both
    template<typename A, typename B>
    std::pair<double, double> bestOfInterleaved(int rounds, A&& a, B&& b) {
        double bestA = 1e30, bestB = 1e30;
        for (int round = 0; round < rounds; round++) {
            bestA = std::min(bestA, a());
            bestB = std::min(bestB, b());
        }
        return {bestA, bestB};
    }

    volatile float sink = 0.0f;

    // ---------------------------------------------------------------- R51, R52: a terrain in a scene

    void giveHeightMap(Texture& texture, const std::string& id, int size) {
        const std::vector<uint8_t> bytes = hills(size);
        unsigned char* raw = static_cast<unsigned char*>(std::malloc(bytes.size()));
        std::memcpy(raw, bytes.data(), bytes.size());
        TextureData data(size, size, static_cast<unsigned int>(bytes.size()), ColorFormat::RED16, 1, raw);
        data.setDataOwned(false);
        texture.setData(id, data);
        texture.getData().setDataOwned(true);
    }

    // A built node tree: 2 roots, 6 levels, over a size² map
    struct SceneTerrain {
        Scene scene;
        Entity entity = NULL_ENTITY;

        SceneTerrain(const std::string& id, int size) {
            const Entity camera = scene.createEntity();
            scene.addComponent<Transform>(camera, {});
            scene.addComponent<CameraComponent>(camera, {});
            scene.setCamera(camera);
            entity = scene.createEntity();
            scene.addComponent<Transform>(entity, {});
            TerrainComponent component;
            component.rootGridSize = 2;
            component.levels = 6;
            component.terrainSize = static_cast<float>(size);
            component.maxHeight = MAX_HEIGHT;
            scene.addComponent<TerrainComponent>(entity, component);
            scene.addComponent<MeshComponent>(entity, {});
            giveHeightMap(terrain().heightMap, id, size);
            system().update(0.016);
        }

        TerrainComponent& terrain() { return scene.getComponent<TerrainComponent>(entity); }
        MeshSystem& system() { return *scene.getSystem<MeshSystem>(); }
    };

}

int main(int argc, char** argv) {
    Engine::systemInit(argc, argv, new testsupport::JobsTestSystem());
    bench::Results results("je-bench-terrain");
    results.add("workers", static_cast<double>(jobs::hasSystem() ? jobs::system().getWorkerCount() : 0), "threads");

    // R47
    results.add("stamp_raise_r64_ms", stampMs(1024, stampOp(OpType::Raise, 64.0f), 200), "ms");
    results.add("stamp_smooth_r64_ms", stampMs(1024, stampOp(OpType::Smooth, 64.0f), 200), "ms");
    {
        Op filtered = stampOp(OpType::Smooth, 64.0f);
        filtered.filter.slope = FilterRange{0.0f, 20.0f};
        results.add("stamp_smooth_slope_filter_r64_ms", stampMs(1024, filtered, 200), "ms");
    }
    results.add("stamp_raise_r512_ms", stampMs(2048, stampOp(OpType::Raise, 512.0f), 12), "ms");

    // R48
    results.add("set_height_4096_ms", setHeightWholeMs(4096), "ms");
    results.add("paint_by_rules_3_2048_ms", paintByRulesMs(2048), "ms");
    results.add("overlay_commit_64_tiles_ms", overlayCommit64Ms(), "ms");

    // R50: 1024² over 1024 units, as R50's raycast states it
    {
        constexpr int SIZE = 1024;
        const std::vector<uint8_t> pixels = hills(SIZE);
        SurfaceView view;
        view.pixels = pixels.data();
        view.width = view.height = SIZE;
        view.channels = 1;
        view.bytesPerChannel = 2;
        view.size = static_cast<float>(SIZE);
        view.maxHeight = MAX_HEIGHT;
        constexpr int POINTS = 1 << 20;
        const std::vector<Vector2> points = queryPoints(view.size, POINTS);

        const auto [heightNs, todayHeightNs] = bestOfInterleaved(5,
            [&]() {
                float sum = 0.0f;
                const auto begin = bench::Clock::now();
                for (const Vector2& p : points) sum += sampleHeight(view, p.x, p.y);
                const double ns = bench::secondsSince(begin) * 1.0e9 / POINTS;
                sink = sink + sum;
                return ns;
            },
            [&]() {
                float sum = 0.0f;
                const auto begin = bench::Clock::now();
                for (const Vector2& p : points) sum += today::sampleHeight(view, p.x, p.y);
                const double ns = bench::secondsSince(begin) * 1.0e9 / POINTS;
                sink = sink + sum;
                return ns;
            });
        results.add("sample_height_ns", heightNs, "ns");
        results.add("sample_height_today_ns", todayHeightNs, "ns");
        results.add("sample_height_vs_today", heightNs / todayHeightNs, "x");

        const auto [surfaceNs, todaySurfaceNs] = bestOfInterleaved(5,
            [&]() {
                float sum = 0.0f;
                const auto begin = bench::Clock::now();
                for (const Vector2& p : points) {
                    const SurfaceSample s = sampleSurface(view, p.x, p.y);
                    sum += s.height + s.normal.x;
                }
                const double ns = bench::secondsSince(begin) * 1.0e9 / POINTS;
                sink = sink + sum;
                return ns;
            },
            [&]() {
                float sum = 0.0f;
                const auto begin = bench::Clock::now();
                for (const Vector2& p : points) {
                    float height = 0.0f;
                    Vector3 normal;
                    today::sampleSurface(view, p.x, p.y, height, normal);
                    sum += height + normal.x;
                }
                const double ns = bench::secondsSince(begin) * 1.0e9 / POINTS;
                sink = sink + sum;
                return ns;
            });
        results.add("sample_height_normal_ns", surfaceNs, "ns");
        results.add("sample_height_normal_today_ns", todaySurfaceNs, "ns");
        results.add("sample_height_normal_vs_today", surfaceNs / todaySurfaceNs, "x");

        // Rays 512 units long, from above the hills down at a shallow angle across the terrain
        constexpr int RAYS = 4096;
        constexpr float LENGTH = 512.0f;
        const std::vector<Vector2> starts = queryPoints(view.size * 0.5f, RAYS);
        std::vector<Vector3> origins;
        std::vector<Vector3> directions;
        for (int i = 0; i < RAYS; i++) {
            const float angle = static_cast<float>(i) * 2.399963f;
            const Vector3 direction = Vector3(std::cos(angle), -0.12f, std::sin(angle)).normalized();
            origins.push_back(Vector3(starts[static_cast<size_t>(i)].x, MAX_HEIGHT * 0.95f, starts[static_cast<size_t>(i)].y));
            directions.push_back(direction);
        }
        int hitsNew = 0, hitsToday = 0;
        const auto [rayUs, todayRayUs] = bestOfInterleaved(5,
            [&]() {
                int hits = 0;
                const auto begin = bench::Clock::now();
                for (int i = 0; i < RAYS; i++) {
                    Vector3 point;
                    float t = 0.0f;
                    if (raycastSurface(view, origins[static_cast<size_t>(i)], directions[static_cast<size_t>(i)], LENGTH, false, point, t)) hits++;
                }
                hitsNew = hits;
                return bench::secondsSince(begin) * 1.0e6 / RAYS;
            },
            [&]() {
                int hits = 0;
                const auto begin = bench::Clock::now();
                for (int i = 0; i < RAYS; i++) {
                    Vector3 point;
                    if (today::raycast(view, origins[static_cast<size_t>(i)], directions[static_cast<size_t>(i)] * LENGTH, point)) hits++;
                }
                hitsToday = hits;
                return bench::secondsSince(begin) * 1.0e6 / RAYS;
            });
        results.add("raycast_512m_us", rayUs, "us");
        results.add("raycast_512m_today_us", todayRayUs, "us");
        results.add("raycast_512m_vs_today", rayUs / todayRayUs, "x");
        results.add("raycast_512m_hits", static_cast<double>(hitsNew), "rays");
        results.add("raycast_512m_today_hits", static_cast<double>(hitsToday), "rays");
    }

    // R51, R52 on a terrain in a scene
    {
        constexpr int SIZE = 1024;
        SceneTerrain built("je-bench-terrain-scene", SIZE);
        TerrainComponent& terrain = built.terrain();
        if (terrain.numNodes == 0) {
            std::fprintf(stderr, "the terrain built no nodes\n");
            return 1;
        }
        std::vector<double> samples;
        for (int i = 0; i < 200; i++) {
            const int x = (i * 61) % (SIZE - 64);
            const int y = (i * 37) % (SIZE - 64);
            const auto begin = bench::Clock::now();
            built.system().refitTerrainNodes(terrain, x, y, 64, 64);
            samples.push_back(msOf(begin));
        }
        results.add("node_refit_64_ms", bench::percentile(samples, 0.5), "ms");

        samples.clear();
        for (int i = 0; i < 5; i++) {
            terrain.needUpdateTerrain = true;
            const auto begin = bench::Clock::now();
            built.system().update(0.016);
            samples.push_back(msOf(begin));
        }
        results.add("node_full_rebuild_ms", bench::percentile(samples, 0.5), "ms");

        // R52: a game's writes on the main thread, the frame's flush between them untimed
        TerrainComponentStore store(&built.scene, built.entity, terrain);
        std::vector<float> heights(64 * 64);
        samples.clear();
        for (int i = 0; i < 100; i++) {
            std::fill(heights.begin(), heights.end(), MAX_HEIGHT * (i % 2 == 0 ? 0.4f : 0.45f));
            const TexelRect rect{(i * 97) % (SIZE - 64), (i * 53) % (SIZE - 64), 64, 64};
            const auto begin = bench::Clock::now();
            const TerrainOpResult result = setHeights(store, rect, heights);
            samples.push_back(msOf(begin));
            if (!result.ok) {
                std::fprintf(stderr, "setHeights failed: %s\n", result.error.c_str());
                return 1;
            }
            built.system().update(0.016);
        }
        results.add("runtime_set_heights_64_ms", bench::percentile(samples, 0.5), "ms");

        samples.clear();
        for (int i = 0; i < 100; i++) {
            Op op;
            op.type = i % 2 == 0 ? OpType::Raise : OpType::Lower;
            op.delta = 0.5f;
            op.shape.type = ShapeType::Circle;
            op.shape.radius = 32.0f;
            op.shape.center = Vector2(static_cast<float>((i * 29) % 800) - 400.0f, static_cast<float>((i * 43) % 800) - 400.0f);
            const auto begin = bench::Clock::now();
            const TerrainOpResult result = applyOps(store, std::span<const Op>(&op, 1));
            samples.push_back(msOf(begin));
            if (!result.ok) return 1;
            built.system().update(0.016);
        }
        results.add("runtime_apply_circle_r32_ms", bench::percentile(samples, 0.5), "ms");
    }

#ifdef JASPERENGINE_PHYSICS_3D
    // R52: SetHeights of a 64 x 64 sample block of a 1025-sample heightfield
    {
        constexpr int SAMPLES = 1025;
        Scene scene;
        // the scene's physics world is what sets Jolt up
        scene.getSystem<PhysicsSystem>();
        const Entity entity = scene.createEntity();
        scene.addComponent<Transform>(entity, {});
        TerrainComponent component;
        component.terrainSize = static_cast<float>(SAMPLES - 1);
        component.maxHeight = MAX_HEIGHT;
        component.allowRuntimeEdits = true;
        component.needUpdateTerrain = false;
        component.heightMapLoaded = true;
        scene.addComponent<TerrainComponent>(entity, component);
        TerrainComponent& terrain = scene.getComponent<TerrainComponent>(entity);
        giveHeightMap(terrain.heightMap, "je-bench-terrain-collision", SAMPLES);
        std::string error;
        JPH::ShapeRefC shape = JoltShapeBuilder::buildHeightFieldShape(&scene, terrain, Vector3(1, 1, 1), SAMPLES, &error);
        if (!shape) {
            std::fprintf(stderr, "no heightfield: %s\n", error.c_str());
            return 1;
        }
        auto& field = const_cast<JPH::HeightFieldShape&>(static_cast<const JPH::HeightFieldShape&>(*shape));
        const HeightFieldBlock block{448, 448, 64, 64};
        TerrainComponentStore store(&scene, entity, terrain);
        std::vector<double> samples;
        int rebuilt = 0;
        for (int i = 0; i < 50; i++) {
            // an edit inside the block, small enough to stay in the shape's height range
            Op op;
            op.type = i % 2 == 0 ? OpType::Raise : OpType::Lower;
            op.delta = 0.05f;
            op.shape.type = ShapeType::Circle;
            op.shape.radius = 20.0f;
            op.shape.center = Vector2(-32.0f, -32.0f);
            if (!applyOps(store, std::span<const Op>(&op, 1)).ok) return 1;
            const auto begin = bench::Clock::now();
            if (!JoltTerrainRefresh::refreshShape(&scene, terrain, Vector3(1, 1, 1), SAMPLES, field, block,
                                                  JoltShapeBuilder::sharedPhysicsTempAllocator())) {
                rebuilt++;
            }
            samples.push_back(msOf(begin));
        }
        results.add("collision_refresh_64_ms", bench::percentile(samples, 0.5), "ms");
        results.add("collision_refresh_64_rebuilds", static_cast<double>(rebuilt), "of 50");
    }
#endif

    const char* path = bench::jsonPath(argc, argv, "je-bench-terrain.json");
    if (!results.write(path)) {
        std::fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    std::printf("wrote %s\n", path);
    return 0;
}
