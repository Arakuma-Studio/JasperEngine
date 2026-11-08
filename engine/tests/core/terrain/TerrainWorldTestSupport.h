// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_WORLD_TEST_SUPPORT_H
#define TERRAIN_WORLD_TEST_SUPPORT_H

// T4's pure tests: worlds in memory, the fixture's heights (a sum of integer sines, so no noise
// library and the same bytes everywhere) and a zstd codec.

#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/TiledMapStore.h"
#include "terrain/world/WorldLattice.h"

#include "zstd.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace terrainworldtest {

    using namespace jasperengine::terrain;

    inline TileCodec zstdCodec() {
        TileCodec codec;
        codec.compress = [](std::span<const uint8_t> in, int level, std::vector<uint8_t>& out) {
            out.resize(ZSTD_compressBound(in.size()));
            const size_t written = ZSTD_compress(out.data(), out.size(), in.data(), in.size(), level);
            if (ZSTD_isError(written)) return false;
            out.resize(written);
            return true;
        };
        codec.decompress = [](std::span<const uint8_t> in, std::span<uint8_t> out) {
            const size_t written = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
            return !ZSTD_isError(written) && written == out.size();
        };
        return codec;
    }

    // The fixture's surface at a global sample, 16-bit: integer sines of the sample coordinate
    inline uint16_t fixtureSample(int sx, int sz) {
        const double x = static_cast<double>(sx), z = static_cast<double>(sz);
        const double value = 0.5 + 0.18 * std::sin(x * 0.011 + 1.0) * std::cos(z * 0.013) + 0.12 * std::sin((x + z) * 0.031) +
                             0.04 * std::sin(x * 0.17) * std::sin(z * 0.19);
        const long quantized = std::lround(value * 65535.0);
        return static_cast<uint16_t>(quantized < 0 ? 0 : (quantized > 65535 ? 65535 : quantized));
    }

    inline WorldLattice smallLattice(int tileQuads = 64, float spacing = 1.0f) {
        WorldLattice lattice;
        lattice.tileQuads = tileQuads;
        lattice.spacing = spacing;
        return lattice;
    }

    // A tile holding the fixture, gutters included (as if every neighbour were present)
    inline std::shared_ptr<TileData> fixtureTile(const WorldLattice& lattice, TileCoord coord) {
        auto tile = std::make_shared<TileData>();
        initTile(*tile, lattice, coord, 0);
        const int n = lattice.tileQuads;
        for (int j = 0; j < n + 3; j++) {
            for (int i = 0; i < n + 3; i++) tile->setSample(i, j, fixtureSample(coord.x * n + i - 1, coord.z * n + j - 1));
        }
        buildMinMax(*tile, lattice);
        buildOverviewHeight(*tile, lattice);
        buildSkirtDepth(*tile, lattice);
        return tile;
    }

    // A rectangle of a store's global height texels, read as samples
    inline std::vector<uint16_t> readSamples(const TiledMapStore& store, TexelRect rect) {
        std::vector<uint8_t> bytes(static_cast<size_t>(rect.area()) * 2);
        std::vector<uint16_t> out;
        if (!store.readBytes(MapId{MapKind::Height, 0}, rect, bytes)) return out;
        out.resize(static_cast<size_t>(rect.area()));
        for (size_t i = 0; i < out.size(); i++) out[i] = static_cast<uint16_t>(bytes[i * 2] | (bytes[i * 2 + 1] << 8));
        return out;
    }

}

#endif // TERRAIN_WORLD_TEST_SUPPORT_H
