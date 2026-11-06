// SPDX-License-Identifier: MIT

// physics/ConvexDecomposition: V-HACD hulls of convex and concave meshes (each in the mesh's
// space, the concavities left out), refusal of flat and broken meshes, the content hash (UV-seam
// copies weld, params count), the .hulls disk cache (shared by instances, corrupt files dropped)
// and the background jobs (polling, cancel of a running job, retry, many threads at once).

#include <gtest/gtest.h>

#include "physics/ConvexDecomposition.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(JASPERENGINE_SANITIZE_ADDRESS)
    // the ASan runtime's (sanitizer/allocator_interface.h, not installed with every compiler)
    extern "C" size_t __sanitizer_get_current_allocated_bytes();
#elif defined(__GLIBC__)
    #include <malloc.h>
#endif

using namespace jasperengine;
namespace fs = std::filesystem;

namespace {

    using Clock = std::chrono::steady_clock;

    struct Mesh {
        std::vector<Vector3> positions;
        std::vector<uint32_t> indices;
    };

    Mesh box(const Vector3& lo, const Vector3& hi) {
        Mesh mesh;
        for (int i = 0; i < 8; i++)
            mesh.positions.emplace_back(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z);
        // two outward triangles per face
        const uint32_t faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
        for (const auto& f : faces)
            mesh.indices.insert(mesh.indices.end(), {f[0], f[1], f[2], f[0], f[2], f[3]});
        return mesh;
    }

    Mesh cube() {
        return box(Vector3(-0.5f, -0.5f, -0.5f), Vector3(0.5f, 0.5f, 0.5f));
    }

    // A polygon (counter-clockwise in xz, star-shaped from its first corner) extruded along y
    Mesh extrude(const std::vector<std::array<float, 2>>& polygon, float height) {
        Mesh mesh;
        const uint32_t n = static_cast<uint32_t>(polygon.size());
        for (float y : {0.0f, height})
            for (const auto& p : polygon)
                mesh.positions.emplace_back(p[0], y, p[1]);
        for (uint32_t i = 1; i + 1 < n; i++) {
            mesh.indices.insert(mesh.indices.end(), {0, i + 1, i});              // bottom, facing down
            mesh.indices.insert(mesh.indices.end(), {n, n + i, n + i + 1});      // top, facing up
        }
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t j = (i + 1) % n;
            mesh.indices.insert(mesh.indices.end(), {i, j, n + j, i, n + j, n + i});
        }
        return mesh;
    }

    // Two 2 x 1 legs meeting at the origin corner; the square [1,2] x [1,2] is the concavity
    Mesh lShape() {
        return extrude({{0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}, {2, 0}}, 1.0f);
    }

    Mesh torus(float major, float minor, uint32_t around, uint32_t tube) {
        Mesh mesh;
        const float tau = 6.28318530718f;
        for (uint32_t i = 0; i < around; i++) {
            const float u = tau * float(i) / float(around);
            for (uint32_t j = 0; j < tube; j++) {
                const float v = tau * float(j) / float(tube);
                const float r = major + minor * std::cos(v);
                mesh.positions.emplace_back(r * std::cos(u), minor * std::sin(v), r * std::sin(u));
            }
        }
        for (uint32_t i = 0; i < around; i++) {
            for (uint32_t j = 0; j < tube; j++) {
                const uint32_t a = i * tube + j;
                const uint32_t b = ((i + 1) % around) * tube + j;
                const uint32_t c = ((i + 1) % around) * tube + (j + 1) % tube;
                const uint32_t d = i * tube + (j + 1) % tube;
                mesh.indices.insert(mesh.indices.end(), {a, d, c, a, c, b});
            }
        }
        return mesh;
    }

    // A grid of quads on the plane through the origin spanned by the two axes
    Mesh plane(const Vector3& axisU, const Vector3& axisV, uint32_t cells) {
        Mesh mesh;
        for (uint32_t i = 0; i <= cells; i++)
            for (uint32_t j = 0; j <= cells; j++)
                mesh.positions.push_back(axisU * float(i) + axisV * float(j));
        for (uint32_t i = 0; i < cells; i++) {
            for (uint32_t j = 0; j < cells; j++) {
                const uint32_t a = i * (cells + 1) + j;
                mesh.indices.insert(mesh.indices.end(), {a, a + 1, a + cells + 2, a, a + cells + 2, a + cells + 1});
            }
        }
        return mesh;
    }

    // The planes of a convex hull's faces, from its points: every triple whose plane has all
    // the points on one side. Brute force, fine for the 64 points of a hull.
    struct HullPlanes {
        std::vector<std::array<double, 4>> planes;   // unit normal, offset; outside where n.p > d
    };

    HullPlanes hullPlanes(const ConvexHullPoints& hull) {
        HullPlanes result;
        const std::vector<Vector3>& p = hull.points;
        double size = 0.0;
        for (const Vector3& a : p)
            for (const Vector3& b : p)
                size = std::max(size, double((a - b).length()));
        const double eps = size * 1e-5;
        for (size_t i = 0; i < p.size(); i++) {
            for (size_t j = i + 1; j < p.size(); j++) {
                for (size_t k = j + 1; k < p.size(); k++) {
                    const double ux = p[j].x - p[i].x, uy = p[j].y - p[i].y, uz = p[j].z - p[i].z;
                    const double vx = p[k].x - p[i].x, vy = p[k].y - p[i].y, vz = p[k].z - p[i].z;
                    double n[3] = {uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
                    const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (length < size * size * 1e-9)
                        continue;
                    for (double& c : n)
                        c /= length;
                    const double d = n[0] * p[i].x + n[1] * p[i].y + n[2] * p[i].z;
                    bool below = true, above = true;
                    for (const Vector3& q : p) {
                        const double s = n[0] * q.x + n[1] * q.y + n[2] * q.z - d;
                        below = below && s <= eps;
                        above = above && s >= -eps;
                    }
                    if (below)
                        result.planes.push_back({n[0], n[1], n[2], d});
                    else if (above)
                        result.planes.push_back({-n[0], -n[1], -n[2], -d});
                }
            }
        }
        return result;
    }

    // Distance outside the hull (negative inside)
    double outside(const HullPlanes& hull, const Vector3& q) {
        double worst = -1e30;
        for (const auto& plane : hull.planes)
            worst = std::max(worst, plane[0] * q.x + plane[1] * q.y + plane[2] * q.z - plane[3]);
        return worst;
    }

    bool insideAny(const std::vector<HullPlanes>& hulls, const Vector3& q, double tolerance) {
        for (const HullPlanes& hull : hulls)
            if (outside(hull, q) <= tolerance)
                return true;
        return false;
    }

    std::vector<HullPlanes> planesOf(const std::vector<ConvexHullPoints>& hulls) {
        std::vector<HullPlanes> result;
        for (const ConvexHullPoints& hull : hulls)
            result.push_back(hullPlanes(hull));
        return result;
    }

    const DecompositionParams LOW = DecompositionParams::fromQuality(DecompositionQuality::LOW);

    template <typename Predicate>
    bool waitUntil(std::chrono::milliseconds limit, Predicate done) {
    #if defined(JASPERENGINE_SANITIZE_ADDRESS) || defined(JASPERENGINE_SANITIZE_THREAD) || defined(JASPERENGINE_SANITIZE_UNDEFINED)
        // V-HACD in a sanitizer's Debug build takes many times as long (the limits measure
        // nothing: the quick-cancel checks keep their own budgets)
        limit *= 10;
    #endif
        const Clock::time_point end = Clock::now() + limit;
        while (!done()) {
            if (Clock::now() > end)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    class TempDir {
    public:
        TempDir() {
            std::random_device device;
            path = fs::temp_directory_path() / ("jasper-convexdecomposition-" + std::to_string(device()) + std::to_string(device()));
        }
        ~TempDir() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
        std::string str() const { return path.string(); }
        fs::path path;
    };

    std::string entryName(uint64_t key) {
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.hulls", static_cast<unsigned long long>(key));
        return name;
    }

    std::vector<ConvexHullPoints> sampleHulls() {
        std::vector<ConvexHullPoints> hulls(2);
        hulls[0].points = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 1)};
        hulls[1].points = {Vector3(-1.5f, 2, 3), Vector3(4, -5.25f, 6), Vector3(7, 8, -9), Vector3(0.125f, 0, 0), Vector3(1e-7f, -0.0f, 3e8f)};
        return hulls;
    }

    void expectSameHulls(const std::vector<ConvexHullPoints>& a, const std::vector<ConvexHullPoints>& b) {
        ASSERT_EQ(a.size(), b.size());
        for (size_t i = 0; i < a.size(); i++) {
            ASSERT_EQ(a[i].points.size(), b[i].points.size());
            for (size_t j = 0; j < a[i].points.size(); j++)
                EXPECT_EQ(a[i].points[j], b[i].points[j]);
        }
    }

    // The tests that run V-HACD skip in builds without it (Web, Android, iOS)
    #define REQUIRE_DECOMPOSITION() \
        if (!ConvexDecompositionCache::available()) GTEST_SKIP() << "built without JASPERENGINE_CONVEX_DECOMPOSITION"

}

