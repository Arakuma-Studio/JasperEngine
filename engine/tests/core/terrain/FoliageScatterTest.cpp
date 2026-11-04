// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// T5 AC1, AC6, AC12: the placement kernel. The hash constants are the same on every target (R2); a
// changed constant means a changed placement, which needs a new foliageAlgorithmVersion.

#include "util/Angle.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "terrain/FoliageScatter.h"
#include "terrain/TerrainDeterministic.h"
#include "terrain/TerrainSurface.h"

#include "FoliageGoldenFixture.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::terrain;
using namespace foliagegolden;

namespace {


    // Today's appendFoliageCell, verbatim but for reading the fixture's maps (AC6): it is what a
    // scene made before T5 placed
    struct LegacyInstance {
        Vector3 position;
        Vector3 scale;
        Quaternion rotation;
    };

    constexpr float LEGACY_CELL_SIZE = 8.0f;
    constexpr unsigned int LEGACY_MAX_INSTANCES = 8192;

    unsigned int legacyAttempts(const FoliageLayerParams& layer) {
        const float estimate = layer.density * LEGACY_CELL_SIZE * LEGACY_CELL_SIZE;
        return static_cast<unsigned int>(std::ceil(std::clamp(estimate, 0.0f, static_cast<float>(LEGACY_MAX_INSTANCES))));
    }

    unsigned int legacyHash(unsigned int seed, unsigned int chunkX, unsigned int chunkZ, unsigned int index) {
        unsigned int hash = (seed + 0x9E3779B9u) * 0x9E3779B1u;
        hash = (hash ^ chunkX) * 0x85EBCA6Bu;
        hash = (hash ^ chunkZ) * 0xC2B2AE35u;
        hash = (hash ^ index) * 0x27D4EB2Fu;
        return hash ^ (hash >> 15);
    }

    unsigned int legacyMix(unsigned int hash) {
        hash = (hash ^ (hash >> 16)) * 0x7FEB352Du;
        hash = (hash ^ (hash >> 15)) * 0x846CA68Bu;
        return hash ^ (hash >> 16);
    }

    float legacyRandom(unsigned int hash) {
        return static_cast<float>(hash & 0xFFFFFFu) / 16777216.0f;
    }

    float legacyDensity(float localX, float localZ) {
        const float terrainSize = FIXTURE_SIZE;
        const int width = FIXTURE_MAP, height = FIXTURE_MAP;
        const float halfSize = terrainSize * 0.5f;
        const int texelX = std::clamp(static_cast<int>((localX + halfSize) / terrainSize * width), 0, width - 1);
        const int texelZ = std::clamp(static_cast<int>((localZ + halfSize) / terrainSize * height), 0, height - 1);
        return fixture().density[static_cast<size_t>(texelZ) * width + texelX] / 255.0f;
    }

    void legacyAppendCell(const FoliageLayerParams& layer, int cellX, int cellZ, std::vector<LegacyInstance>& instances) {
        const float originX = cellX * LEGACY_CELL_SIZE;
        const float originZ = cellZ * LEGACY_CELL_SIZE;
        const float halfSize = FIXTURE_SIZE * 0.5f;

        if (originX > halfSize || originZ > halfSize ||
            (originX + LEGACY_CELL_SIZE) < -halfSize || (originZ + LEGACY_CELL_SIZE) < -halfSize) {
            return;
        }

        const unsigned int attempts = legacyAttempts(layer);
        const float expectedInstances = layer.density * LEGACY_CELL_SIZE * LEGACY_CELL_SIZE;
        const float scaleRange = layer.maxScale - layer.minScale;
        const SurfaceView surface = fixture().view();

        for (unsigned int i = 0; i < attempts; i++) {
            const unsigned int hashX = legacyHash(layer.seed, static_cast<unsigned int>(cellX), static_cast<unsigned int>(cellZ), i);
            const unsigned int hashZ = legacyMix(hashX);
            const unsigned int hashAccept = legacyMix(hashZ);
            const unsigned int hashScale = legacyMix(hashAccept);
            const unsigned int hashYaw = legacyMix(hashScale);

            const float localX = originX + legacyRandom(hashX) * LEGACY_CELL_SIZE;
            const float localZ = originZ + legacyRandom(hashZ) * LEGACY_CELL_SIZE;
            if (localX < -halfSize || localX > halfSize || localZ < -halfSize || localZ > halfSize) {
                continue;
            }

            float density = legacyDensity(localX, localZ);
            if (i + 1 == attempts) {
                density *= std::min(1.0f, expectedInstances - i);
            }
            if (legacyRandom(hashAccept) >= density) {
                continue;
            }

            const SurfaceSample sample = sampleSurface(surface, localX, localZ);
            const float height = sample.height;
            const Vector3 normal = sample.normal;

            const float slope = Angle::radToDefault(std::acos(std::clamp(normal.y, -1.0f, 1.0f)));
            if (slope < layer.minSlope || slope > layer.maxSlope) {
                continue;
            }

            const float normalizedHeight = (FIXTURE_MAX_HEIGHT != 0.0f) ? (height / FIXTURE_MAX_HEIGHT) : 0.0f;
            if (normalizedHeight < layer.minHeight || normalizedHeight > layer.maxHeight) {
                continue;
            }

            LegacyInstance instance;
            instance.position = Vector3(localX, height, localZ);
            instance.scale = Vector3(layer.minScale + legacyRandom(hashScale) * scaleRange);
            instance.rotation = Quaternion(legacyRandom(hashYaw) * Angle::degToDefault(360.0f) * layer.rotationJitter, Vector3(0.0f, 1.0f, 0.0f));

            if (layer.alignToNormal > 0.0f) {
                const Vector3 axis = Vector3(0.0f, 1.0f, 0.0f).crossProduct(normal);
                if (axis.length() > std::numeric_limits<float>::epsilon()) {
                    const Quaternion tilt(slope, axis.normalized());
                    instance.rotation = Quaternion::slerp(layer.alignToNormal, Quaternion(), tilt) * instance.rotation;
                }
            }

            instances.push_back(instance);
        }
    }

}

