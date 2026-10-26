// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Terrain heights on the CPU (MeshSystem's height queries, terrain ray casts, the Jolt
// heightfield samples, the Terrain window's brush) against the surface the terrain vertex
// shader draws: includes/terrain_vs.glsl getHeight() is a LINEAR, CLAMP_TO_EDGE texture()
// lookup at uv = (local.xz + size / 2) / size, so texel i sits at (i + 0.5) / width of the
// terrain. Reading the map with texels at i / (width - 1) instead put the collider, the
// cursor and the brush up to half a texel off the drawn slopes.

#include <gtest/gtest.h>

#include "Scene.h"
#include "component/TerrainComponent.h"
#include "component/Transform.h"
#include "math/Ray.h"
#include "pool/TextureDataPool.h"
#include "subsystem/MeshSystem.h"
#include "texture/TextureData.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

    struct HeightMap {
        std::string name;
        int width = 0;
        int height = 0;
        int channels = 1;
        ColorFormat format = ColorFormat::RED;
        std::vector<unsigned char> pixels;

        int bytesPerChannel() const {
            return format == ColorFormat::RED16 ? 2 : 1;
        }

        // the normalized value the GPU reads from the R channel
        float texel(int x, int y) const {
            const size_t index = (static_cast<size_t>(y) * width + x) * channels * bytesPerChannel();
            if (format == ColorFormat::RED16) {
                return static_cast<float>(pixels[index] | (pixels[index + 1] << 8)) / 65535.0f;
            }
            return static_cast<float>(pixels[index]) / 255.0f;
        }
    };

    // GLSL texture() with LINEAR filtering and CLAMP_TO_EDGE, as the GL/Vulkan specifications
    // define it: i0 = floor(u * width - 0.5), weight = fract(u * width - 0.5), and both
    // texel indices clamped to the edge
    float shaderTexture(const HeightMap& map, float u, float v) {
        const float tu = u * static_cast<float>(map.width) - 0.5f;
        const float tv = v * static_cast<float>(map.height) - 0.5f;
        const int i0 = static_cast<int>(std::floor(tu));
        const int j0 = static_cast<int>(std::floor(tv));
        const float a = tu - std::floor(tu);
        const float b = tv - std::floor(tv);
        auto at = [&](int i, int j) {
            return map.texel(std::clamp(i, 0, map.width - 1), std::clamp(j, 0, map.height - 1));
        };
        return (1.0f - a) * (1.0f - b) * at(i0, j0) + a * (1.0f - b) * at(i0 + 1, j0) +
               (1.0f - a) * b * at(i0, j0 + 1) + a * b * at(i0 + 1, j0 + 1);
    }

    // terrain_vs.glsl getHeight()
    float shaderHeight(const HeightMap& map, float terrainSize, float maxHeight, float x, float z) {
        return shaderTexture(map, (x + terrainSize / 2.0f) / terrainSize, (z + terrainSize / 2.0f) / terrainSize) * maxHeight;
    }

    HeightMap ramp8() {
        // a slope along x, and a step along z
        HeightMap map{"w08-terrain-ramp8", 5, 4, 1, ColorFormat::RED, {}};
        for (int y = 0; y < map.height; y++) {
            for (int x = 0; x < map.width; x++) {
                map.pixels.push_back(static_cast<unsigned char>(x * 60 + (y >= 2 ? 15 : 0)));
            }
        }
        return map;
    }

    HeightMap random16() {
        HeightMap map{"w08-terrain-random16", 17, 9, 1, ColorFormat::RED16, {}};
        std::mt19937 rng(1234);
        std::uniform_int_distribution<int> value(0, 65535);
        for (int i = 0; i < map.width * map.height; i++) {
            const int v = value(rng);
            map.pixels.push_back(static_cast<unsigned char>(v & 0xFF));
            map.pixels.push_back(static_cast<unsigned char>(v >> 8));
        }
        return map;
    }

    HeightMap rgba8() {
        // a legacy RGBA heightmap: only R is the height, the other channels must not leak in
        HeightMap map{"w08-terrain-rgba8", 6, 6, 4, ColorFormat::RGBA, {}};
        std::mt19937 rng(99);
        std::uniform_int_distribution<int> value(0, 255);
        for (int i = 0; i < map.width * map.height; i++) {
            map.pixels.push_back(static_cast<unsigned char>(value(rng)));
            map.pixels.push_back(255);
            map.pixels.push_back(0);
            map.pixels.push_back(static_cast<unsigned char>(value(rng)));
        }
        return map;
    }

    HeightMap flat8() {
        HeightMap map{"w08-terrain-flat8", 8, 8, 1, ColorFormat::RED, {}};
        map.pixels.assign(static_cast<size_t>(map.width) * map.height, 128);
        return map;
    }

    std::vector<HeightMap> heightMaps() {
        return {ramp8(), random16(), rgba8(), flat8()};
    }

    float cpuHeight(const HeightMap& map, float terrainSize, float maxHeight, float x, float z) {
        return MeshSystem::sampleTerrainHeight(map.pixels.data(), map.width, map.height, map.channels, map.bytesPerChannel(),
                                               terrainSize, x, z) * maxHeight;
    }

    // positions across the terrain, its edges and a little beyond them
    std::vector<float> axisPositions(float terrainSize, int count, float overshoot) {
        std::vector<float> positions;
        const float half = terrainSize * 0.5f * overshoot;
        for (int i = 0; i < count; i++) {
            positions.push_back(-half + 2.0f * half * static_cast<float>(i) / static_cast<float>(count - 1));
        }
        return positions;
    }

    // The texture data pool hands an id's first pixels to every later setData of that id, so
    // each map gets its own id, out of the pool again when the test ends (the pixels go too)
    struct TerrainMapScope {
        std::string id;

        TerrainMapScope(TerrainComponent& terrain, HeightMap& map, float terrainSize, float maxHeight) {
            static int counter = 0;
            id = map.name + "-" + std::to_string(counter++);
            terrain.terrainSize = terrainSize;
            terrain.maxHeight = maxHeight;
            const unsigned int size = static_cast<unsigned int>(map.pixels.size());
            terrain.heightMap.setData(id, TextureData(map.width, map.height, size, map.format, map.channels, map.pixels.data()));
        }

        ~TerrainMapScope() {
            TextureDataPool::remove(id);
        }
    };

}