TEST(ConvexDecomposition, QualityPresets) {
    // The design specifies Medium's defaults. Low/High's exact tuning numbers
    // are implementation choices, and are exercised by the placement workflows.
    const DecompositionParams medium = DecompositionParams::fromQuality(DecompositionQuality::MEDIUM);
    EXPECT_EQ(medium.maxHulls, 8u);
    EXPECT_EQ(medium.resolution, 400000u);
    EXPECT_EQ(medium.maxVerticesPerHull, 64u);
}

TEST(ConvexDecomposition, CubeIsOneHullHoldingItsCorners) {
    REQUIRE_DECOMPOSITION();
    const Mesh mesh = cube();
    std::vector<ConvexHullPoints> hulls;
    std::string error;
    ASSERT_TRUE(ConvexDecompositionCache::decompose(mesh.positions, mesh.indices, LOW, hulls, nullptr, &error)) << error;
    ASSERT_EQ(hulls.size(), 1u);
    EXPECT_GE(hulls[0].points.size(), 8u);
    EXPECT_LE(hulls[0].points.size(), LOW.maxVerticesPerHull);
    const HullPlanes planes = hullPlanes(hulls[0]);
    for (const Vector3& corner : mesh.positions)
        EXPECT_LE(outside(planes, corner), 0.02) << corner.toString();
    // in the mesh's space: nothing beyond the cube
    for (const Vector3& p : hulls[0].points) {
        EXPECT_LE(std::fabs(p.x), 0.5f + 1e-4f);
        EXPECT_LE(std::fabs(p.y), 0.5f + 1e-4f);
        EXPECT_LE(std::fabs(p.z), 0.5f + 1e-4f);
    }
}

TEST(ConvexDecomposition, LShapeSplitsAroundItsConcavity) {
    REQUIRE_DECOMPOSITION();
    const Mesh mesh = lShape();
    std::vector<ConvexHullPoints> hulls;
    std::string error;
    ASSERT_TRUE(ConvexDecompositionCache::decompose(mesh.positions, mesh.indices, LOW, hulls, nullptr, &error)) << error;
    EXPECT_GE(hulls.size(), 2u);
    EXPECT_LE(hulls.size(), LOW.maxHulls);
    const std::vector<HullPlanes> planes = planesOf(hulls);
    for (const Vector3& v : mesh.positions)
        EXPECT_TRUE(insideAny(planes, v, 0.03)) << v.toString();
    // the notch stays empty
    EXPECT_FALSE(insideAny(planes, Vector3(1.25f, 0.5f, 1.25f), 0.0));
}

