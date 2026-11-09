// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "cook/TerrainWorldCooker.h"

#include "io/AtomicFile.h"
#include "subsystem/TerrainWorldRuntime.h"
#include "terrain/world/TerrainTileFormat.h"
#include "terrain/world/TerrainWorldData.h"
#include "terrainworld/TerrainWorldFiles.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::terraincore;

namespace fs = std::filesystem;

namespace {

    std::shared_ptr<WorldTile> loadCookTile(const fs::path& folder, TileCoord tile) {
        std::ifstream file(folder / terrainWorldTileName(tile), std::ios::binary);
        if (!file) return nullptr;
        std::ostringstream text;
        text << file.rdbuf();
        const std::string bytes = text.str();
        auto data = std::make_shared<WorldTile>();
        if (decodeTile(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()), terrainWorldDecodeCodec(), *data) !=
            TileError::None) {
            return nullptr;
        }
        return data;
    }

    bool writeCooked(const fs::path& path, const std::vector<uint8_t>& bytes, std::string& error) {
        io::AtomicWriteOptions options;
        options.durability = io::Durability::Cache;
        const io::WriteResult result = io::writeFileAtomic(path, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), options);
        if (!result.ok()) {
            error = result.message;
            return false;
        }
        return true;
    }

}

bool TerrainWorldCooker::cook(const fs::path& source, const fs::path& destination, bool web, TerrainWorldCookReport& report, std::string& error) {
    // a report says what this cook did, not the sum of the cooks it was given to
    report = TerrainWorldCookReport();
    WorldManifest manifest;
    if (!readWorldManifest(source, manifest, error)) return false;
    const WorldLattice& lattice = manifest.lattice;
    if (web && lattice.tileQuads > WEB_MAX_TILE_QUADS) {
        error = "a Terrain World on the web takes tile_quads up to " + std::to_string(WEB_MAX_TILE_QUADS) + "; " + source.filename().string() +
                " has " + std::to_string(lattice.tileQuads);
        return false;
    }

    std::map<uint32_t, bool> present;
    for (const WorldTileInfo& info : manifest.tiles) present[info.coord.key()] = true;
    const int n = lattice.tileQuads;

    TileEncodeOptions options;
    options.level = 19;
    options.keepUnknown = false;
    const TileCodec codec = terrainWorldEditorCodec();
    TileCoord low, high;
    bool any = false;
    std::vector<std::shared_ptr<WorldTile>> overviewPatches;
    for (WorldTileInfo& info : manifest.tiles) {
        const TileCoord tile = info.coord;
        // the tile and its eight neighbours: every holder of each texel of its page
        std::shared_ptr<WorldTile> near[3][3];
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                const TileCoord other(tile.x + dx, tile.z + dz);
                if (other.addressable() && present.count(other.key())) near[dz + 1][dx + 1] = loadCookTile(source, other);
            }
        }
        std::shared_ptr<WorldTile> data = near[1][1];
        if (!data) {
            error = "tile (" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + ") of " + source.filename().string() + " could not be read";
            return false;
        }
        // R51: each texel takes its lowest (x, z) holder's sample, so both sides of a seam agree
        int repairs = 0;
        for (int j = 0; j < n + 3; j++) {
            for (int i = 0; i < n + 3; i++) {
                const int sx = tile.x * n + i - 1, sz = tile.z * n + j - 1;
                const uint16_t own = data->sample(i, j);
                bool found = false;
                uint16_t winner = own;
                for (int dx = -1; dx <= 1 && !found; dx++) {
                    for (int dz = -1; dz <= 1 && !found; dz++) {
                        const std::shared_ptr<WorldTile>& other = near[dz + 1][dx + 1];
                        if (!other) continue;
                        const int ox = tile.x + dx, oz = tile.z + dz;
                        const int lx = sx - ox * n, lz = sz - oz * n;
                        if (lx < 0 || lz < 0 || lx > n || lz > n) continue;
                        winner = other->sample(lx + 1, lz + 1);
                        found = true;
                    }
                }
                if (found && winner != own) {
                    data->setSample(i, j, winner);
                    if (i >= 1 && i <= n + 1 && j >= 1 && j <= n + 1) repairs++;
                }
            }
        }
        if (repairs > 0) {
            report.seamRepairs += repairs;
            report.warnings.push_back("tile (" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + "): " + std::to_string(repairs) +
                                      " border samples repaired in the build");
        }
        buildMinMax(*data, lattice);
        buildOverviewHeight(*data, lattice);
        buildSkirtDepth(*data, lattice);
        const TileMinMax whole = tileNodeMinMax(*data, lattice, lattice.tileLevel(), 0, 0);
        info.minHeight = whole.min;
        info.maxHeight = whole.max;
        manifest.skirtDepth = std::max(manifest.skirtDepth, data->skirtDepth);

        std::vector<uint8_t> bytes;
        if (!encodeTile(*data, codec, options, bytes, &error)) return false;
        if (!writeCooked(destination / terrainWorldTileName(tile), bytes, error)) return false;
        report.compressedBytes += bytes.size();
        report.tiles++;

        // R27: T5's trees of the tile ship beside it, as they are (world.bin lists them)
        if (info.trees) {
            std::ifstream file(source / terrainWorldTreeName(tile), std::ios::binary);
            std::ostringstream text;
            text << file.rdbuf();
            if (!file) {
                error = terrainWorldTreeName(tile) + " of " + source.filename().string() + " could not be read";
                return false;
            }
            const std::string treeBytes = text.str();
            if (!writeCooked(destination / terrainWorldTreeName(tile), std::vector<uint8_t>(treeBytes.begin(), treeBytes.end()), error)) return false;
            report.compressedBytes += treeBytes.size();
        }

        auto patch = std::make_shared<WorldTile>();
        patch->coord = tile;
        patch->overviewHeight = data->overviewHeight;
        patch->overviewColour = data->overviewColour;
        patch->skirtDepth = data->skirtDepth;
        overviewPatches.push_back(std::move(patch));
        low = any ? TileCoord(std::min(low.x, tile.x), std::min(low.z, tile.z)) : tile;
        high = any ? TileCoord(std::max(high.x, tile.x), std::max(high.z, tile.z)) : tile;
        any = true;
    }
    if (web && report.compressedBytes > WEB_MAX_BYTES) {
        error = "a Terrain World on the web is at most 96 MiB of compressed tiles; " + source.filename().string() + " has " +
                std::to_string(report.compressedBytes >> 20) + " MiB";
        return false;
    }
    if (web && report.compressedBytes > WEB_WARN_BYTES) {
        report.warnings.push_back(source.filename().string() + " has " + std::to_string(report.compressedBytes >> 20) +
                                  " MiB of tiles: the web keeps the whole pack in memory (R38)");
    }

    WorldOverview overview;
    if (any) {
        if (web && std::max(high.x - low.x + 1, high.z - low.z + 1) * lattice.overviewStride + 1 > WORLD_OVERVIEW_MAX_MOBILE) {
            error = "a Terrain World on the web spans at most " + std::to_string((WORLD_OVERVIEW_MAX_MOBILE - 1) / lattice.overviewStride) + " tiles a side";
            return false;
        }
        initOverview(overview, lattice, low, high, manifest.maps.baseHeight);
        for (const auto& patch : overviewPatches) setOverviewPatch(overview, *patch);
        std::vector<uint8_t> bytes;
        if (!encodeOverview(overview, lattice, codec, 19, bytes, &error)) return false;
        if (!writeCooked(destination / "overview.jtile", bytes, error)) return false;
    }
    std::vector<uint8_t> worldBin;
    encodeWorldBin(manifest, worldBin);
    return writeCooked(destination / "world.bin", worldBin, error);
}
