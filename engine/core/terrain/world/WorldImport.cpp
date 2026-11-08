// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/world/WorldImport.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    // The image rows a row of tiles reads: its N + 1 shared rows and a gutter row either side, so at
    // most N + 3 image rows are held at once
    class ImportBand {
    public:
        ImportBand(const HeightmapRows& source, bool flipZ, int tileQuads)
            : source(source), flipZ(flipZ), rows(static_cast<size_t>(tileQuads) + 3), rowOf(rows.size(), -1) {}

        // the row of import sample row `importRow` (0: the import's first), clamped: the edge repeats
        const std::vector<uint16_t>* row(int importRow, std::string& error) {
            const int clamped = std::min(std::max(importRow, 0), source.height - 1);
            const int wanted = flipZ ? source.height - 1 - clamped : clamped;
            const int count = static_cast<int>(rows.size());
            const size_t slot = static_cast<size_t>(((clamped % count) + count) % count);
            if (rowOf[slot] != wanted) {
                rows[slot].assign(static_cast<size_t>(source.width), 0);
                if (!source.read(wanted, rows[slot]) || rows[slot].size() != static_cast<size_t>(source.width)) {
                    error = "the heightmap's row " + std::to_string(wanted) + " could not be read";
                    return nullptr;
                }
                rowOf[slot] = wanted;
            }
            return &rows[slot];
        }

    private:
        const HeightmapRows& source;
        bool flipZ;
        std::vector<std::vector<uint16_t>> rows;
        std::vector<int> rowOf;
    };

    // The few neighbours a made tile's gutter reads, most recent kept
    class ImportNeighbours {
    public:
        explicit ImportNeighbours(const ImportTarget& target): target(target) {}

        std::shared_ptr<WorldTile> get(TileCoord tile) {
            for (size_t i = 0; i < kept.size(); i++) {
                if (kept[i].first != tile.key()) continue;
                std::rotate(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(i), kept.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                return kept.front().second;
            }
            std::shared_ptr<WorldTile> data = target.read(tile);
            if (!data) return nullptr;
            kept.insert(kept.begin(), {tile.key(), data});
            if (kept.size() > KEPT) kept.pop_back();
            return data;
        }

    private:
        static constexpr size_t KEPT = 4;
        const ImportTarget& target;
        std::vector<std::pair<uint32_t, std::shared_ptr<WorldTile>>> kept;
    };

    uint16_t rawSample(const uint8_t* at, bool bigEndian) {
        return bigEndian ? static_cast<uint16_t>((at[0] << 8) | at[1]) : static_cast<uint16_t>(at[0] | (at[1] << 8));
    }

    bool rawSizeMatches(std::ifstream& file, int width, int height, std::string& error) {
        file.seekg(0, std::ios::end);
        const std::streamoff length = file.tellg();
        file.seekg(0, std::ios::beg);
        if (length != static_cast<std::streamoff>(width) * height * 2) {
            error = "the file holds " + std::to_string(length) + " bytes, not " + std::to_string(width) + " x " + std::to_string(height) + " 16-bit samples";
            return false;
        }
        return true;
    }

}

int jasperengine::terrain::importTilesAcross(int samples, int tileQuads) {
    if (samples < 2 || tileQuads <= 0) return 0;
    // a tile needs its last shared sample: (samples - 1) quads, a partial tile repeating the edge
    return (samples - 1 + tileQuads - 1) / tileQuads;
}

std::vector<TileCoord> jasperengine::terrain::importCoveredTiles(const HeightmapImport& plan, int width, int height) {
    std::vector<TileCoord> tiles;
    const int n = plan.lattice.tileQuads;
    const int across = importTilesAcross(width, n), down = importTilesAcross(height, n);
    for (int z = 0; z < down; z++) {
        for (int x = 0; x < across; x++) tiles.emplace_back(plan.origin.x + x, plan.origin.z + z);
    }
    return tiles;
}

std::vector<TileCoord> jasperengine::terrain::importChangedTiles(const HeightmapImport& plan, int width, int height,
                                                                 const std::function<bool(TileCoord tile)>& present) {
    std::vector<TileCoord> tiles;
    const int n = plan.lattice.tileQuads;
    const int across = importTilesAcross(width, n), down = importTilesAcross(height, n);
    for (int z = -1; z <= down; z++) {
        for (int x = -1; x <= across; x++) {
            const TileCoord tile(plan.origin.x + x, plan.origin.z + z);
            const bool covered = x >= 0 && x < across && z >= 0 && z < down;
            if (covered || (tile.addressable() && present && present(tile))) tiles.push_back(tile);
        }
    }
    return tiles;
}