TEST(ConvexDecomposition, TorusLeavesItsHoleOpen) {
    REQUIRE_DECOMPOSITION();
    const Mesh mesh = torus(1.0f, 0.3f, 32, 12);
    // coarse voxels keep it fast; the ring still needs every hull it is allowed
    DecompositionParams params = LOW;
    params.maxHulls = 6;
    params.resolution = 20000;
    std::vector<ConvexHullPoints> hulls;
    std::string error;
    ASSERT_TRUE(ConvexDecompositionCache::decompose(mesh.positions, mesh.indices, params, hulls, nullptr, &error)) << error;
    EXPECT_GE(hulls.size(), 4u);
    EXPECT_LE(hulls.size(), params.maxHulls);
    const std::vector<HullPlanes> planes = planesOf(hulls);
    for (const HullPlanes& hull : planes)
        EXPECT_GT(outside(hull, Vector3(0, 0, 0)), 0.1);
    // the ring itself is covered
    for (int i = 0; i < 8; i++) {
        const float u = 6.28318530718f * float(i) / 8.0f;
        EXPECT_TRUE(insideAny(planes, Vector3(std::cos(u), 0, std::sin(u)), 0.0)) << i;
    }
}

TEST(ConvexDecomposition, SameMeshSameHullsBitForBit) {
    REQUIRE_DECOMPOSITION();
    // V-HACD splits and merges on its job pool: the order the jobs finish in must not show
    const Mesh mesh = torus(1.0f, 0.3f, 24, 10);
    DecompositionParams params = LOW;
    params.resolution = 20000;
    std::vector<ConvexHullPoints> first;
    std::string error;
    ASSERT_TRUE(ConvexDecompositionCache::decompose(mesh.positions, mesh.indices, params, first, nullptr, &error)) << error;
    std::vector<ConvexHullPoints> again;
    ASSERT_TRUE(ConvexDecompositionCache::decompose(mesh.positions, mesh.indices, params, again, nullptr, &error)) << error;
    ASSERT_EQ(again.size(), first.size());
    for (size_t h = 0; h < first.size(); h++) {
        ASSERT_EQ(again[h].points.size(), first[h].points.size());
        EXPECT_EQ(std::memcmp(again[h].points.data(), first[h].points.data(), first[h].points.size() * sizeof(Vector3)), 0) << h;
    }
}

TEST(ConvexDecomposition, RefusesFlatAndBrokenMeshes) {
    REQUIRE_DECOMPOSITION();
    std::vector<ConvexHullPoints> hulls;
    std::string error;
    auto refuse = [&](const Mesh& mesh, const DecompositionParams& params, const std::string& expected) {
        hulls.assign(1, ConvexHullPoints());
        error.clear();
        EXPECT_FALSE(ConvexDecompositionCache::decompose(mesh.positions, mesh.indices, params, hulls, nullptr, &error));
        EXPECT_EQ(error, expected);
        EXPECT_TRUE(hulls.empty());
    };

    refuse(plane(Vector3(1, 0, 0), Vector3(0, 0, 1), 4), LOW, "mesh has no volume");
    refuse(plane(Vector3(0.6f, 0.8f, 0), Vector3(0, 0.6f, 0.8f), 3), LOW, "mesh has no volume");   // tilted
    // two sheets of a plane, front and back: still flat
    Mesh sheet = plane(Vector3(1, 0, 0), Vector3(0, 1, 0), 2);
    const size_t front = sheet.indices.size();
    for (size_t t = 0; t < front; t += 3)
        sheet.indices.insert(sheet.indices.end(), {sheet.indices[t], sheet.indices[t + 2], sheet.indices[t + 1]});
    refuse(sheet, LOW, "mesh has no volume");

    Mesh triangle;
    triangle.positions = {Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0)};
    triangle.indices = {0, 1, 2};
    refuse(triangle, LOW, "mesh has fewer than 4 distinct vertices");

    Mesh noTriangles = cube();
    noTriangles.indices.clear();
    refuse(noTriangles, LOW, "mesh has no triangles");

    Mesh collapsed = cube();   // every triangle has a repeated corner
    for (size_t t = 0; t < collapsed.indices.size(); t += 3)
        collapsed.indices[t + 1] = collapsed.indices[t];
    refuse(collapsed, LOW, "mesh has no triangles");

    Mesh outOfRange = cube();
    outOfRange.indices.back() = 8;
    refuse(outOfRange, LOW, "triangle index out of range");

    Mesh partial = cube();
    partial.indices.pop_back();
    refuse(partial, LOW, "index count is not a multiple of 3");

    Mesh notFinite = cube();
    notFinite.positions[3].y = std::nanf("");
    refuse(notFinite, LOW, "mesh has a non-finite position");

    DecompositionParams params = LOW;
    params.maxHulls = 0;
    refuse(cube(), params, "maxHulls must be at least 1");
    params = LOW;
    params.resolution = 100;
    refuse(cube(), params, "resolution must be between 10000 and 64000000 voxels");
    params = LOW;
    params.maxVerticesPerHull = 3;
    refuse(cube(), params, "maxVerticesPerHull must be at least 4");

    // a cancel raised before the call stops it at once
    std::atomic<bool> cancel{true};
    const Mesh mesh = cube();
    EXPECT_FALSE(ConvexDecompositionCache::decompose(mesh.positions, mesh.indices, LOW, hulls, &cancel, &error));
    EXPECT_EQ(error, "cancelled");
}

namespace {

    // Heap bytes in use by the whole process, or 0 where this build cannot tell
    size_t heapBytesInUse() {
    #if defined(JASPERENGINE_SANITIZE_ADDRESS)
        return __sanitizer_get_current_allocated_bytes();
    #elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
        const struct mallinfo2 info = mallinfo2();
        return info.uordblks + info.hblkhd;
    #else
        return 0;
    #endif
    }

}

