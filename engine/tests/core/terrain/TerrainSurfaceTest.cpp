// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The one surface query (T1 R14-R16, R93; AC10, AC11, AC73). The existing TerrainHeight.*,
// MeshRaycast.Kinds and JoltShapeBuilderTest keep it honest against the drawn surface from their side.

#include "terrain/TerrainHoles.h"
#include "terrain/TerrainSurface.h"

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "object/TerrainEdit.h"
#include "subsystem/MeshSystem.h"
#include "terrain/TerrainDeterministic.h"
#include "texture/Texture.h"
#include "texture/TextureData.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::terraincore;

namespace {

    constexpr float SIZE = 100.0f;
    constexpr float MAX_HEIGHT = 20.0f;

    struct HeightMap {
        std::vector<uint8_t> pixels;
        int width = 17;
        int height = 9;
        int channels = 1;
        int bytesPerChannel = 2;

        SurfaceView view() const {
            SurfaceView v;
            v.pixels = pixels.data();
            v.width = width;
            v.height = height;
            v.channels = channels;
            v.bytesPerChannel = bytesPerChannel;
            v.size = SIZE;
            v.maxHeight = MAX_HEIGHT;
            return v;
        }

        float texel(int x, int y) const {
            const size_t at = (static_cast<size_t>(y) * width + x) * channels * bytesPerChannel;
            if (bytesPerChannel == 2) {
                return static_cast<float>(pixels[at] | (pixels[at + 1] << 8)) / 65535.0f;
            }
            return pixels[at] / 255.0f;
        }
    };

    HeightMap random16() {
        HeightMap map;
        Pcg32 random(1234);
        for (int i = 0; i < map.width * map.height; i++) {
            const uint32_t v = random.next() & 0xFFFF;
            map.pixels.push_back(static_cast<uint8_t>(v & 0xFF));
            map.pixels.push_back(static_cast<uint8_t>(v >> 8));
        }
        return map;
    }

    HeightMap ramp8() {
        HeightMap map;
        map.bytesPerChannel = 1;
        map.width = 5;
        map.height = 4;
        for (int y = 0; y < map.height; y++) {
            for (int x = 0; x < map.width; x++) {
                map.pixels.push_back(static_cast<uint8_t>(x * 60 + (y >= 2 ? 15 : 0)));
            }
        }
        return map;
    }

    HeightMap rgba8() {
        // a legacy map: only R is height, the other channels must not leak in
        HeightMap map;
        map.channels = 4;
        map.bytesPerChannel = 1;
        map.width = 6;
        map.height = 6;
        Pcg32 random(99);
        for (int i = 0; i < map.width * map.height; i++) {
            map.pixels.push_back(static_cast<uint8_t>(random.next() & 0xFF));
            map.pixels.push_back(255);
            map.pixels.push_back(0);
            map.pixels.push_back(static_cast<uint8_t>(random.next() & 0xFF));
        }
        return map;
    }

    // terrain_vs.glsl's fetch: a software textureLod(sampler2D, uv, 0.0) with linear filtering and
    // clamp to edge, texel centres at (i + 0.5) / N
    float shaderFetch(const HeightMap& map, float u, float v) {
        const float tu = u * static_cast<float>(map.width) - 0.5f;
        const float tv = v * static_cast<float>(map.height) - 0.5f;
        const int i0 = static_cast<int>(std::floor(tu));
        const int j0 = static_cast<int>(std::floor(tv));
        const float a = tu - std::floor(tu);
        const float b = tv - std::floor(tv);
        auto at = [&](int i, int j) {
            return map.texel(std::max(0, std::min(i, map.width - 1)), std::max(0, std::min(j, map.height - 1)));
        };
        return (1.0f - a) * (1.0f - b) * at(i0, j0) + a * (1.0f - b) * at(i0 + 1, j0) +
               (1.0f - a) * b * at(i0, j0 + 1) + a * b * at(i0 + 1, j0 + 1);
    }

    // sampleTerrainSurface as it was before T1 moved it (MeshSystem.cpp at 1c59858cd): the
    // reference for "normals keep today's forward difference exactly"
    float todaysTexel(const HeightMap& map, int x, int y) {
        const size_t texelIndex = static_cast<size_t>(y) * map.width + x;
        const size_t index = texelIndex * map.channels * map.bytesPerChannel;
        if (map.bytesPerChannel >= 2) {
            const unsigned int value = static_cast<unsigned int>(map.pixels[index]) | (static_cast<unsigned int>(map.pixels[index + 1]) << 8);
            return static_cast<float>(value) / 65535.0f;
        }
        return map.pixels[index] / 255.0f;
    }

