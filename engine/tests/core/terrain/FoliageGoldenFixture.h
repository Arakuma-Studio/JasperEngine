// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TESTS_FOLIAGE_GOLDEN_FIXTURE_H
#define TESTS_FOLIAGE_GOLDEN_FIXTURE_H

// T5 AC1's fixture: the bytes and layers whose placement hash is pinned, on the native core suite
// and in wasm (GATE=foliage).

#include "terrain/FoliageScatter.h"
#include "terrain/TerrainDeterministic.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace foliagegolden {

    using namespace jasperengine;
    using namespace jasperengine::terrain;

    constexpr int FIXTURE_MAP = 256;
    constexpr float FIXTURE_SIZE = 256.0f;
    constexpr float FIXTURE_MAX_HEIGHT = 40.0f;

    // 16-bit hash-fractal heights and an 8-bit density field, the same bytes on every target
    struct ScatterFixture {
        std::vector<uint8_t> height;
        std::vector<uint8_t> density;

        ScatterFixture(): height(static_cast<size_t>(FIXTURE_MAP) * FIXTURE_MAP * 2), density(static_cast<size_t>(FIXTURE_MAP) * FIXTURE_MAP) {
            for (int y = 0; y < FIXTURE_MAP; y++) {
                for (int x = 0; x < FIXTURE_MAP; x++) {
                    float value = 0.0f;
                    float amplitude = 0.5f;
                    float frequency = 1.0f / 64.0f;
                    for (int octave = 0; octave < 4; octave++) {
                        value += amplitude * foliageValueNoise(static_cast<float>(x) * frequency, static_cast<float>(y) * frequency, 91u + static_cast<uint32_t>(octave));
                        amplitude *= 0.5f;
                        frequency *= 2.0f;
                    }
                    const uint32_t stored = static_cast<uint32_t>(std::floor(detClamp01(value) * 65535.0f + 0.5f));
                    const size_t index = (static_cast<size_t>(y) * FIXTURE_MAP + x) * 2;
                    height[index] = static_cast<uint8_t>(stored & 0xFF);
                    height[index + 1] = static_cast<uint8_t>(stored >> 8);
                    density[static_cast<size_t>(y) * FIXTURE_MAP + x] = static_cast<uint8_t>(40 + detHash01(x / 16, y / 16, 7u) * 215.0f);
                }
            }
        }

        FoliageCellInput input(const FoliageLayerParams& layer, int cellX, int cellZ) const {
            FoliageCellInput cell;
            cell.layer = layer;
            cell.cellX = cellX;
            cell.cellZ = cellZ;
            cell.terrainSize = FIXTURE_SIZE;
            cell.terrainMaxHeight = FIXTURE_MAX_HEIGHT;
            cell.heightMap.texels = height.data();
            cell.heightMap.mapWidth = cell.heightMap.width = FIXTURE_MAP;
            cell.heightMap.mapHeight = cell.heightMap.height = FIXTURE_MAP;
            cell.heightMap.bytesPerChannel = 2;
            cell.densityMap.texels = density.data();
            cell.densityMap.mapWidth = cell.densityMap.width = FIXTURE_MAP;
            cell.densityMap.mapHeight = cell.densityMap.height = FIXTURE_MAP;
            return cell;
        }

        SurfaceView view() const {
            SurfaceView surface;
            surface.pixels = height.data();
            surface.width = surface.height = FIXTURE_MAP;
            surface.channels = 1;
            surface.bytesPerChannel = 2;
            surface.size = FIXTURE_SIZE;
            surface.maxHeight = FIXTURE_MAX_HEIGHT;
            return surface;
        }
    };

    inline const ScatterFixture& fixture() {
        static const ScatterFixture shared;
        return shared;
    }

    // the fixture's four layers: both upright and tilted, as AC1 asks
    inline FoliageLayerParams fixtureLayer(int index) {
        FoliageLayerParams layer;
        switch (index) {
            case 0:
                layer.seed = 11; layer.density = 4.0f; layer.maxSlope = 35.0f;
                break;
            case 1:
                layer.seed = 22; layer.density = 2.0f; layer.alignToNormal = 1.0f; layer.maxSlope = 60.0f; layer.rotationJitter = 0.5f;
                break;
            case 2:
                layer.seed = 33; layer.density = 1.0f; layer.alignToNormal = 0.35f; layer.minHeightRatio = 0.7f; layer.maxHeightRatio = 1.4f; layer.windBend = 128;
                break;
            default:
                layer.seed = 44; layer.density = 0.5f; layer.minSlope = 2.0f; layer.maxSlope = 80.0f; layer.minHeight = 0.1f; layer.maxHeight = 0.9f;
                layer.healthyColor[0] = 90; layer.healthyColor[1] = 200; layer.healthyColor[2] = 60;
                layer.dryColor[0] = 200; layer.dryColor[1] = 180; layer.dryColor[2] = 80;
                layer.noiseSpread = 0.05f;
                break;
        }
        return layer;
    }

    inline uint64_t hashFixture(int firstCell = -16, int lastCell = 15) {
        uint64_t hash = foliageHash({});
        std::vector<FoliageInstance> instances;
        for (int layer = 0; layer < 4; layer++) {
            for (int z = firstCell; z <= lastCell; z++) {
                for (int x = firstCell; x <= lastCell; x++) {
                    instances.clear();
                    scatterFoliageCell(fixture().input(fixtureLayer(layer), x, z), instances);
                    hash = foliageHash(instances, hash);
                }
            }
        }
        return hash;
    }

}

#endif // TESTS_FOLIAGE_GOLDEN_FIXTURE_H