// A cancel that lands once V-HACD has merged its hulls down to maxHulls used to leak every one
// of them: its finalize loop stopped on the cancel, then cleared the map that owned the rest
// (engine/libs/vhacd/README.jasper.md). The editor's LeakSanitizer runs saw it whenever a
// Physics Placement decomposition was cancelled in that window.
TEST(ConvexDecomposition, CancelWhileFinalizingFreesTheHulls) {
    REQUIRE_DECOMPOSITION();
    const Mesh mesh = torus(1.0f, 0.3f, 32, 12);
    DecompositionParams params = LOW;
    params.maxHulls = 2;          // far fewer than V-HACD splits the ring into: it has to merge
    params.resolution = 20000;
    std::vector<ConvexHullPoints> hulls;
    std::string error;
    // a whole run first: what V-HACD and its job pool set up once is not counted below
    ASSERT_TRUE(ConvexDecompositionCache::decompose(mesh.positions, mesh.indices, params, hulls, nullptr, &error)) << error;

    const size_t before = heapBytesInUse();
    const int RUNS = 10;
    for (int run = 0; run < RUNS; run++) {
        std::atomic<bool> cancel{false};
        bool merged = false;
        bool cancelled = false;
        auto progress = [&](const char* stage, double stageProgress) {
            if (std::strcmp(stage, "COMPUTING_COST_MATRIX") == 0)
                merged = true;
            // the hulls are merged and about to be handed over: V-HACD owns every one of them
            if (merged && std::strcmp(stage, "FINALIZING_RESULTS") == 0 && stageProgress == 0.0) {
                cancelled = true;
                cancel = true;
            }
        };
        error.clear();
        EXPECT_FALSE(ConvexDecompositionCache::decompose(mesh.positions, mesh.indices, params, hulls, &cancel, &error, progress));
        EXPECT_EQ(error, "cancelled");
        EXPECT_TRUE(hulls.empty());
        ASSERT_TRUE(cancelled) << "V-HACD did not merge these hulls (merged " << merged << ")";
    }
    const size_t after = heapBytesInUse();
    if (before != 0) {
        // each leaking run lost every hull, tens of KB; allocator bookkeeping stays far below that
        const long long grown = static_cast<long long>(after) - static_cast<long long>(before);
        EXPECT_LT(grown, 16 * 1024) << "the heap grew by " << grown << " bytes over " << RUNS << " cancelled runs";
    }
}

TEST(ConvexDecomposition, HashWeldsCopiesAndCountsParams) {
    const Mesh mesh = cube();
    const uint64_t key = hashDecompositionInput(mesh.positions, mesh.indices, LOW);
    EXPECT_EQ(key, hashDecompositionInput(mesh.positions, mesh.indices, LOW));

    // UV-seam copies: each face's corners duplicated, the triangles pointing at the copies
    Mesh seams = mesh;
    for (size_t i = 0; i < mesh.indices.size(); i++) {
        seams.positions.push_back(mesh.positions[mesh.indices[i]]);
        seams.indices[i] = static_cast<uint32_t>(seams.positions.size() - 1);
    }
    // the first copy of a corner comes first, so the welded order is the cube's
    std::vector<Vector3> reordered = mesh.positions;
    reordered.insert(reordered.end(), seams.positions.begin() + 8, seams.positions.end());
    seams.positions = reordered;
    EXPECT_EQ(hashDecompositionInput(seams.positions, seams.indices, LOW), key);

    // -0 is +0
    Mesh withZero = box(Vector3(0, -0.5f, -0.5f), Vector3(1, 0.5f, 0.5f));
    Mesh withNegativeZero = box(Vector3(-0.0f, -0.5f, -0.5f), Vector3(1, 0.5f, 0.5f));
    EXPECT_TRUE(std::signbit(withNegativeZero.positions[0].x));
    EXPECT_EQ(hashDecompositionInput(withZero.positions, withZero.indices, LOW),
              hashDecompositionInput(withNegativeZero.positions, withNegativeZero.indices, LOW));

    // what changes the hulls changes the key
    DecompositionParams params = LOW;
    params.maxHulls++;
    EXPECT_NE(hashDecompositionInput(mesh.positions, mesh.indices, params), key);
    params = LOW;
    params.resolution++;
    EXPECT_NE(hashDecompositionInput(mesh.positions, mesh.indices, params), key);
    params = LOW;
    params.maxVerticesPerHull++;
    EXPECT_NE(hashDecompositionInput(mesh.positions, mesh.indices, params), key);
    EXPECT_NE(hashDecompositionInput(mesh.positions, mesh.indices, DecompositionParams()), key);
    Mesh moved = mesh;
    moved.positions[5].x = std::nextafter(moved.positions[5].x, 1.0f);
    EXPECT_NE(hashDecompositionInput(moved.positions, moved.indices, LOW), key);
    Mesh flipped = mesh;
    std::swap(flipped.indices[1], flipped.indices[2]);
    EXPECT_NE(hashDecompositionInput(flipped.positions, flipped.indices, LOW), key);

    // pinned: the key is little-endian xxHash64 on every platform; a change here orphans every
    // cache on disk, so it goes with a bump of the algorithm version
    EXPECT_EQ(key, 0xec527fb09a44c4b6ull) << std::hex << key;
}

TEST(ConvexDecomposition, DiskEntriesAreSharedBetweenInstances) {
    TempDir dir;
    const fs::path directory = dir.path / "nested" / "colliders";
    const std::vector<ConvexHullPoints> hulls = sampleHulls();
    const uint64_t key = 0x0123456789abcdefull;
    {
        ConvexDecompositionCache cache(directory.string());
        EXPECT_EQ(cache.getDirectory(), directory.string());
        std::vector<ConvexHullPoints> found;
        EXPECT_FALSE(cache.find(key, found));
        EXPECT_EQ(cache.status(key), DecompositionStatus::NONE);
        EXPECT_FALSE(fs::exists(directory));   // created by the first store only
        cache.store(key, hulls);
        ASSERT_TRUE(fs::exists(directory / entryName(key)));
        ASSERT_TRUE(cache.find(key, found));
        expectSameHulls(found, hulls);
        EXPECT_EQ(cache.entryCount(), 1u);
        // magic, version, key, count; per hull a count and float3s
        const size_t expected = 4 + 4 + 8 + 4 + (4 + 4 * 12) + (4 + 5 * 12);
        EXPECT_EQ(cache.bytesOnDisk(), expected);
        EXPECT_EQ(fs::file_size(directory / entryName(key)), expected);
        // nothing left aside
        size_t files = 0;
        for (const auto& entry : fs::directory_iterator(directory))
            files += entry.is_regular_file() ? 1 : 0;
        EXPECT_EQ(files, 1u);
    }
    ConvexDecompositionCache other(directory.string());
    EXPECT_EQ(other.status(key), DecompositionStatus::READY);
    std::vector<ConvexHullPoints> found;
    ASSERT_TRUE(other.find(key, found));
    expectSameHulls(found, hulls);
    // memory and disk hold the same key once
    EXPECT_EQ(other.entryCount(), 1u);
    other.store(key + 1, hulls);
    EXPECT_EQ(other.entryCount(), 2u);

    // unrelated files are neither counted nor cleared
    std::ofstream(directory / "notes.txt") << "keep";
    std::ofstream(directory / "0123.hulls") << "keep";
    EXPECT_EQ(other.entryCount(), 2u);
    other.clear();
    EXPECT_EQ(other.entryCount(), 0u);
    EXPECT_EQ(other.bytesOnDisk(), 0u);
    EXPECT_FALSE(other.find(key, found));
    EXPECT_TRUE(fs::exists(directory / "notes.txt"));
    EXPECT_TRUE(fs::exists(directory / "0123.hulls"));
}