bool jasperengine::terrain::importHeightmap(const HeightmapRows& source, const HeightmapImport& plan, const ImportTarget& target,
                                            const std::function<bool()>& cancelled, const std::function<void(int done, int total)>& progress,
                                            std::string& error) {
    const WorldLattice& lattice = plan.lattice;
    if (!validateLattice(lattice, &error)) return false;
    if (source.width < 2 || source.height < 2 || !source.read) {
        error = "the heightmap is empty";
        return false;
    }
    if (!target.present || !target.read || !target.write) {
        error = "the import has nowhere to write";
        return false;
    }
    const int n = lattice.tileQuads;
    const int across = importTilesAcross(source.width, n), down = importTilesAcross(source.height, n);
    for (int corner = 0; corner < 4; corner++) {
        const TileCoord tile(plan.origin.x + (corner % 2) * (across - 1), plan.origin.z + (corner / 2) * (down - 1));
        if (!tileWithinLimit(lattice, tile)) {
            error = "tile (" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + ") is past the world's limit of +-16384 m";
            return false;
        }
    }

    // the samples the image sets: every shared sample of the covered tiles (signed world samples)
    const int lowX = plan.origin.x * n, highX = (plan.origin.x + across) * n;
    const int lowZ = plan.origin.z * n, highZ = (plan.origin.z + down) * n;
    auto inImage = [&](int sx, int sz) { return sx >= lowX && sx <= highX && sz >= lowZ && sz <= highZ; };
    // a present tile other than `self` whose shared range holds the sample (outside the image only
    // tiles that were there hold one)
    auto holder = [&](int sx, int sz, TileCoord self, TileCoord& out) {
        const int tx = floorDiv(sx, n), tz = floorDiv(sz, n);
        const int xs[2] = {tx, sx % n == 0 ? tx - 1 : tx};
        const int zs[2] = {tz, sz % n == 0 ? tz - 1 : tz};
        for (int a = 0; a < 2; a++) {
            for (int b = 0; b < 2; b++) {
                const TileCoord tile(xs[b], zs[a]);
                if (tile == self || !tile.addressable() || !target.present(tile)) continue;
                out = tile;
                return true;
            }
        }
        return false;
    };

    ImportBand band(source, plan.flipZ, n);
    ImportNeighbours neighbours(target);
    const std::vector<TileCoord> changed = importChangedTiles(plan, source.width, source.height, target.present);
    const int total = static_cast<int>(changed.size());
    int done = 0;
    for (const TileCoord& coord : changed) {
        if (cancelled && cancelled()) {
            error = "cancelled";
            return false;
        }
        std::shared_ptr<WorldTile> before;
        if (target.present(coord)) {
            before = target.read(coord);
            if (!before) {
                error = "tile (" + std::to_string(coord.x) + ", " + std::to_string(coord.z) + ") could not be read";
                return false;
            }
        }
        WorldTile tile;
        if (before) {
            tile = *before;
        } else {
            initTile(tile, lattice, coord, target.baseHeight);
        }
        // the shared samples first: the image's where it reaches, the tile's own elsewhere
        for (int j = 1; j <= n + 1; j++) {
            const int sz = coord.z * n + j - 1;
            if (sz < lowZ || sz > highZ) continue;
            const std::vector<uint16_t>* row = band.row(sz - lowZ, error);
            if (!row) return false;
            for (int i = 1; i <= n + 1; i++) {
                const int sx = coord.x * n + i - 1;
                if (sx < lowX || sx > highX) continue;
                tile.setSample(i, j, (*row)[static_cast<size_t>(std::min(sx - lowX, source.width - 1))]);
            }
        }
        // then the gutter: the image's, else a present neighbour's sample, else this tile's border
        // repeated (TiledMapStore::syncGutters)
        for (int j = 0; j <= n + 2; j++) {
            for (int i = 0; i <= n + 2; i++) {
                if (i != 0 && i != n + 2 && j != 0 && j != n + 2) continue;
                const int sx = coord.x * n + i - 1, sz = coord.z * n + j - 1;
                if (inImage(sx, sz)) {
                    const std::vector<uint16_t>* row = band.row(sz - lowZ, error);
                    if (!row) return false;
                    tile.setSample(i, j, (*row)[static_cast<size_t>(std::min(sx - lowX, source.width - 1))]);
                    continue;
                }
                TileCoord other;
                if (holder(sx, sz, coord, other)) {
                    // a tile that was there holds its neighbour's sample in its gutter already
                    if (before) continue;
                    std::shared_ptr<WorldTile> theirs = neighbours.get(other);
                    if (!theirs) {
                        error = "tile (" + std::to_string(other.x) + ", " + std::to_string(other.z) + ") could not be read";
                        return false;
                    }
                    tile.setSample(i, j, theirs->sample(sx - other.x * n + 1, sz - other.z * n + 1));
                    continue;
                }
                tile.setSample(i, j, tile.sample(std::min(std::max(i, 1), n + 1), std::min(std::max(j, 1), n + 1)));
            }
        }
        buildMinMax(tile, lattice);
        buildOverviewHeight(tile, lattice);
        buildSkirtDepth(tile, lattice);
        if (!target.layerColours.empty()) buildOverviewColour(tile, lattice, target.layerColours, target.layerCount);
        if (!target.write(tile)) {
            if (error.empty()) error = "tile (" + std::to_string(coord.x) + ", " + std::to_string(coord.z) + ") could not be written";
            return false;
        }
        done++;
        if (progress) progress(done, total);
    }
    return true;
}

