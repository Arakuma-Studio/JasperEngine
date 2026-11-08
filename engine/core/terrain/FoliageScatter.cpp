// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/FoliageScatter.h"

#include "terrain/TerrainDeterministic.h"
#include "terrain/TerrainSurface.h"
#include "util/Fnv.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::terrain;

unsigned int jasperengine::terrain::hashFoliage(unsigned int seed, unsigned int chunkX, unsigned int chunkZ, unsigned int index) {
    unsigned int hash = (seed + 0x9E3779B9u) * 0x9E3779B1u;
    hash = (hash ^ chunkX) * 0x85EBCA6Bu;
    hash = (hash ^ chunkZ) * 0xC2B2AE35u;
    hash = (hash ^ index) * 0x27D4EB2Fu;
    return hash ^ (hash >> 15);
}

// Mix each draw independently before selecting its low 24 bits.
unsigned int jasperengine::terrain::mixFoliage(unsigned int hash) {
    hash = (hash ^ (hash >> 16)) * 0x7FEB352Du;
    hash = (hash ^ (hash >> 15)) * 0x846CA68Bu;
    return hash ^ (hash >> 16);
}

float jasperengine::terrain::randomFoliage(unsigned int hash) {
    return static_cast<float>(hash & 0xFFFFFFu) / 16777216.0f;
}

// A zero-density cell must attempt no placements.
unsigned int jasperengine::terrain::foliageCellAttempts(const FoliageLayerParams& layer) {
    const float estimate = layer.density * FOLIAGE_DETAIL_CELL_SIZE * FOLIAGE_DETAIL_CELL_SIZE;
    return static_cast<unsigned int>(std::ceil(std::clamp(estimate, 0.0f, static_cast<float>(FOLIAGE_MAX_CELL_ATTEMPTS))));
}

namespace {

    // asin on [-0.5, 0.5] by its Taylor series, within 1e-6
    float foliageAsinSmall(float x) {
        const float x2 = x * x;
        return x * (1.0f + x2 * (1.0f / 6.0f + x2 * (3.0f / 40.0f + x2 * (15.0f / 336.0f + x2 * (105.0f / 3456.0f + x2 * (945.0f / 42240.0f + x2 * (10395.0f / 599040.0f)))))));
    }

    inline float foliageHeightTexel(const FoliageMapPatch& patch, int x, int y) {
        const size_t row = static_cast<size_t>(y - patch.originY);
        const size_t column = static_cast<size_t>(x - patch.originX);
        return heightTexelAt(patch.texels, row * static_cast<size_t>(patch.width) + column, patch.channels, patch.bytesPerChannel);
    }

    // sampleHeightTexels' arithmetic, reading from the window (T5 R7)
    float foliagePatchHeightNormalized(const FoliageMapPatch& patch, float terrainSize, float localX, float localZ) {
        if (!patch.texels || patch.mapWidth <= 0 || patch.mapHeight <= 0 || patch.width <= 0 || patch.height <= 0) {
            return 0.0f;
        }
        const int mapWidth = patch.mapWidth;
        const int mapHeight = patch.mapHeight;
        const float texelX = std::clamp(surfaceTexelCoord(localX, terrainSize, mapWidth), 0.0f, static_cast<float>(mapWidth - 1));
        const float texelY = std::clamp(surfaceTexelCoord(localZ, terrainSize, mapHeight), 0.0f, static_cast<float>(mapHeight - 1));
        const int lowerX = std::clamp(static_cast<int>(std::floor(texelX)), 0, mapWidth - 1);
        const int lowerY = std::clamp(static_cast<int>(std::floor(texelY)), 0, mapHeight - 1);
        const int upperX = std::min(lowerX + 1, mapWidth - 1);
        const int upperY = std::min(lowerY + 1, mapHeight - 1);
        const float blendX = texelX - static_cast<float>(lowerX);
        const float blendY = texelY - static_cast<float>(lowerY);

        const float a = foliageHeightTexel(patch, lowerX, lowerY);
        const float b = foliageHeightTexel(patch, upperX, lowerY);
        const float c = foliageHeightTexel(patch, lowerX, upperY);
        const float d = foliageHeightTexel(patch, upperX, upperY);
        const float lower = a + (b - a) * blendX;
        const float upper = c + (d - c) * blendX;
        return lower + (upper - lower) * blendY;
    }