    float todaysHeight(const HeightMap& map, float x, float z) {
        float texelX = ((x + SIZE * 0.5f) / SIZE) * static_cast<float>(map.width) - 0.5f;
        float texelY = ((z + SIZE * 0.5f) / SIZE) * static_cast<float>(map.height) - 0.5f;
        texelX = std::clamp(texelX, 0.0f, static_cast<float>(map.width - 1));
        texelY = std::clamp(texelY, 0.0f, static_cast<float>(map.height - 1));
        const int lowerX = std::clamp(static_cast<int>(std::floor(texelX)), 0, map.width - 1);
        const int lowerY = std::clamp(static_cast<int>(std::floor(texelY)), 0, map.height - 1);
        const int upperX = std::min(lowerX + 1, map.width - 1);
        const int upperY = std::min(lowerY + 1, map.height - 1);
        const float blendX = texelX - static_cast<float>(lowerX);
        const float blendY = texelY - static_cast<float>(lowerY);
        const float lower = todaysTexel(map, lowerX, lowerY) + (todaysTexel(map, upperX, lowerY) - todaysTexel(map, lowerX, lowerY)) * blendX;
        const float upper = todaysTexel(map, lowerX, upperY) + (todaysTexel(map, upperX, upperY) - todaysTexel(map, lowerX, upperY)) * blendX;
        return MAX_HEIGHT * (lower + (upper - lower) * blendY);
    }

    // Terrain over one map in the pool, under a test's own id
    struct TerrainScope {
        TerrainComponent terrain;

        TerrainScope(const HeightMap& map, const char* id) {
            terrain.terrainSize = SIZE;
            terrain.maxHeight = MAX_HEIGHT;
            const size_t bytes = map.pixels.size();
            unsigned char* raw = static_cast<unsigned char*>(std::malloc(bytes));
            std::memcpy(raw, map.pixels.data(), bytes);
            const ColorFormat format = map.channels == 4 ? ColorFormat::RGBA : (map.bytesPerChannel == 2 ? ColorFormat::RED16 : ColorFormat::RED);
            TextureData data(map.width, map.height, static_cast<unsigned int>(bytes), format, map.channels, raw);
            data.setDataOwned(false);
            terrain.heightMap.setData(id, data);
            terrain.heightMap.getData().setDataOwned(true);
        }
    };

}

TEST(TerrainSurface, MatchesShaderFetch) {
    const HeightMap maps[] = {random16(), ramp8(), rgba8()};
    for (const HeightMap& map : maps) {
        const SurfaceView view = map.view();
        for (float z = -49.0f; z <= 49.0f; z += 3.7f) {
            for (float x = -49.0f; x <= 49.0f; x += 3.1f) {
                const float expected = shaderFetch(map, (x + SIZE * 0.5f) / SIZE, (z + SIZE * 0.5f) / SIZE) * MAX_HEIGHT;
                EXPECT_NEAR(sampleSurface(view, x, z).height, expected, 1e-4f * MAX_HEIGHT) << x << ", " << z;
                EXPECT_NEAR(sampleHeight(view, x, z), expected, 1e-4f * MAX_HEIGHT) << x << ", " << z;
            }
        }
    }
}

TEST(TerrainSurface, NormalIsTodaysForwardDifference) {
    const HeightMap maps[] = {random16(), ramp8(), rgba8()};
    for (const HeightMap& map : maps) {
        const SurfaceView view = map.view();
        const float delta = SIZE / static_cast<float>(map.width);
        for (float z = -60.0f; z <= 60.0f; z += 4.3f) {
            for (float x = -60.0f; x <= 60.0f; x += 3.9f) {
                const float height = todaysHeight(map, x, z);
                const Vector3 expected = Vector3(height - todaysHeight(map, x + delta, z), delta, height - todaysHeight(map, x, z + delta)).normalized();
                const SurfaceSample sample = sampleSurface(view, x, z);
                // bit for bit: this is what foliage and physics were placed with
                EXPECT_EQ(sample.height, height);
                EXPECT_EQ(sample.normal.x, expected.x);
                EXPECT_EQ(sample.normal.y, expected.y);
                EXPECT_EQ(sample.normal.z, expected.z);
            }
        }
    }
}