TEST(TerrainHeight, MatchesTheTerrainShader) {
    const float terrainSize = 100.0f;
    const float maxHeight = 20.0f;
    for (const HeightMap& map : heightMaps()) {
        SCOPED_TRACE(map.name);
        for (float z : axisPositions(terrainSize, 41, 1.1f)) {
            for (float x : axisPositions(terrainSize, 53, 1.1f)) {
                EXPECT_NEAR(cpuHeight(map, terrainSize, maxHeight, x, z), shaderHeight(map, terrainSize, maxHeight, x, z), 1e-4f)
                    << "at " << x << ", " << z;
            }
        }
    }
}

TEST(TerrainHeight, TexelCentresCarryTheirTexel) {
    // the old (width - 1) spacing read texel i at i / (width - 1): between texels on all
    // but the edges, so a centre read a blend of its neighbours
    const HeightMap map = ramp8();
    const float terrainSize = 64.0f;
    for (int y = 0; y < map.height; y++) {
        for (int x = 0; x < map.width; x++) {
            const float localX = (static_cast<float>(x) + 0.5f) / static_cast<float>(map.width) * terrainSize - terrainSize * 0.5f;
            const float localZ = (static_cast<float>(y) + 0.5f) / static_cast<float>(map.height) * terrainSize - terrainSize * 0.5f;
            EXPECT_NEAR(cpuHeight(map, terrainSize, 1.0f, localX, localZ), map.texel(x, y), 1e-5f) << x << ", " << y;
        }
    }
    // the terrain edges hold the edge texels (clamp to edge)
    EXPECT_NEAR(cpuHeight(map, terrainSize, 1.0f, -terrainSize * 0.5f, -terrainSize * 0.5f), map.texel(0, 0), 1e-5f);
    EXPECT_NEAR(cpuHeight(map, terrainSize, 1.0f, terrainSize * 0.5f, terrainSize * 0.5f), map.texel(map.width - 1, map.height - 1), 1e-5f);
    EXPECT_FLOAT_EQ(MeshSystem::terrainTexelCoord(-terrainSize * 0.5f, terrainSize, map.width), -0.5f);
    EXPECT_FLOAT_EQ(MeshSystem::terrainTexelCoord(terrainSize * 0.5f, terrainSize, map.width), static_cast<float>(map.width) - 0.5f);
}

TEST(TerrainHeight, FlatGroundStaysFlat) {
    const HeightMap map = flat8();
    const float expected = 128.0f / 255.0f * 30.0f;
    for (float z : axisPositions(50.0f, 17, 1.2f)) {
        for (float x : axisPositions(50.0f, 17, 1.2f)) {
            EXPECT_FLOAT_EQ(cpuHeight(map, 50.0f, 30.0f, x, z), expected);
        }
    }
}