    float foliagePatchDensity(const FoliageMapPatch& patch, float terrainSize, float localX, float localZ) {
        if (!patch.texels || patch.mapWidth <= 0 || patch.mapHeight <= 0 || patch.width <= 0 || patch.height <= 0) {
            return 0.0f;
        }
        const float half = terrainSize * 0.5f;
        const int texelX = std::clamp(static_cast<int>((localX + half) / terrainSize * patch.mapWidth), 0, patch.mapWidth - 1);
        const int texelZ = std::clamp(static_cast<int>((localZ + half) / terrainSize * patch.mapHeight), 0, patch.mapHeight - 1);
        const size_t row = static_cast<size_t>(texelZ - patch.originY);
        const size_t column = static_cast<size_t>(texelX - patch.originX);
        return patch.texels[(row * patch.width + column) * patch.channels * patch.bytesPerChannel] / 255.0f;
    }

    inline uint8_t foliageMixByte(uint8_t a, uint8_t b, float t) {
        return static_cast<uint8_t>(std::floor(static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t + 0.5f));
    }

}

float jasperengine::terrain::foliageAcos(float cosine) {
    if (!(cosine > -1.0f)) return DET_PI;
    if (cosine >= 1.0f) return 0.0f;
    // acos(c) = 2 asin(sqrt((1 - c) / 2)), and a sine above one half reflects past pi / 2
    const float sine = std::sqrt((1.0f - cosine) * 0.5f);
    if (sine <= 0.5f) {
        return 2.0f * foliageAsinSmall(sine);
    }
    return 2.0f * (DET_PI * 0.5f - 2.0f * foliageAsinSmall(std::sqrt((1.0f - sine) * 0.5f)));
}

float jasperengine::terrain::foliageValueNoise(float x, float z, uint32_t seed) {
    const float floorX = std::floor(x);
    const float floorZ = std::floor(z);
    const int cellX = static_cast<int>(floorX);
    const int cellZ = static_cast<int>(floorZ);
    const float tx = detSmooth(x - floorX);
    const float tz = detSmooth(z - floorZ);
    const float a = detHash01(cellX, cellZ, seed);
    const float b = detHash01(cellX + 1, cellZ, seed);
    const float c = detHash01(cellX, cellZ + 1, seed);
    const float d = detHash01(cellX + 1, cellZ + 1, seed);
    const float lower = a + (b - a) * tx;
    const float upper = c + (d - c) * tx;
    return lower + (upper - lower) * tz;
}

FoliagePatchRect jasperengine::terrain::foliageHeightPatchRect(int cellX, int cellZ, float terrainSize, int mapWidth, int mapHeight) {
    FoliagePatchRect rect;
    if (mapWidth <= 0 || mapHeight <= 0 || !(terrainSize > 0.0f)) return rect;
    const float originX = static_cast<float>(cellX) * FOLIAGE_DETAIL_CELL_SIZE;
    const float originZ = static_cast<float>(cellZ) * FOLIAGE_DETAIL_CELL_SIZE;
    const float delta = terrainSize / static_cast<float>(mapWidth);

    // a texel to spare on each side: the forward difference, and the rounding of a float at the edge
    const auto span = [&](float from, float to, int size, int& first, int& count) {
        const int low = std::clamp(static_cast<int>(std::floor(surfaceTexelCoord(from, terrainSize, size))) - 1, 0, size - 1);
        const int high = std::clamp(static_cast<int>(std::floor(surfaceTexelCoord(to, terrainSize, size))) + 2, 0, size - 1);
        first = low;
        count = high - low + 1;
    };
    span(originX, originX + FOLIAGE_DETAIL_CELL_SIZE + delta, mapWidth, rect.x, rect.width);
    span(originZ, originZ + FOLIAGE_DETAIL_CELL_SIZE + delta, mapHeight, rect.y, rect.height);
    return rect;
}

FoliagePatchRect jasperengine::terrain::foliageDensityPatchRect(int cellX, int cellZ, float terrainSize, int mapWidth, int mapHeight) {
    FoliagePatchRect rect;
    if (mapWidth <= 0 || mapHeight <= 0 || !(terrainSize > 0.0f)) return rect;
    const float half = terrainSize * 0.5f;
    const float originX = static_cast<float>(cellX) * FOLIAGE_DETAIL_CELL_SIZE;
    const float originZ = static_cast<float>(cellZ) * FOLIAGE_DETAIL_CELL_SIZE;
    const auto span = [&](float from, float to, int size, int& first, int& count) {
        const int low = std::clamp(static_cast<int>(std::floor((from + half) / terrainSize * size)) - 1, 0, size - 1);
        const int high = std::clamp(static_cast<int>(std::floor((to + half) / terrainSize * size)) + 1, 0, size - 1);
        first = low;
        count = high - low + 1;
    };
    span(originX, originX + FOLIAGE_DETAIL_CELL_SIZE, mapWidth, rect.x, rect.width);
    span(originZ, originZ + FOLIAGE_DETAIL_CELL_SIZE, mapHeight, rect.y, rect.height);
    return rect;
}