TEST(ConvexDecomposition, CorruptEntriesAreMissesAndDeleted) {
    TempDir dir;
    const uint64_t key = 42;
    const fs::path path = dir.path / entryName(key);
    {
        ConvexDecompositionCache writer(dir.str());
        writer.store(key, sampleHulls());
    }
    std::vector<uint8_t> good;
    {
        std::ifstream in(path, std::ios::binary);
        good.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    ASSERT_FALSE(good.empty());

    auto write = [&](const std::vector<uint8_t>& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    };
    auto expectMiss = [&](const char* what) {
        ConvexDecompositionCache cache(dir.str());
        std::vector<ConvexHullPoints> found;
        EXPECT_FALSE(cache.find(key, found)) << what;
        EXPECT_FALSE(fs::exists(path)) << what;
        EXPECT_EQ(cache.status(key), DecompositionStatus::NONE) << what;
    };

    write({'g', 'a', 'r', 'b', 'a', 'g', 'e'});
    expectMiss("garbage");
    write(std::vector<uint8_t>(good.begin(), good.end() - 5));
    expectMiss("truncated");
    std::vector<uint8_t> longer = good;
    longer.push_back(0);
    write(longer);
    expectMiss("trailing bytes");
    std::vector<uint8_t> otherKey = good;
    otherKey[8] ^= 1;
    write(otherKey);
    expectMiss("another key's entry");
    std::vector<uint8_t> nan = good;
    std::fill(nan.end() - 4, nan.end(), 0xFF);
    write(nan);
    expectMiss("non-finite point");
    write({});
    expectMiss("empty");

    // the intact file still reads
    write(good);
    ConvexDecompositionCache cache(dir.str());
    std::vector<ConvexHullPoints> found;
    EXPECT_TRUE(cache.find(key, found));
    expectSameHulls(found, sampleHulls());
}

TEST(ConvexDecomposition, ClearLeavesOtherWritersTemporaryFiles) {
    TempDir dir;
    ConvexDecompositionCache cache(dir.str());
    cache.store(1, sampleHulls());
    // another process (a CLI run on the same project) halfway through writing entry 5: deleting
    // its file would fail its rename and lose the entry
    const fs::path busy = dir.path / (entryName(5) + ".0123456789abcdef-7.tmp");
    std::ofstream(busy) << "half";
    const fs::path crashed = dir.path / (entryName(6) + ".0123456789abcdef-2.tmp");
    std::ofstream(crashed) << "half";
    fs::last_write_time(crashed, fs::file_time_type::clock::now() - std::chrono::minutes(61));

    cache.clear();
    EXPECT_FALSE(fs::exists(dir.path / entryName(1)));
    EXPECT_TRUE(fs::exists(busy));
    EXPECT_FALSE(fs::exists(crashed));
    // the other writer's rename still lands, and a cache reading the directory finds it
    fs::rename(busy, dir.path / entryName(5));
    ConvexDecompositionCache other(dir.str());
    EXPECT_EQ(other.entryCount(), 1u);
}

TEST(ConvexDecomposition, UnsavableEntryStaysInMemory) {
    TempDir dir;
    const uint64_t key = 77;
    // a directory where the file should go: every rename onto it fails
    fs::create_directories(dir.path / entryName(key) / "blocker");
    {
        ConvexDecompositionCache cache(dir.str());
        EXPECT_EQ(cache.lastDiskError(), "");
        cache.store(key, sampleHulls());
        std::vector<ConvexHullPoints> found;
        ASSERT_TRUE(cache.find(key, found));
        expectSameHulls(found, sampleHulls());
        EXPECT_EQ(cache.status(key), DecompositionStatus::READY);
        // reported, not silent
        EXPECT_EQ(cache.lastDiskError().rfind("could not rename " + (dir.path / entryName(key)).string() + ".", 0), 0u)
            << cache.lastDiskError();
        // the next save that works clears it
        cache.store(key + 1, sampleHulls());
        EXPECT_EQ(cache.lastDiskError(), "");
        EXPECT_TRUE(fs::exists(dir.path / entryName(key + 1)));
        fs::remove(dir.path / entryName(key + 1));
        cache.store(key, sampleHulls());
        EXPECT_NE(cache.lastDiskError(), "");
        cache.clear();
        EXPECT_EQ(cache.lastDiskError(), "");
        cache.store(key, sampleHulls());   // fails again, for the checks below
    }
    // both tries cleaned up after themselves, and nothing claims the entry is on disk
    size_t entries = 0;
    for (const auto& entry : fs::directory_iterator(dir.path)) {
        EXPECT_EQ(entry.path().filename().string(), entryName(key));
        entries++;
    }
    EXPECT_EQ(entries, 1u);
    ConvexDecompositionCache other(dir.str());
    std::vector<ConvexHullPoints> found;
    EXPECT_FALSE(other.find(key, found));
    EXPECT_EQ(other.entryCount(), 0u);
}

TEST(ConvexDecomposition, DiskMissesAreRememberedUntilRequest) {
    TempDir dir;
    ConvexDecompositionCache cache(dir.str());
    ConvexDecompositionCache writer(dir.str());   // another process sharing the directory
    const Mesh mesh = cube();
    const uint64_t key = hashDecompositionInput(mesh.positions, mesh.indices, LOW);
    EXPECT_EQ(cache.status(key), DecompositionStatus::NONE);

    // written behind its back: polling keeps the remembered miss, without touching the disk
    writer.store(key, sampleHulls());
    std::vector<ConvexHullPoints> found;
    EXPECT_EQ(cache.status(key), DecompositionStatus::NONE);
    EXPECT_FALSE(cache.find(key, found));

    // request() looks again and loads it instead of decomposing
    EXPECT_EQ(cache.request(mesh.positions, mesh.indices, LOW), key);
    EXPECT_EQ(cache.pendingCount(), 0u);
    EXPECT_EQ(cache.completionVersion(), 0u);
    EXPECT_EQ(cache.status(key), DecompositionStatus::READY);
    ASSERT_TRUE(cache.find(key, found));
    expectSameHulls(found, sampleHulls());

    // clear() forgets misses too
    const uint64_t other = key + 1;
    EXPECT_EQ(cache.status(other), DecompositionStatus::NONE);
    cache.clear();
    writer.store(other, sampleHulls());
    EXPECT_EQ(cache.status(other), DecompositionStatus::READY);

    // and so does a store() of the key itself
    const uint64_t third = key + 2;
    EXPECT_FALSE(cache.find(third, found));
    cache.store(third, sampleHulls());
    EXPECT_EQ(cache.status(third), DecompositionStatus::READY);
}

TEST(ConvexDecomposition, MemoryOnlyCache) {
    ConvexDecompositionCache cache("");
    const std::vector<ConvexHullPoints> hulls = sampleHulls();
    cache.store(7, hulls);
    std::vector<ConvexHullPoints> found;
    ASSERT_TRUE(cache.find(7, found));
    expectSameHulls(found, hulls);
    EXPECT_EQ(cache.status(7), DecompositionStatus::READY);
    EXPECT_EQ(cache.entryCount(), 1u);
    EXPECT_EQ(cache.bytesOnDisk(), 0u);
    cache.clear();
    EXPECT_FALSE(cache.find(7, found));
    EXPECT_EQ(cache.entryCount(), 0u);
}

TEST(ConvexDecomposition, RequestLandsInMemoryAndOnDisk) {
    REQUIRE_DECOMPOSITION();
    TempDir dir;
    const Mesh mesh = cube();
    const uint64_t expectedKey = hashDecompositionInput(mesh.positions, mesh.indices, LOW);
    {
        ConvexDecompositionCache cache(dir.str());
        const uint64_t version = cache.completionVersion();
        const uint64_t key = cache.request(mesh.positions, mesh.indices, LOW);
        EXPECT_EQ(key, expectedKey);
        EXPECT_NE(cache.status(key), DecompositionStatus::NONE);
        ASSERT_TRUE(waitUntil(std::chrono::seconds(5), [&]() { return cache.completionVersion() != version; }));
        EXPECT_EQ(cache.completionVersion(), version + 1);
        EXPECT_EQ(cache.status(key), DecompositionStatus::READY);
        EXPECT_EQ(cache.pendingCount(), 0u);
        EXPECT_EQ(cache.failureReason(key), "");
        std::vector<ConvexHullPoints> hulls;
        ASSERT_TRUE(cache.find(key, hulls));
        EXPECT_EQ(hulls.size(), 1u);
        EXPECT_TRUE(fs::exists(dir.path / entryName(key)));

        // cached: no new job
        EXPECT_EQ(cache.request(mesh.positions, mesh.indices, LOW), key);
        EXPECT_EQ(cache.pendingCount(), 0u);
        EXPECT_EQ(cache.completionVersion(), version + 1);
    }
    // a new instance reads it from disk without running anything
    ConvexDecompositionCache cache(dir.str());
    EXPECT_EQ(cache.request(mesh.positions, mesh.indices, LOW), expectedKey);
    EXPECT_EQ(cache.pendingCount(), 0u);
    EXPECT_EQ(cache.completionVersion(), 0u);
    EXPECT_EQ(cache.status(expectedKey), DecompositionStatus::READY);
}

TEST(ConvexDecomposition, FailuresStickUntilCleared) {
    REQUIRE_DECOMPOSITION();
    ConvexDecompositionCache cache("");
    const Mesh flat = plane(Vector3(1, 0, 0), Vector3(0, 0, 1), 2);
    const uint64_t key = cache.request(flat.positions, flat.indices, LOW);
    ASSERT_TRUE(waitUntil(std::chrono::seconds(5), [&]() { return cache.status(key) == DecompositionStatus::FAILED; }));
    EXPECT_EQ(cache.failureReason(key), "mesh has no volume");
    EXPECT_EQ(cache.completionVersion(), 1u);
    // not retried
    cache.request(flat.positions, flat.indices, LOW);
    EXPECT_EQ(cache.pendingCount(), 0u);
    EXPECT_EQ(cache.status(key), DecompositionStatus::FAILED);
    EXPECT_EQ(cache.completionVersion(), 1u);

    // broken input fails on the spot
    Mesh broken = cube();
    broken.indices.back() = 99;
    const uint64_t brokenKey = cache.request(broken.positions, broken.indices, LOW);
    EXPECT_EQ(cache.status(brokenKey), DecompositionStatus::FAILED);
    EXPECT_EQ(cache.failureReason(brokenKey), "triangle index out of range");
    EXPECT_EQ(cache.completionVersion(), 2u);

    cache.clear();
    EXPECT_EQ(cache.status(key), DecompositionStatus::NONE);
    EXPECT_EQ(cache.failureReason(key), "");
    cache.request(flat.positions, flat.indices, LOW);
    ASSERT_TRUE(waitUntil(std::chrono::seconds(5), [&]() { return cache.completionVersion() == 3u; }));
    EXPECT_EQ(cache.status(key), DecompositionStatus::FAILED);
}

TEST(ConvexDecomposition, CancelStopsARunningJobQuickly) {
    REQUIRE_DECOMPOSITION();
    ConvexDecompositionCache cache("");
    const Mesh dense = torus(1.0f, 0.35f, 256, 128);
    const DecompositionParams high = DecompositionParams::fromQuality(DecompositionQuality::HIGH);
    const Mesh small = cube();

    const uint64_t key = cache.request(dense.positions, dense.indices, high);
    const uint64_t queuedKey = cache.request(small.positions, small.indices, LOW);   // waits behind it
    EXPECT_EQ(cache.status(key), DecompositionStatus::PENDING);
    EXPECT_EQ(cache.status(queuedKey), DecompositionStatus::PENDING);
    EXPECT_EQ(cache.pendingCount(), 2u);

    // a queued job is dropped at once
    cache.cancel(queuedKey);
    EXPECT_EQ(cache.status(queuedKey), DecompositionStatus::FAILED);
    EXPECT_EQ(cache.failureReason(queuedKey), "cancelled");
    EXPECT_EQ(cache.completionVersion(), 1u);
    EXPECT_EQ(cache.pendingCount(), 1u);

    // let V-HACD get going, then stop it
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_EQ(cache.status(key), DecompositionStatus::PENDING);
    const Clock::time_point start = Clock::now();
    cache.cancel(key);
    ASSERT_TRUE(waitUntil(std::chrono::seconds(10), [&]() { return cache.completionVersion() == 2u; }));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    EXPECT_LT(took.count(), 2000);
    EXPECT_EQ(cache.status(key), DecompositionStatus::FAILED);
    EXPECT_EQ(cache.failureReason(key), "cancelled");
    EXPECT_EQ(cache.pendingCount(), 0u);
    std::vector<ConvexHullPoints> hulls;
    EXPECT_FALSE(cache.find(key, hulls));

    // a cancelled key is retried by the next request
    cache.request(small.positions, small.indices, LOW);
    ASSERT_TRUE(waitUntil(std::chrono::seconds(5), [&]() { return cache.status(queuedKey) == DecompositionStatus::READY; }));
    EXPECT_EQ(cache.failureReason(queuedKey), "");

    // cancelAll stops the running job and waits for it
    cache.request(dense.positions, dense.indices, high);
    EXPECT_EQ(cache.status(key), DecompositionStatus::PENDING);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const Clock::time_point allStart = Clock::now();
    cache.cancelAll();
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - allStart).count(), 2000);
    EXPECT_EQ(cache.pendingCount(), 0u);
    EXPECT_EQ(cache.status(key), DecompositionStatus::FAILED);
    EXPECT_EQ(cache.failureReason(key), "cancelled");

    // the worker starts again after cancelAll
    const Mesh other = box(Vector3(0, 0, 0), Vector3(1, 2, 3));
    const uint64_t otherKey = cache.request(other.positions, other.indices, LOW);
    EXPECT_TRUE(waitUntil(std::chrono::seconds(5), [&]() { return cache.status(otherKey) == DecompositionStatus::READY; }));
    // cancel of a key that is not pending does nothing
    const uint64_t version = cache.completionVersion();
    cache.cancel(otherKey);
    cache.cancel(12345);
    EXPECT_EQ(cache.status(otherKey), DecompositionStatus::READY);
    EXPECT_EQ(cache.status(12345), DecompositionStatus::NONE);
    EXPECT_EQ(cache.completionVersion(), version);
}