TEST(FoliageScatter, GoldenHashOnEveryTarget) {
    const uint64_t hash = hashFixture();
    EXPECT_EQ(hash, 0xd32745bfa2c90c1dull) << "actual 0x" << std::hex << hash;
}

TEST(FoliageScatter, GoldenHashWithSeedChanges) {
    FoliageLayerParams layer = fixtureLayer(1);
    uint64_t hash = foliageHash({});
    std::vector<FoliageInstance> instances;
    for (uint32_t seed = 0; seed < 8; seed++) {
        layer.seed = seed * 7919u;
        instances.clear();
        scatterFoliageCell(fixture().input(layer, 2, -3), instances);
        hash = foliageHash(instances, hash);
    }
    EXPECT_EQ(hash, 0x550905a32769a533ull) << "actual 0x" << std::hex << hash;
}

// A flat Terrain's normal is exactly up: a layer with no Min Slope places on it
TEST(FoliageScatter, FlatGroundPlaces) {
    std::vector<uint8_t> height(static_cast<size_t>(FIXTURE_MAP) * FIXTURE_MAP * 2, 0);
    std::vector<uint8_t> density(static_cast<size_t>(FIXTURE_MAP) * FIXTURE_MAP, 255);
    FoliageCellInput input = fixture().input(FoliageLayerParams{}, 0, 0);
    input.heightMap.texels = height.data();
    input.densityMap.texels = density.data();
    std::vector<FoliageInstance> out;
    scatterFoliageCell(input, out);
    EXPECT_GT(out.size(), 0u);
}

TEST(FoliageScatter, LegacyParity) {
    for (int index = 0; index < 4; index++) {
        const FoliageLayerParams layer = fixtureLayer(index);
        // the legacy path had no ratios or colours; they do not reach position, scale or rotation
        for (int z = -8; z < 8; z++) {
            for (int x = -8; x < 8; x++) {
                std::vector<LegacyInstance> legacy;
                legacyAppendCell(layer, x, z, legacy);
                std::vector<FoliageInstance> now;
                scatterFoliageCell(fixture().input(layer, x, z), now);
                ASSERT_EQ(now.size(), legacy.size()) << "layer " << index << " cell " << x << "," << z;
                for (size_t i = 0; i < now.size(); i++) {
                    EXPECT_EQ(std::memcmp(now[i].position, &legacy[i].position.x, 4), 0);
                    EXPECT_EQ(std::memcmp(now[i].position + 1, &legacy[i].position.y, 4), 0);
                    EXPECT_EQ(std::memcmp(now[i].position + 2, &legacy[i].position.z, 4), 0);
                    EXPECT_EQ(std::memcmp(&now[i].scale, &legacy[i].scale.x, 4), 0);
                    const Quaternion& q = legacy[i].rotation;
                    // a quaternion and its negation are one rotation
                    float dot = (now[i].rotation[0] / 32767.0f) * q.x + (now[i].rotation[1] / 32767.0f) * q.y +
                                (now[i].rotation[2] / 32767.0f) * q.z + (now[i].rotation[3] / 32767.0f) * q.w;
                    const float sign = dot < 0.0f ? -1.0f : 1.0f;
                    EXPECT_NEAR(now[i].rotation[0] / 32767.0f * sign, q.x, 2e-4f);
                    EXPECT_NEAR(now[i].rotation[1] / 32767.0f * sign, q.y, 2e-4f);
                    EXPECT_NEAR(now[i].rotation[2] / 32767.0f * sign, q.z, 2e-4f);
                    EXPECT_NEAR(now[i].rotation[3] / 32767.0f * sign, q.w, 2e-4f);
                }
            }
        }
    }
}