float jasperengine::terrain::foliagePatchHeight(const FoliageMapPatch& patch, float terrainSize, float terrainMaxHeight, float localX, float localZ) {
    return terrainMaxHeight * foliagePatchHeightNormalized(patch, terrainSize, localX, localZ);
}

void jasperengine::terrain::foliagePatchNormal(const FoliageMapPatch& patch, float terrainSize, float terrainMaxHeight, float localX, float localZ,
                                               float height, float normal[3]) {
    const float delta = terrainSize / static_cast<float>(patch.mapWidth);
    const float nx = height - terrainMaxHeight * foliagePatchHeightNormalized(patch, terrainSize, localX + delta, localZ);
    const float nz = height - terrainMaxHeight * foliagePatchHeightNormalized(patch, terrainSize, localX, localZ + delta);
    const float length = std::sqrt(nx * nx + delta * delta + nz * nz);
    if (length > 0.0f) {
        normal[0] = nx / length;
        normal[1] = delta / length;
        normal[2] = nz / length;
    } else {
        normal[0] = 0.0f;
        normal[1] = 1.0f;
        normal[2] = 0.0f;
    }
}

void jasperengine::terrain::foliageOrientation(float yawTurns, float alignToNormal, const float normal[3], float rotation[4]) {
    // half the yaw about +Y, as Quaternion(angle, axis)
    const float halfYaw = yawTurns * DET_PI;
    float x = 0.0f, y = detSin(halfYaw), z = 0.0f, w = detCos(halfYaw);

    if (alignToNormal > 0.0f) {
        // the axis (0, 1, 0) x normal, and the slerp from no tilt: its half angle times the share
        const float axisX = normal[2];
        const float axisZ = -normal[0];
        const float length = std::sqrt(axisX * axisX + axisZ * axisZ);
        if (length > 1.1920929e-07f) {
            const float halfTilt = foliageAcos(normal[1]) * 0.5f * alignToNormal;
            const float sine = detSin(halfTilt);
            const float tx = axisX / length * sine;
            const float tz = axisZ / length * sine;
            const float tw = detCos(halfTilt);
            // tilt * yaw, Hamilton's product
            const float rw = tw * w - tz * z - tx * x;
            const float rx = tw * x + tx * w - tz * y;
            const float ry = tw * y + tz * x - tx * z;
            const float rz = tw * z + tz * w + tx * y;
            x = rx;
            y = ry;
            z = rz;
            w = rw;
        }
    }

    rotation[0] = x;
    rotation[1] = y;
    rotation[2] = z;
    rotation[3] = w;
}

void jasperengine::terrain::foliageQuantizeRotation(const float rotation[4], int16_t out[4]) {
    for (int i = 0; i < 4; i++) {
        const float clamped = std::clamp(rotation[i], -1.0f, 1.0f);
        out[i] = static_cast<int16_t>(std::floor(clamped * 32767.0f + 0.5f));
    }
}