namespace {

    // A cache whose worker is held by a long job, so requests queue up behind it: release()
    // cancels that job and the queue drains in order.
    class BusyCache {
    public:
        BusyCache(): cache("") {
            const Mesh dense = torus(1.0f, 0.35f, 256, 128);
            blocker = cache.request(dense.positions, dense.indices, DecompositionParams::fromQuality(DecompositionQuality::HIGH));
            // the worker has picked it up
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Distinct quick jobs: boxes of their own size, coarse voxels
        uint64_t request(int which, bool urgent) {
            const Mesh mesh = box(Vector3(0, 0, 0), Vector3(1.0f + 0.1f * float(which), 1.0f, 1.0f));
            DecompositionParams params = LOW;
            params.resolution = 10000;
            return cache.request(mesh.positions, mesh.indices, params, urgent);
        }

        // Cancels the blocker, then the keys in the order they become READY, until count of them did
        std::vector<uint64_t> release(const std::vector<uint64_t>& keys, size_t count) {
            EXPECT_EQ(cache.status(blocker), DecompositionStatus::PENDING);
            EXPECT_EQ(cache.failureReason(blocker), "");   // not preempted by anything queued
            cache.cancel(blocker);
            std::vector<uint64_t> order;
            waitUntil(std::chrono::seconds(10), [&]() {
                for (uint64_t key : keys)
                    if (std::find(order.begin(), order.end(), key) == order.end() && cache.status(key) == DecompositionStatus::READY)
                        order.push_back(key);
                return order.size() >= count;
            });
            cache.cancelAll();
            EXPECT_EQ(cache.failureReason(blocker), "cancelled");
            return order;
        }

        ConvexDecompositionCache cache;
        uint64_t blocker = 0;
    };

}

TEST(ConvexDecomposition, UrgentRequestJumpsTheQueue) {
    REQUIRE_DECOMPOSITION();
    BusyCache busy;
    std::vector<uint64_t> keys;
    for (int i = 0; i < 5; i++)
        keys.push_back(busy.request(i, false));
    const uint64_t urgent = busy.request(5, true);
    keys.push_back(urgent);
    EXPECT_EQ(busy.cache.pendingCount(), 7u);
    const std::vector<uint64_t> order = busy.release(keys, 2);
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], urgent);
    EXPECT_EQ(order[1], keys[0]);   // then the others, first come first served
}