TEST(FoliageScatter, PatchesMatchTheWholeMap) {
    // R7: a job's copied texels give what the whole map gives, at every instance
    const FoliageLayerParams layer = fixtureLayer(2);
    for (int z = -16; z < 16; z += 3) {
        for (int x = -16; x < 16; x += 3) {
            std::vector<FoliageInstance> whole;
            scatterFoliageCell(fixture().input(layer, x, z), whole);

            FoliageCellInput cell = fixture().input(layer, x, z);
            const FoliagePatchRect heightRect = foliageHeightPatchRect(x, z, FIXTURE_SIZE, FIXTURE_MAP, FIXTURE_MAP);
            std::vector<uint8_t> heightBytes(static_cast<size_t>(heightRect.width) * heightRect.height * 2);
            for (int row = 0; row < heightRect.height; row++) {
                std::memcpy(heightBytes.data() + static_cast<size_t>(row) * heightRect.width * 2,
                            fixture().height.data() + (static_cast<size_t>(heightRect.y + row) * FIXTURE_MAP + heightRect.x) * 2,
                            static_cast<size_t>(heightRect.width) * 2);
            }
            cell.heightMap.texels = heightBytes.data();
            cell.heightMap.originX = heightRect.x;
            cell.heightMap.originY = heightRect.y;
            cell.heightMap.width = heightRect.width;
            cell.heightMap.height = heightRect.height;

            const FoliagePatchRect densityRect = foliageDensityPatchRect(x, z, FIXTURE_SIZE, FIXTURE_MAP, FIXTURE_MAP);
            std::vector<uint8_t> densityBytes(static_cast<size_t>(densityRect.width) * densityRect.height);
            for (int row = 0; row < densityRect.height; row++) {
                std::memcpy(densityBytes.data() + static_cast<size_t>(row) * densityRect.width,
                            fixture().density.data() + static_cast<size_t>(densityRect.y + row) * FIXTURE_MAP + densityRect.x,
                            static_cast<size_t>(densityRect.width));
            }
            cell.densityMap.texels = densityBytes.data();
            cell.densityMap.originX = densityRect.x;
            cell.densityMap.originY = densityRect.y;
            cell.densityMap.width = densityRect.width;
            cell.densityMap.height = densityRect.height;

            std::vector<FoliageInstance> patched;
            scatterFoliageCell(cell, patched);
            ASSERT_EQ(patched.size(), whole.size());
            EXPECT_EQ(foliageHash(patched), foliageHash(whole));
        }
    }
}

TEST(FoliageScatter, SurfaceMatchesTheQuery) {
    // R7: the patch's answers against T1's surface, within 1e-4 (S26:467)
    const SurfaceView surface = fixture().view();
    FoliageCellInput cell = fixture().input(FoliageLayerParams{}, 0, 0);
    for (float z = -120.0f; z < 120.0f; z += 7.3f) {
        for (float x = -120.0f; x < 120.0f; x += 5.1f) {
            const SurfaceSample sample = sampleSurface(surface, x, z);
            const float height = foliagePatchHeight(cell.heightMap, FIXTURE_SIZE, FIXTURE_MAX_HEIGHT, x, z);
            EXPECT_NEAR(height, sample.height, 1e-4f);
            float normal[3];
            foliagePatchNormal(cell.heightMap, FIXTURE_SIZE, FIXTURE_MAX_HEIGHT, x, z, height, normal);
            EXPECT_NEAR(normal[0], sample.normal.x, 1e-4f);
            EXPECT_NEAR(normal[1], sample.normal.y, 1e-4f);
            EXPECT_NEAR(normal[2], sample.normal.z, 1e-4f);
        }
    }
}

TEST(FoliageScatter, HolesRejectAfterDensityBeforeHeight) {
    FoliageLayerParams layer = fixtureLayer(0);
    layer.maxSlope = 90.0f;
    FoliageCellInput open = fixture().input(layer, 1, 1);
    std::vector<FoliageInstance> placed;
    scatterFoliageCell(open, placed);
    ASSERT_FALSE(placed.empty());

    std::vector<uint8_t> holes(16 * 16, HOLE_VALUE);
    FoliageCellInput cut = fixture().input(layer, 1, 1);
    cut.holes = HoleMapView{holes.data(), 16, 16};
    std::vector<FoliageInstance> none;
    FoliageScatterStats stats;
    scatterFoliageCell(cut, none, &stats);
    EXPECT_TRUE(none.empty());
    EXPECT_EQ(stats.hole, placed.size());
}