TEST(TerrainSurface, NeverNaN) {
    const HeightMap map = random16();
    const float huge = std::numeric_limits<float>::max();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float points[][2] = {
        {-1000.0f, 0.0f}, {1000.0f, 1000.0f}, {huge, -huge}, {-huge, huge}, {inf, 0.0f}, {0.0f, -inf}, {nan, 0.0f}, {0.0f, nan}, {nan, nan}};

    SurfaceView noMap;
    noMap.size = SIZE;
    noMap.maxHeight = MAX_HEIGHT;
    SurfaceView zeroSize = map.view();
    zeroSize.size = 0.0f;
    const SurfaceView views[] = {map.view(), noMap, zeroSize};

    for (const SurfaceView& view : views) {
        for (const auto& point : points) {
            const SurfaceSample sample = sampleSurface(view, point[0], point[1]);
            EXPECT_FALSE(std::isnan(sample.height)) << point[0] << ", " << point[1];
            EXPECT_FALSE(std::isnan(sample.normal.x));
            EXPECT_FALSE(std::isnan(sample.normal.y));
            EXPECT_FALSE(std::isnan(sample.normal.z));
            EXPECT_FALSE(std::isnan(sampleHeight(view, point[0], point[1])));
        }
    }

    // Outside clamps to the edge
    const SurfaceSample outside = sampleSurface(map.view(), 1000.0f, 1000.0f);
    EXPECT_FALSE(outside.inside);
    EXPECT_NEAR(outside.height, map.texel(map.width - 1, map.height - 1) * MAX_HEIGHT, 1e-3f);
    EXPECT_TRUE(sampleSurface(map.view(), 0.0f, 0.0f).inside);

    // No map: height 0, up, present false
    const SurfaceSample none = sampleSurface(noMap, 3.0f, 3.0f);
    EXPECT_EQ(none.height, 0.0f);
    EXPECT_EQ(none.normal.y, 1.0f);
    EXPECT_FALSE(none.present);
    EXPECT_TRUE(none.inside);

    const SurfaceSample zero = sampleSurface(zeroSize, 0.0f, 0.0f);
    EXPECT_FALSE(zero.present);
    EXPECT_FALSE(zero.inside);
    EXPECT_FALSE(zero.hole);
}

TEST(TerrainSurface, ExactAndPresentFlags) {
    const HeightMap map = random16();
    const SurfaceSample sample = sampleSurface(map.view(), 1.0f, 1.0f);
    EXPECT_TRUE(sample.present);
    EXPECT_TRUE(sample.exact) << "true on a single map; T4 returns false from an overview";

    TerrainScope scope(map, "t1-surface-flags");
    const SurfaceSample queried = querySurface(scope.terrain, 1.0f, 1.0f);
    EXPECT_TRUE(queried.present);
    EXPECT_TRUE(queried.exact);
    EXPECT_FALSE(queried.hole);

    TerrainComponent empty;
    const SurfaceSample none = querySurface(empty, 1.0f, 1.0f);
    EXPECT_FALSE(none.present);
    EXPECT_EQ(none.height, 0.0f);
    EXPECT_EQ(none.normal.y, 1.0f);
}

TEST(TerrainSurface, RaycastSkipsHolesOnRequest) {
    // Flat ground with a hole texel in the middle
    HeightMap map;
    map.width = 4;
    map.height = 4;
    map.bytesPerChannel = 1;
    map.pixels.assign(16, 128);
    SurfaceView view = map.view();

    uint8_t holes[16];
    std::memset(holes, 255, sizeof(holes));
    holes[1 * 4 + 1] = 0;   // x and z both in -25..0
    view.holes = HoleMapView{holes, 4, 4};

    const float ground = 128.0f / 255.0f * MAX_HEIGHT;
    Vector3 point;
    float t = 0.0f;
    // Straight down into the hole texel
    const Vector3 origin(-12.0f, MAX_HEIGHT + 10.0f, -12.0f);
    const Vector3 down(0.0f, -(MAX_HEIGHT + 20.0f), 0.0f);
    ASSERT_TRUE(raycastSurface(view, origin, down, 1.0f, false, point, t)) << "a brush ray still lands, so a hole can be filled";
    EXPECT_NEAR(point.y, ground, 1e-3f);
    EXPECT_FALSE(raycastSurface(view, origin, down, 1.0f, true, point, t)) << "a ray down a hole misses this terrain";

    // Beside the hole both find the ground
    const Vector3 beside(30.0f, MAX_HEIGHT + 10.0f, 30.0f);
    ASSERT_TRUE(raycastSurface(view, beside, down, 1.0f, true, point, t));
    EXPECT_NEAR(point.y, ground, 1e-3f);
    ASSERT_TRUE(raycastSurface(view, beside, down, 1.0f, false, point, t));
    EXPECT_NEAR(point.y, ground, 1e-3f);

    // A long slanted ray through the hole goes on, and what it finds beyond is what it hit: it comes
    // down on the hole's far side, solid, without stopping in it
    const Vector3 from(-20.0f, 12.0f, -12.0f);
    const Vector3 across(25.0f, -4.5f, 0.0f);
    float skippedT = 0.0f;
    Vector3 skippedPoint;
    if (raycastSurface(view, from, across, 2.0f, true, skippedPoint, skippedT)) {
        EXPECT_GE(skippedPoint.x, 0.0f) << "whatever it hit is past the hole";
    }
    float landedT = 0.0f;
    Vector3 landedPoint;
    ASSERT_TRUE(raycastSurface(view, from, across, 2.0f, false, landedPoint, landedT));
    EXPECT_LT(landedPoint.x, 0.0f) << "a brush ray lands inside the hole";
}