TEST(ConvexDecomposition, PrioritizeMovesAQueuedKeyToTheFront) {
    REQUIRE_DECOMPOSITION();
    BusyCache busy;
    std::vector<uint64_t> keys;
    for (int i = 0; i < 5; i++)
        keys.push_back(busy.request(i, false));
    busy.cache.prioritize(keys[3]);
    // no effect on the running job, an unknown key or one already urgent
    busy.cache.prioritize(busy.blocker);
    busy.cache.prioritize(12345);
    busy.cache.prioritize(keys[3]);
    EXPECT_EQ(busy.cache.pendingCount(), 6u);
    EXPECT_EQ(busy.cache.status(12345), DecompositionStatus::NONE);
    const std::vector<uint64_t> order = busy.release(keys, 3);
    ASSERT_EQ(order.size(), 3u);
    EXPECT_EQ(order[0], keys[3]);
    EXPECT_EQ(order[1], keys[0]);
    EXPECT_EQ(order[2], keys[1]);
}

TEST(ConvexDecomposition, UrgentRequestsRunFirstComeFirstServed) {
    REQUIRE_DECOMPOSITION();
    BusyCache busy;
    std::vector<uint64_t> keys;
    keys.push_back(busy.request(0, false));
    keys.push_back(busy.request(1, false));
    keys.push_back(busy.request(2, true));
    keys.push_back(busy.request(3, true));
    // a queued key requested again urgently, and one prioritized, line up behind them
    EXPECT_EQ(busy.request(1, true), keys[1]);
    keys.push_back(busy.request(4, false));
    busy.cache.prioritize(keys[4]);
    keys.push_back(busy.request(5, true));
    EXPECT_EQ(busy.cache.pendingCount(), 7u);
    const std::vector<uint64_t> order = busy.release(keys, 6);
    ASSERT_EQ(order.size(), 6u);
    EXPECT_EQ(order, (std::vector<uint64_t>{keys[2], keys[3], keys[1], keys[4], keys[5], keys[0]}));
}