TEST(TerrainHeight, NoPixelsIsFlatZero) {
    EXPECT_EQ(MeshSystem::sampleTerrainHeight(nullptr, 4, 4, 1, 1, 10.0f, 0.0f, 0.0f), 0.0f);
    const HeightMap map = ramp8();
    EXPECT_EQ(MeshSystem::sampleTerrainHeight(map.pixels.data(), map.width, map.height, map.channels, 1, 0.0f, 0.0f, 0.0f), 0.0f);
}

TEST(TerrainHeight, HeightfieldSamplesSitOnTheSurface) {
    // PhysicsSystem builds the Jolt heightfield from samplesSize samples spaced
    // terrainSize / (samplesSize - 1) apart from -terrainSize / 2: the first and the last
    // sit on the terrain edges, every one on the drawn surface
    const HeightMap map = random16();
    const float terrainSize = 80.0f;
    const float maxHeight = 12.0f;
    for (unsigned int samplesSize : {4u, 16u, 32u}) {
        const float spacing = terrainSize / static_cast<float>(samplesSize - 1);
        EXPECT_FLOAT_EQ(-terrainSize * 0.5f + spacing * static_cast<float>(samplesSize - 1), terrainSize * 0.5f);
        for (unsigned int y = 0; y < samplesSize; y++) {
            for (unsigned int x = 0; x < samplesSize; x++) {
                const float localX = -terrainSize * 0.5f + spacing * static_cast<float>(x);
                const float localZ = -terrainSize * 0.5f + spacing * static_cast<float>(y);
                EXPECT_NEAR(cpuHeight(map, terrainSize, maxHeight, localX, localZ), shaderHeight(map, terrainSize, maxHeight, localX, localZ), 1e-4f);
            }
        }
    }
}

TEST(TerrainHeight, MeshSystemQueriesMatchTheShader) {
    Scene scene;
    std::shared_ptr<MeshSystem> meshSystem = scene.getSystem<MeshSystem>();
    const float terrainSize = 100.0f;
    const float maxHeight = 20.0f;

    for (HeightMap map : heightMaps()) {
        SCOPED_TRACE(map.name);
        TerrainComponent terrain;
        TerrainMapScope mapScope(terrain, map, terrainSize, maxHeight);
        Transform transform;

        for (float z : axisPositions(terrainSize, 13, 0.98f)) {
            for (float x : axisPositions(terrainSize, 15, 0.98f)) {
                const float expected = shaderHeight(map, terrainSize, maxHeight, x, z);

                // sampleTerrainSurface: the foliage scatter, the MCP sample_terrain tool, the brush masks
                float height = -1.0f;
                Vector3 normal;
                meshSystem->sampleTerrainSurface(terrain, x, z, height, normal);
                EXPECT_NEAR(height, expected, 1e-4f) << "sampleTerrainSurface at " << x << ", " << z;
                EXPECT_GT(normal.y, 0.0f);

                // raycastTerrainSurface: the Scene window's terrain picks and the brush cursor
                Vector3 worldPoint;
                const Ray down(Vector3(x, maxHeight + 10.0f, z), Vector3(0.0f, -(maxHeight + 20.0f), 0.0f));
                ASSERT_TRUE(meshSystem->raycastTerrainSurface(down, terrain, transform, worldPoint)) << "at " << x << ", " << z;
                EXPECT_NEAR(worldPoint.x, x, 1e-3f);
                EXPECT_NEAR(worldPoint.z, z, 1e-3f);
                EXPECT_NEAR(worldPoint.y, expected, 2e-3f) << "raycastTerrainSurface at " << x << ", " << z;
            }
        }
    }
}

TEST(TerrainHeight, FlatGroundNormalPointsUp) {
    Scene scene;
    std::shared_ptr<MeshSystem> meshSystem = scene.getSystem<MeshSystem>();
    HeightMap map = flat8();
    TerrainComponent terrain;
    TerrainMapScope mapScope(terrain, map, 40.0f, 10.0f);
    for (float z : axisPositions(40.0f, 9, 1.0f)) {
        for (float x : axisPositions(40.0f, 9, 1.0f)) {
            float height = 0.0f;
            Vector3 normal;
            meshSystem->sampleTerrainSurface(terrain, x, z, height, normal);
            EXPECT_FLOAT_EQ(height, 128.0f / 255.0f * 10.0f);
            EXPECT_FLOAT_EQ(normal.x, 0.0f);
            EXPECT_FLOAT_EQ(normal.y, 1.0f);
            EXPECT_FLOAT_EQ(normal.z, 0.0f);
        }
    }
}