bool jasperengine::terrain::scanRawHeightmap(const std::string& path, int width, int height, const std::function<bool()>& cancelled,
                                             RawHeightmapScan& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "the file could not be opened";
        return false;
    }
    if (width < 2 || height < 2 || !rawSizeMatches(file, width, height, error)) {
        if (error.empty()) error = "the heightmap is empty";
        return false;
    }
    out = RawHeightmapScan();
    uint16_t lowL = std::numeric_limits<uint16_t>::max(), highL = 0, lowB = lowL, highB = 0;
    double stepsL = 0.0, stepsB = 0.0;
    // T1's roughness looks at about 256 rows and columns: so does this, every row counting for the range
    const int step = std::max(1, std::max(width, height) / 256);
    std::vector<uint8_t> bytes(static_cast<size_t>(width) * 2);
    for (int y = 0; y < height; y++) {
        if ((y & 63) == 0 && cancelled && cancelled()) {
            error = "cancelled";
            return false;
        }
        if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            error = "the heightmap's row " + std::to_string(y) + " could not be read";
            return false;
        }
        for (int x = 0; x < width; x++) {
            const uint16_t little = rawSample(&bytes[static_cast<size_t>(x) * 2], false), big = rawSample(&bytes[static_cast<size_t>(x) * 2], true);
            lowL = std::min(lowL, little);
            highL = std::max(highL, little);
            lowB = std::min(lowB, big);
            highB = std::max(highB, big);
        }
        if (y % step != 0) continue;
        for (int x = 0; x + 1 < width; x += step) {
            const uint8_t* a = &bytes[static_cast<size_t>(x) * 2];
            const uint8_t* b = &bytes[static_cast<size_t>(x + 1) * 2];
            stepsL += std::fabs(static_cast<double>(rawSample(a, false)) - rawSample(b, false));
            stepsB += std::fabs(static_cast<double>(rawSample(a, true)) - rawSample(b, true));
        }
    }
    out.minLittle = lowL;
    out.maxLittle = highL;
    out.minBig = lowB;
    out.maxBig = highB;
    out.roughLittle = stepsL / std::max(1.0, static_cast<double>(highL) - lowL);
    out.roughBig = stepsB / std::max(1.0, static_cast<double>(highB) - lowB);
    return true;
}

HeightmapRows jasperengine::terrain::rawHeightmapRows(const RawHeightmapFile& raw) {
    HeightmapRows rows;
    auto file = std::make_shared<std::ifstream>(raw.path, std::ios::binary);
    std::string error;
    if (!*file || !rawSizeMatches(*file, raw.width, raw.height, error)) return rows;
    rows.width = raw.width;
    rows.height = raw.height;
    auto bytes = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(raw.width) * 2);
    const double span = raw.high - raw.low;
    rows.read = [file, bytes, raw, span](int row, std::vector<uint16_t>& out) {
        file->clear();
        file->seekg(static_cast<std::streamoff>(row) * raw.width * 2, std::ios::beg);
        if (!file->read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()))) return false;
        out.resize(static_cast<size_t>(raw.width));
        for (int x = 0; x < raw.width; x++) {
            const double value = rawSample(&(*bytes)[static_cast<size_t>(x) * 2], raw.bigEndian);
            const double normalized = span > 0.0 ? std::clamp((value - raw.low) / span, 0.0, 1.0) : 0.0;
            out[static_cast<size_t>(x)] = static_cast<uint16_t>(std::lround(normalized * 65535.0));
        }
        return true;
    };
    return rows;
}