TEST(ConvexDecomposition, ConcurrentRequests) {
    REQUIRE_DECOMPOSITION();
    TempDir dir;
    ConvexDecompositionCache cache(dir.str());
    std::vector<Mesh> meshes;
    for (int i = 0; i < 4; i++)
        meshes.push_back(box(Vector3(0, 0, 0), Vector3(1.0f + float(i), 1.0f, 0.5f + 0.25f * float(i))));

    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, t]() {
            // each thread asks for every mesh, in its own order, and polls them all
            std::vector<uint64_t> keys;
            for (int i = 0; i < 4; i++) {
                const Mesh& mesh = meshes[(i + t) % 4];
                keys.push_back(cache.request(mesh.positions, mesh.indices, LOW));
            }
            const bool done = waitUntil(std::chrono::seconds(8), [&]() {
                for (uint64_t key : keys) {
                    if (cache.status(key) != DecompositionStatus::READY)
                        return false;
                    cache.pendingCount();
                    cache.entryCount();
                }
                return true;
            });
            std::vector<ConvexHullPoints> hulls;
            for (uint64_t key : keys)
                if (done && cache.find(key, hulls) && hulls.size() == 1)
                    ready++;
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    EXPECT_EQ(ready.load(), 16);
    // every mesh decomposed once
    EXPECT_EQ(cache.completionVersion(), 4u);
    EXPECT_EQ(cache.entryCount(), 4u);
    EXPECT_EQ(cache.pendingCount(), 0u);
}

TEST(ConvexDecomposition, ClearRemovesEverything) {
    REQUIRE_DECOMPOSITION();
    TempDir dir;
    ConvexDecompositionCache cache(dir.str());
    const Mesh mesh = cube();
    const uint64_t key = cache.request(mesh.positions, mesh.indices, LOW);
    ASSERT_TRUE(waitUntil(std::chrono::seconds(5), [&]() { return cache.status(key) == DecompositionStatus::READY; }));
    cache.store(99, sampleHulls());
    const Mesh flat = plane(Vector3(1, 0, 0), Vector3(0, 0, 1), 1);
    const uint64_t flatKey = cache.request(flat.positions, flat.indices, LOW);
    ASSERT_TRUE(waitUntil(std::chrono::seconds(5), [&]() { return cache.status(flatKey) == DecompositionStatus::FAILED; }));
    // a temporary file a crashed writer left behind, long ago
    const fs::path stale = dir.path / (entryName(5) + ".00000000deadbeef-0.tmp");
    std::ofstream(stale) << "partial";
    fs::last_write_time(stale, fs::file_time_type::clock::now() - std::chrono::hours(2));
    EXPECT_EQ(cache.entryCount(), 2u);
    EXPECT_GT(cache.bytesOnDisk(), 0u);

    cache.clear();
    EXPECT_EQ(cache.entryCount(), 0u);
    EXPECT_EQ(cache.bytesOnDisk(), 0u);
    EXPECT_EQ(cache.status(key), DecompositionStatus::NONE);
    EXPECT_EQ(cache.status(99), DecompositionStatus::NONE);
    EXPECT_EQ(cache.status(flatKey), DecompositionStatus::NONE);
    EXPECT_TRUE(fs::is_empty(dir.path));
}

// the build option and what the cache reports agree (the test below is in one build or the other)
TEST(ConvexDecomposition, AvailabilityMatchesTheBuildOption) {
    EXPECT_EQ(ConvexDecompositionCache::available(), JASPER_TEST_CONVEX_DECOMPOSITION != 0);
}

// only in a build without V-HACD (S02 R11: registered there, never a skipped test here)
#if !JASPER_TEST_CONVEX_DECOMPOSITION
TEST(ConvexDecomposition, BuildsWithoutDecompositionStillCache) {
    const char* const expected = "convex decomposition is not available in this build";
    const Mesh mesh = cube();
    std::vector<ConvexHullPoints> hulls(1);
    std::string error;
    EXPECT_FALSE(ConvexDecompositionCache::decompose(mesh.positions, mesh.indices, LOW, hulls, nullptr, &error));
    EXPECT_EQ(error, expected);
    EXPECT_TRUE(hulls.empty());

    TempDir dir;
    ConvexDecompositionCache cache(dir.str());
    const uint64_t key = cache.request(mesh.positions, mesh.indices, LOW);
    EXPECT_EQ(key, hashDecompositionInput(mesh.positions, mesh.indices, LOW));
    EXPECT_EQ(cache.status(key), DecompositionStatus::FAILED);
    EXPECT_EQ(cache.failureReason(key), expected);
    EXPECT_EQ(cache.pendingCount(), 0u);

    // hulls baked elsewhere still serve
    cache.store(key, sampleHulls());
    EXPECT_EQ(cache.status(key), DecompositionStatus::READY);
    ConvexDecompositionCache other(dir.str());
    EXPECT_EQ(other.request(mesh.positions, mesh.indices, LOW), key);
    EXPECT_EQ(other.status(key), DecompositionStatus::READY);
    std::vector<ConvexHullPoints> loaded;
    ASSERT_TRUE(other.find(key, loaded));
    expectSameHulls(loaded, sampleHulls());
    other.clear();
    EXPECT_EQ(other.entryCount(), 0u);
}
#endif