TEST(TerrainSurface, QueryInterfaceAndViewAgree) {
    const HeightMap map = random16();
    TerrainScope scope(map, "t1-surface-agree");

    const SurfaceView view = map.view();
    SurfaceViewQuery viewQuery(view);
    TerrainComponentQuery componentQuery(scope.terrain);
    TerrainSurfaceSnapshot snapshot;
    snapshotTerrainSurface(scope.terrain, snapshot);
    SurfaceViewQuery snapshotQuery(snapshot.view);

    const TerrainSurfaceQuery* queries[] = {&viewQuery, &componentQuery, &snapshotQuery};

    Pcg32 random(2026);
    for (int i = 0; i < 10000; i++) {
        const float x = (random.nextFloat() - 0.5f) * SIZE * 1.1f;
        const float z = (random.nextFloat() - 0.5f) * SIZE * 1.1f;
        const SurfaceSample expected = sampleSurface(view, x, z);
        for (const TerrainSurfaceQuery* query : queries) {
            const SurfaceSample sample = query->sample(x, z);
            ASSERT_EQ(sample.height, expected.height);
            ASSERT_EQ(sample.normal.x, expected.normal.x);
            ASSERT_EQ(sample.normal.z, expected.normal.z);
            ASSERT_EQ(sample.inside, expected.inside);
            ASSERT_EQ(query->heightAt(x, z), expected.height);
        }
        float height = 0.0f;
        Vector3 normal;
        MeshSystem::sampleTerrainSurface(scope.terrain, x, z, height, normal);
        ASSERT_EQ(height, expected.height) << "the forwarder";
        ASSERT_EQ(normal.y, expected.normal.y);
    }

    // The copy stays what it was when the map changes under it (T5's jobs)
    scope.terrain.heightMap.getData();
    unsigned char* live = static_cast<unsigned char*>(scope.terrain.heightMap.getData().getData());
    const float copied = snapshotQuery.heightAt(0.0f, 0.0f);
    std::memset(live, 0, map.pixels.size());
    EXPECT_EQ(snapshotQuery.heightAt(0.0f, 0.0f), copied);
    EXPECT_EQ(componentQuery.heightAt(0.0f, 0.0f), 0.0f);
}

TEST(TerrainSurface, ForwardersAreTheSameFunctions) {
    const HeightMap map = random16();
    for (float x : {-60.0f, -10.0f, 0.0f, 10.0f, 60.0f}) {
        EXPECT_EQ(MeshSystem::terrainTexelCoord(x, SIZE, map.width), surfaceTexelCoord(x, SIZE, map.width));
        const float tx = surfaceTexelCoord(x, SIZE, map.width);
        EXPECT_EQ(MeshSystem::sampleTerrainHeightTexels(map.pixels.data(), map.width, map.height, map.channels, map.bytesPerChannel, tx, tx),
                  sampleHeightTexels(map.pixels.data(), map.width, map.height, map.channels, map.bytesPerChannel, tx, tx));
        EXPECT_EQ(MeshSystem::sampleTerrainHeight(map.pixels.data(), map.width, map.height, map.channels, map.bytesPerChannel, SIZE, x, x),
                  sampleHeightLocal(map.pixels.data(), map.width, map.height, map.channels, map.bytesPerChannel, SIZE, x, x));
    }
}

TEST(TerrainSurface, RaycastForwarderMatchesTheView) {
    const HeightMap map = random16();
    TerrainScope scope(map, "t1-surface-ray");
    Scene scene;
    std::shared_ptr<MeshSystem> meshSystem = scene.getSystem<MeshSystem>();
    Transform transform;
    const SurfaceView view = map.view();

    for (float z = -40.0f; z <= 40.0f; z += 17.0f) {
        for (float x = -40.0f; x <= 40.0f; x += 13.0f) {
            Vector3 world;
            const Ray ray(Vector3(x, MAX_HEIGHT + 10.0f, z), Vector3(0.0f, -(MAX_HEIGHT + 20.0f), 0.0f));
            ASSERT_TRUE(meshSystem->raycastTerrainSurface(ray, scope.terrain, transform, world));
            Vector3 local;
            float t = 0.0f;
            ASSERT_TRUE(raycastSurface(view, ray.getOrigin(), ray.getDirection(), 1.0f, false, local, t));
            EXPECT_FLOAT_EQ(world.x, local.x);
            EXPECT_FLOAT_EQ(world.y, local.y);
            EXPECT_FLOAT_EQ(world.z, local.z);
            EXPECT_NEAR(world.y, sampleHeight(view, x, z), 2e-3f);
        }
    }
}