void jasperengine::terrain::scatterFoliageCell(const FoliageCellInput& input, std::vector<FoliageInstance>& out, FoliageScatterStats* stats) {
    const FoliageLayerParams& layer = input.layer;
    const float size = input.terrainSize;
    if (!(size > 0.0f)) return;

    const float originX = static_cast<float>(input.cellX) * FOLIAGE_DETAIL_CELL_SIZE;
    const float originZ = static_cast<float>(input.cellZ) * FOLIAGE_DETAIL_CELL_SIZE;
    const float halfSize = size * 0.5f;

    if (originX > halfSize || originZ > halfSize ||
        (originX + FOLIAGE_DETAIL_CELL_SIZE) < -halfSize || (originZ + FOLIAGE_DETAIL_CELL_SIZE) < -halfSize) {
        return;
    }

    const unsigned int attempts = foliageCellAttempts(layer);
    const float expectedInstances = layer.density * FOLIAGE_DETAIL_CELL_SIZE * FOLIAGE_DETAIL_CELL_SIZE;
    const float scaleRange = layer.maxScale - layer.minScale;
    const float ratioRange = layer.maxHeightRatio - layer.minHeightRatio;
    const bool constantColor = std::equal(layer.healthyColor, layer.healthyColor + 4, layer.dryColor);

    // F2: comparing cosines takes no acos, and the range still means degrees. A larger slope has a smaller cosine.
    // detCos(0) lands a hair under 1, and a flat Terrain's normal is exactly (0, 1, 0): no lower bound
    // means no lower bound, as the std::cos of the path this replaced gave
    const float cosineMinSlope = layer.minSlope > 0.0f ? detCos(detRadians(layer.minSlope)) : 2.0f;
    const float cosineMaxSlope = detCos(detRadians(layer.maxSlope));

    const unsigned int cellHashX = static_cast<unsigned int>(input.cellX + input.regionOffsetX);
    const unsigned int cellHashZ = static_cast<unsigned int>(input.cellZ + input.regionOffsetZ);

    for (unsigned int i = 0; i < attempts; i++) {
        const unsigned int hashX = hashFoliage(layer.seed, cellHashX, cellHashZ, i);
        const unsigned int hashZ = mixFoliage(hashX);
        const unsigned int hashAccept = mixFoliage(hashZ);
        const unsigned int hashScale = mixFoliage(hashAccept);
        const unsigned int hashYaw = mixFoliage(hashScale);

        if (stats) stats->attempts++;

        const float localX = originX + randomFoliage(hashX) * FOLIAGE_DETAIL_CELL_SIZE;
        const float localZ = originZ + randomFoliage(hashZ) * FOLIAGE_DETAIL_CELL_SIZE;
        if (localX < -halfSize || localX > halfSize || localZ < -halfSize || localZ > halfSize) {
            continue;
        }

        float density = foliagePatchDensity(input.densityMap, size, localX, localZ);
        // Keep the fractional attempt so sparse layers do not round down to zero.
        if (i + 1 == attempts) {
            density *= std::min(1.0f, expectedInstances - static_cast<float>(i));
        }
        if (randomFoliage(hashAccept) >= density) {
            if (stats) stats->density++;
            continue;
        }

        // T3 R19, R26: at the instance origin, after the density test and before height
        if (isHole(input.holes, (localX + halfSize) / size, (localZ + halfSize) / size)) {
            if (stats) stats->hole++;
            continue;
        }

        const float height = foliagePatchHeight(input.heightMap, size, input.terrainMaxHeight, localX, localZ);
        float normal[3];
        foliagePatchNormal(input.heightMap, size, input.terrainMaxHeight, localX, localZ, height, normal);

        const float normalY = std::clamp(normal[1], -1.0f, 1.0f);
        if (normalY > cosineMinSlope || normalY < cosineMaxSlope) {
            if (stats) stats->slope++;
            continue;
        }

        // A zero maxHeight leaves nothing to normalise against, so everything reads as the base.
        const float normalizedHeight = (input.terrainMaxHeight != 0.0f) ? (height / input.terrainMaxHeight) : 0.0f;
        if (normalizedHeight < layer.minHeight || normalizedHeight > layer.maxHeight) {
            if (stats) stats->height++;
            continue;
        }

        // F2: rank, phase and colour take later draws, so they cannot disturb the five above
        const unsigned int hashRank = mixFoliage(hashYaw);
        const unsigned int hashPhase = mixFoliage(hashRank);
        const unsigned int hashRatio = mixFoliage(hashPhase);
        const unsigned int hashColor = mixFoliage(hashRatio);

        FoliageInstance instance;
        instance.position[0] = localX;
        instance.position[1] = height;
        instance.position[2] = localZ;
        instance.scale = layer.minScale + randomFoliage(hashScale) * scaleRange;

        float rotation[4];
        foliageOrientation(randomFoliage(hashYaw) * layer.rotationJitter, layer.alignToNormal, normal, rotation);
        foliageQuantizeRotation(rotation, instance.rotation);

        if (constantColor) {
            std::copy(layer.healthyColor, layer.healthyColor + 4, instance.color);
        } else {
            const float share = layer.noiseSpread > 0.0f
                ? foliageValueNoise(localX * layer.noiseSpread, localZ * layer.noiseSpread, layer.seed)
                : randomFoliage(hashColor);
            for (int c = 0; c < 4; c++) {
                instance.color[c] = foliageMixByte(layer.dryColor[c], layer.healthyColor[c], share);
            }
        }

        const float ratio = layer.minHeightRatio + randomFoliage(hashRatio) * ratioRange;
        instance.heightRatio = static_cast<uint8_t>(std::clamp(std::floor(ratio * 128.0f + 0.5f), 1.0f, 255.0f));
        instance.windPhase = static_cast<uint8_t>(hashPhase >> 24);
        instance.windBend = layer.windBend;
        instance.densityRank = static_cast<uint8_t>(hashRank >> 24);
        out.push_back(instance);
    }
}

uint64_t jasperengine::terrain::foliageHash(std::span<const FoliageInstance> instances, uint64_t previous) {
    // the struct has no padding (static_assert), and every shipped target is little-endian
    return fnv1a64(instances.data(), instances.size_bytes(), previous);
}