TEST(FoliageScatter, DefaultColoursAreWhite) {
    std::vector<FoliageInstance> placed;
    FoliageLayerParams layer = fixtureLayer(0);
    scatterFoliageCell(fixture().input(layer, 0, 0), placed);
    ASSERT_FALSE(placed.empty());
    for (const FoliageInstance& instance : placed) {
        for (int c = 0; c < 4; c++) EXPECT_EQ(instance.color[c], 255);
        EXPECT_EQ(instance.heightRatio, 128) << "a ratio of 1 stores 128, which is exactly 1";
    }
}

TEST(FoliageScatter, HealthyDryNoiseIsDeterministic) {
    FoliageLayerParams layer = fixtureLayer(3);
    std::vector<FoliageInstance> first, second;
    scatterFoliageCell(fixture().input(layer, 3, 3), first);
    scatterFoliageCell(fixture().input(layer, 3, 3), second);
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(foliageHash(first), foliageHash(second));

    // between the two colours, and more than one value in a large enough area
    std::vector<FoliageInstance> field;
    for (int z = -8; z < 8; z++) {
        for (int x = -8; x < 8; x++) scatterFoliageCell(fixture().input(layer, x, z), field);
    }
    bool differs = false;
    for (const FoliageInstance& instance : field) {
        EXPECT_GE(instance.color[0], 90);
        EXPECT_LE(instance.color[0], 200);
        differs = differs || instance.color[0] != field[0].color[0];
    }
    EXPECT_TRUE(differs);
}

TEST(FoliageScatter, RatiosReachTheInstance) {
    FoliageLayerParams layer = fixtureLayer(2);
    std::vector<FoliageInstance> placed;
    for (int x = -4; x < 4; x++) scatterFoliageCell(fixture().input(layer, x, 0), placed);
    ASSERT_FALSE(placed.empty());
    bool differs = false;
    for (const FoliageInstance& instance : placed) {
        const float ratio = instance.heightRatio / 128.0f;
        EXPECT_GE(ratio, 0.69f);
        EXPECT_LE(ratio, 1.41f);
        differs = differs || instance.heightRatio != placed[0].heightRatio;
        EXPECT_EQ(instance.windBend, 128);
    }
    EXPECT_TRUE(differs);
}

TEST(FoliageScatter, RegionOffsetIsTheCellSeed) {
    // T5 F2: two terrains side by side meet as one would have placed them. The offset moves only the hash.
    FoliageLayerParams layer = fixtureLayer(0);
    FoliageCellInput withOffset = fixture().input(layer, 1, 2);
    withOffset.regionOffsetX = 3;
    withOffset.regionOffsetZ = -1;
    std::vector<FoliageInstance> moved;
    scatterFoliageCell(withOffset, moved);

    for (unsigned int i = 0; i < 16; i++) {
        const unsigned int hashX = hashFoliage(layer.seed, 4u, 1u, i);
        const float x = 8.0f + randomFoliage(hashX) * 8.0f;
        bool found = false;
        for (const FoliageInstance& instance : moved) found = found || instance.position[0] == x;
        // an attempt the density let through is at this very x; others are rejected, never moved
        if (found) SUCCEED();
    }
    std::vector<FoliageInstance> plain;
    scatterFoliageCell(fixture().input(layer, 1, 2), plain);
    EXPECT_NE(foliageHash(moved), foliageHash(plain));
}

TEST(FoliageScatter, VersionFieldIsWritten) {
    const FoliageLayerParams layer;
    EXPECT_EQ(layer.algorithmVersion, 1u);
    EXPECT_EQ(FOLIAGE_ALGORITHM_VERSION, 1u);
}

TEST(FoliageScatter, AcosKernelIsWithinItsBound) {
    for (float c = -1.0f; c <= 1.0f; c += 0.0009f) {
        EXPECT_NEAR(foliageAcos(c), std::acos(c), 1e-5f) << c;
    }
    EXPECT_EQ(foliageAcos(1.0f), 0.0f);
    EXPECT_NEAR(foliageAcos(-1.0f), 3.14159265f, 1e-5f);
}

TEST(FoliageTypes, InstanceIs32BytesAtFixedOffsets) {
    EXPECT_EQ(sizeof(FoliageInstance), 32u);
    EXPECT_EQ(offsetof(FoliageInstance, position), 0u);
    EXPECT_EQ(offsetof(FoliageInstance, scale), 12u);
    EXPECT_EQ(offsetof(FoliageInstance, rotation), 16u);
    EXPECT_EQ(offsetof(FoliageInstance, color), 24u);
    EXPECT_EQ(offsetof(FoliageInstance, heightRatio), 28u);
    EXPECT_EQ(offsetof(FoliageInstance, windPhase), 29u);
    EXPECT_EQ(offsetof(FoliageInstance, windBend), 30u);
    EXPECT_EQ(offsetof(FoliageInstance, densityRank), 31u);
    EXPECT_EQ(sizeof(TreeRecord), 40u);
}
