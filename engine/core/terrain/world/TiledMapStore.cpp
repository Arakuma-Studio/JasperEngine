// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/world/TiledMapStore.h"

#include <algorithm>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::terrain;

namespace {

    TileCoord offsetCoord(TileCoord tile, int dx, int dz) {
        return TileCoord(static_cast<int>(tile.x) + dx, static_cast<int>(tile.z) + dz);
    }

    std::shared_ptr<WorldTile> cloneTile(const std::shared_ptr<const WorldTile>& tile) {
        return tile ? std::make_shared<WorldTile>(*tile) : nullptr;
    }

    // R30, D11: the tiles of a large edit as they were before it, and as they were before its undo
    class TiledLargeEdit final : public TerrainLargeEdit {
    public:
        TiledLargeEdit(TiledMapStore& store, std::vector<std::pair<TileCoord, std::shared_ptr<WorldTile>>> before)
            : store(store), before(std::move(before)) {}

        void undo() override {
            after.clear();
            for (const auto& [coord, data] : before) {
                after.emplace_back(coord, cloneTile(store.tiles().tile(coord)));
            }
            for (const auto& [coord, data] : before) store.restoreTile(coord, std::make_shared<WorldTile>(*data));
        }

        void redo() override {
            for (const auto& [coord, data] : after) store.restoreTile(coord, data ? std::make_shared<WorldTile>(*data) : nullptr);
        }

    private:
        TiledMapStore& store;
        std::vector<std::pair<TileCoord, std::shared_ptr<WorldTile>>> before;
        std::vector<std::pair<TileCoord, std::shared_ptr<WorldTile>>> after;
    };

    // Over the cap with allow_no_undo: the edit runs and its step holds nothing to restore
    class UnrecordedLargeEdit final : public TerrainLargeEdit {
    public:
        void undo() override {}
        void redo() override {}
    };

    // R29, R86: the tiles a write made, removed on undo and put back as they were on redo
    class CreatedTilesHook final : public TerrainStoreUndoHook {
    public:
        CreatedTilesHook(TiledMapStore& store, std::vector<TileCoord> tiles): store(store), tiles(std::move(tiles)) {}

        void undo() override {
            kept.assign(tiles.size(), nullptr);
            for (size_t i = tiles.size(); i-- > 0;) {
                kept[i] = cloneTile(store.tiles().tile(tiles[i]));
                store.dropTile(tiles[i]);
            }
        }

        void redo() override {
            for (size_t i = 0; i < tiles.size(); i++) {
                if (kept[i]) store.restoreTile(tiles[i], kept[i]);
            }
        }

    private:
        TiledMapStore& store;
        std::vector<TileCoord> tiles;
        std::vector<std::shared_ptr<WorldTile>> kept;
    };

}

// ------------------------------------------------------------------ MemoryTileAccess

bool MemoryTileAccess::present(TileCoord tile) const {
    return tiles.count(tile.key()) > 0;
}

std::shared_ptr<const WorldTile> MemoryTileAccess::tile(TileCoord tile) const {
    const auto it = tiles.find(tile.key());
    return it == tiles.end() ? nullptr : it->second;
}

std::shared_ptr<WorldTile> MemoryTileAccess::writable(TileCoord tile) {
    const auto it = tiles.find(tile.key());
    if (it == tiles.end()) return nullptr;
    // someone else (a job, an undo snapshot) holds this copy: they keep it as it is
    if (it->second.use_count() > 1) it->second = std::make_shared<WorldTile>(*it->second);
    return it->second;
}

void MemoryTileAccess::add(TileCoord tile, std::shared_ptr<WorldTile> data) {
    tiles[tile.key()] = std::move(data);
    changedTiles[tile.key()] = true;
}

void MemoryTileAccess::remove(TileCoord tile) {
    tiles.erase(tile.key());
    changedTiles[tile.key()] = true;
}

void MemoryTileAccess::changed(TileCoord tile) {
    changedTiles[tile.key()] = true;
}

bool MemoryTileAccess::bounds(TileCoord& low, TileCoord& high) const {
    if (tiles.empty()) return false;
    int minX = WORLD_TILE_MAX, minZ = WORLD_TILE_MAX, maxX = WORLD_TILE_MIN, maxZ = WORLD_TILE_MIN;
    for (const auto& [key, data] : tiles) {
        const TileCoord coord = TileCoord::fromKey(key);
        minX = std::min<int>(minX, coord.x);
        minZ = std::min<int>(minZ, coord.z);
        maxX = std::max<int>(maxX, coord.x);
        maxZ = std::max<int>(maxZ, coord.z);
    }
    low = TileCoord(minX, minZ);
    high = TileCoord(maxX, maxZ);
    return true;
}

std::vector<TileCoord> MemoryTileAccess::coords() const {
    std::vector<TileCoord> out;
    out.reserve(tiles.size());
    for (const auto& [key, data] : tiles) out.push_back(TileCoord::fromKey(key));
    return out;
}

std::vector<TileCoord> MemoryTileAccess::takeChanged() {
    std::vector<TileCoord> out;
    for (const auto& [key, flag] : changedTiles) out.push_back(TileCoord::fromKey(key));
    changedTiles.clear();
    return out;
}

// ------------------------------------------------------------------ TiledMapStore

TiledMapStore::TiledMapStore(const WorldLattice& lattice, const WorldMaps& maps, TileAccess& access, TiledStoreOptions options)
    : worldLattice(lattice), worldMaps(maps), access(&access), storeOptions(options) {}

TiledMapStore::MapGeometry TiledMapStore::geometry(MapId map) const {
    MapGeometry geo;
    switch (map.kind) {
        case MapKind::Height:
            geo = MapGeometry{worldLattice.heightOrigin(), worldLattice.tileQuads, worldLattice.tileQuads + 1, worldLattice.heightLatticeSide(), true, 2};
            break;
        case MapKind::Blend:
            geo = MapGeometry{worldLattice.blendOrigin(), worldLattice.blendQuadCount(), worldLattice.blendSide(), worldLattice.blendLatticeSide(), true, 4};
            break;
        case MapKind::Hole:
            geo = MapGeometry{worldLattice.holeOrigin(), worldLattice.holeSide(), worldLattice.holeSide(), worldLattice.holeLatticeSide(), false, 1};
            break;
        case MapKind::Density:
            geo = MapGeometry{worldLattice.densityOrigin(), worldLattice.densitySide(), worldLattice.densitySide(), worldLattice.densityLatticeSide(), false, 1};
            break;
    }
    return geo;
}

int TiledMapStore::tileTexelOrigin(MapId map, int tileCoord) const {
    const MapGeometry geo = geometry(map);
    return geo.origin + tileCoord * geo.period;
}

TexelRect TiledMapStore::tileRect(MapId map, TileCoord tile) const {
    const MapGeometry geo = geometry(map);
    return TexelRect{geo.origin + tile.x * geo.period, geo.origin + tile.z * geo.period, geo.side, geo.side};
}

std::vector<TileCoord> TiledMapStore::tilesOf(MapId map, TexelRect rect) const {
    std::vector<TileCoord> out;
    if (rect.empty()) return out;
    const MapGeometry geo = geometry(map);
    const int lowX = std::max(WORLD_TILE_MIN, floorDiv(rect.x - geo.origin, geo.period) - (geo.shared ? 1 : 0));
    const int highX = std::min(WORLD_TILE_MAX, floorDiv(rect.right() - 1 - geo.origin, geo.period));
    const int lowZ = std::max(WORLD_TILE_MIN, floorDiv(rect.y - geo.origin, geo.period) - (geo.shared ? 1 : 0));
    const int highZ = std::min(WORLD_TILE_MAX, floorDiv(rect.bottom() - 1 - geo.origin, geo.period));
    for (int z = lowZ; z <= highZ; z++) {
        for (int x = lowX; x <= highX; x++) {
            const TileCoord coord(x, z);
            if (!intersect(rect, tileRect(map, coord)).empty()) out.push_back(coord);
        }
    }
    return out;
}

void TiledMapStore::defaultTexel(MapId map, uint8_t* out) const {
    switch (map.kind) {
        case MapKind::Height:
            out[0] = static_cast<uint8_t>(worldMaps.baseHeight & 0xFF);
            out[1] = static_cast<uint8_t>(worldMaps.baseHeight >> 8);
            break;
        case MapKind::Blend:
            for (int c = 0; c < 4; c++) {
                const float value = map.index >= 0 && map.index < 8 ? worldMaps.blendFill[map.index][c] : 0.0f;
                out[c] = static_cast<uint8_t>(quantizeChannel(value, 8));
            }
            break;
        case MapKind::Hole:
            out[0] = 255;
            break;
        case MapKind::Density:
            out[0] = 0;
            break;
    }
}

const TileMap* TiledMapStore::tileMap(const WorldTile& tile, MapId map) const {
    switch (map.kind) {
        case MapKind::Height: return tile.height.empty() ? nullptr : &tile.height;
        case MapKind::Blend:
            return map.index >= 0 && map.index < static_cast<int>(tile.blend.size()) && !tile.blend[map.index].empty() ? &tile.blend[map.index] : nullptr;
        case MapKind::Hole: return tile.holes.empty() ? nullptr : &tile.holes;
        case MapKind::Density:
            return map.index >= 0 && map.index < static_cast<int>(tile.density.size()) && !tile.density[map.index].empty() ? &tile.density[map.index]
                                                                                                                          : nullptr;
    }
    return nullptr;
}

TileMap* TiledMapStore::tileMap(WorldTile& tile, MapId map, bool allocate) {
    if (const TileMap* existing = tileMap(static_cast<const WorldTile&>(tile), map)) return const_cast<TileMap*>(existing);
    if (!allocate || map.kind == MapKind::Height || map.index < 0) return nullptr;
    const MapGeometry geo = geometry(map);
    uint8_t fill[4] = {};
    defaultTexel(map, fill);
    TileMap* made = nullptr;
    switch (map.kind) {
        case MapKind::Blend:
            if (static_cast<int>(tile.blend.size()) <= map.index) tile.blend.resize(map.index + 1);
            made = &tile.blend[map.index];
            made->allocate(geo.side, 4, 8, 0);
            for (size_t i = 0; i < made->bytes.size(); i += 4) std::memcpy(made->bytes.data() + i, fill, 4);
            copyBordersFromNeighbours(tile, map);
            break;
        case MapKind::Hole:
            made = &tile.holes;
            made->allocate(geo.side, 1, 8, fill[0]);
            break;
        case MapKind::Density:
            if (static_cast<int>(tile.density.size()) <= map.index) tile.density.resize(map.index + 1);
            made = &tile.density[map.index];
            made->allocate(geo.side, 1, 8, fill[0]);
            break;
        case MapKind::Height:
            break;
    }
    return made;
}

void TiledMapStore::copyBordersFromNeighbours(WorldTile& tile, MapId map) {
    const MapGeometry geo = geometry(map);
    if (!geo.shared) return;
    TileMap* own = map.kind == MapKind::Height ? &tile.height : (map.index < static_cast<int>(tile.blend.size()) ? &tile.blend[map.index] : nullptr);
    if (!own || own->empty()) return;
    const int last = geo.side - 1;
    const int gutter = map.kind == MapKind::Height ? 1 : 0;
    std::shared_ptr<const WorldTile> near[3][3];
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx != 0 || dz != 0) near[dz + 1][dx + 1] = access->tile(offsetCoord(tile.coord, dx, dz));
        }
    }
    // the first present neighbour holding the border texel gives it
    auto copyFrom = [&](int i, int j) {
        for (int dz = (j == 0 ? -1 : 0); dz <= (j == last ? 1 : 0); dz++) {
            for (int dx = (i == 0 ? -1 : 0); dx <= (i == last ? 1 : 0); dx++) {
                if (dx == 0 && dz == 0) continue;
                const std::shared_ptr<const WorldTile>& other = near[dz + 1][dx + 1];
                const TileMap* theirs = other ? tileMap(*other, map) : nullptr;
                if (!theirs) continue;
                std::memcpy(own->texel(i + gutter, j + gutter), theirs->texel(i - dx * last + gutter, j - dz * last + gutter), geo.bytesPerTexel);
                return;
            }
        }
    };
    for (int j = 0; j <= last; j++) {
        for (int i = 0; i <= last; i++) {
            if (i == 0 || i == last || j == 0 || j == last) copyFrom(i, j);
        }
    }
}

std::shared_ptr<WorldTile> TiledMapStore::makeTile(TileCoord coord) {
    auto tile = std::make_shared<WorldTile>();
    initTile(*tile, worldLattice, coord, worldMaps.baseHeight);
    copyBordersFromNeighbours(*tile, MapId{MapKind::Height, 0});
    return tile;
}

bool TiledMapStore::canCreate(TileCoord tile, std::string* why) const {
    if (!tile.addressable() || !tileWithinLimit(worldLattice, tile)) {
        if (why) *why = "tile (" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + ") is past the world's limit of +-16384 m";
        return false;
    }
    TileCoord low, high;
    if (access->bounds(low, high)) {
        const int width = std::max<int>(high.x, tile.x) - std::min<int>(low.x, tile.x) + 1;
        const int length = std::max<int>(high.z, tile.z) - std::min<int>(low.z, tile.z) + 1;
        if (width > storeOptions.maxBoxTiles || length > storeOptions.maxBoxTiles) {
            if (why) *why = "tile (" + std::to_string(tile.x) + ", " + std::to_string(tile.z) + ") would make the world span more than " +
                            std::to_string(storeOptions.maxBoxTiles) + " tiles a side";
            return false;
        }
    }
    return true;
}

bool TiledMapStore::addTile(TileCoord tile, std::string* why) {
    if (access->present(tile)) return true;
    if (!canCreate(tile, why)) return false;
    access->add(tile, makeTile(tile));
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) syncGutters(offsetCoord(tile, dx, dz));
    }
    markDirty(MapId{MapKind::Height, 0}, tileRect(MapId{MapKind::Height, 0}, tile));
    return true;
}

bool TiledMapStore::removeTile(TileCoord tile) {
    if (!access->present(tile)) return false;
    dropTile(tile);
    return true;
}

void TiledMapStore::dropTile(TileCoord tile) {
    if (!access->present(tile)) return;
    access->remove(tile);
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx != 0 || dz != 0) syncGutters(offsetCoord(tile, dx, dz));
        }
    }
    markDirty(MapId{MapKind::Height, 0}, tileRect(MapId{MapKind::Height, 0}, tile));
}

void TiledMapStore::restoreTile(TileCoord tile, std::shared_ptr<WorldTile> data) {
    if (!data) {
        dropTile(tile);
        return;
    }
    access->add(tile, std::move(data));
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) syncGutters(offsetCoord(tile, dx, dz));
    }
    markDirty(MapId{MapKind::Height, 0}, tileRect(MapId{MapKind::Height, 0}, tile));
    for (int i = 0; i < worldMaps.blendMaps; i++) markDirty(MapId{MapKind::Blend, i}, tileRect(MapId{MapKind::Blend, i}, tile));
    if (worldMaps.hasHoles) markDirty(MapId{MapKind::Hole, 0}, tileRect(MapId{MapKind::Hole, 0}, tile));
    for (int i = 0; i < worldMaps.densityMaps; i++) markDirty(MapId{MapKind::Density, i}, tileRect(MapId{MapKind::Density, i}, tile));
}

void TiledMapStore::syncGutters(TileCoord coord) {
    const std::shared_ptr<const WorldTile> current = access->tile(coord);
    if (!current) return;
    const int n = worldLattice.tileQuads;
    const int last = n + 2;
    std::shared_ptr<const WorldTile> near[3][3];
    for (int dz = -1; dz <= 1; dz++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx != 0 || dz != 0) near[dz + 1][dx + 1] = access->tile(offsetCoord(coord, dx, dz));
        }
    }
    // the tiles whose shared range holds a local sample coordinate (-1..n+1): one, or two on a border
    auto holders = [n](int local, int out[2]) {
        if (local < 0) { out[0] = -1; return 1; }
        if (local > n) { out[0] = 1; return 1; }
        int count = 0;
        out[count++] = 0;
        if (local == 0) out[count++] = -1;
        if (local == n) out[count++] = 1;
        return count;
    };
    // what each gutter texel should hold: a present neighbour's sample, else this tile's border
    std::vector<std::pair<int, uint16_t>> wanted;
    for (int j = 0; j <= last; j++) {
        for (int i = 0; i <= last; i++) {
            if (i != 0 && i != last && j != 0 && j != last) continue;
            const int lx = i - 1, lz = j - 1;
            int xs[2], zs[2];
            const int xCount = holders(lx, xs), zCount = holders(lz, zs);
            bool found = false;
            uint16_t value = 0;
            for (int a = 0; a < zCount && !found; a++) {
                for (int b = 0; b < xCount && !found; b++) {
                    const int ox = xs[b], oz = zs[a];
                    if (ox == 0 && oz == 0) continue;
                    const std::shared_ptr<const WorldTile>& other = near[oz + 1][ox + 1];
                    if (!other || other->height.empty()) continue;
                    value = other->sample(lx - ox * n + 1, lz - oz * n + 1);
                    found = true;
                }
            }
            if (!found) value = current->sample(std::min(std::max(i, 1), n + 1), std::min(std::max(j, 1), n + 1));
            if (current->sample(i, j) != value) wanted.emplace_back(j * (n + 3) + i, value);
        }
    }
    if (wanted.empty()) return;
    const std::shared_ptr<WorldTile> tile = access->writable(coord);
    if (!tile) return;
    for (const auto& [index, value] : wanted) tile->setSample(index % (n + 3), index / (n + 3), value);
    access->changed(coord);
}

int TiledMapStore::seamErrors(TileCoord coord, bool repair) {
    const std::shared_ptr<const WorldTile> tile = access->tile(coord);
    if (!tile) return 0;
    const int n = worldLattice.tileQuads;
    int errors = 0;
    for (int j = 0; j <= n; j++) {
        for (int i = 0; i <= n; i++) {
            if (i != 0 && i != n && j != 0 && j != n) continue;
            const int sx = coord.x * n + i, sz = coord.z * n + j;
            const uint16_t mine = access->tile(coord)->sample(i + 1, j + 1);
            // the holder with the lowest (x, z) decides
            TileCoord winner = coord;
            uint16_t winning = mine;
            bool mismatch = false;
            std::vector<TileCoord> holders;
            for (int dz = (j == 0 ? -1 : 0); dz <= (j == n ? 1 : 0); dz++) {
                for (int dx = (i == 0 ? -1 : 0); dx <= (i == n ? 1 : 0); dx++) {
                    if (dx == 0 && dz == 0) continue;
                    const TileCoord other = offsetCoord(coord, dx, dz);
                    const std::shared_ptr<const WorldTile> theirs = access->tile(other);
                    if (!theirs || theirs->height.empty()) continue;
                    holders.push_back(other);
                    const uint16_t value = theirs->sample(sx - other.x * n + 1, sz - other.z * n + 1);
                    if (value != mine) mismatch = true;
                    if (other.x < winner.x || (other.x == winner.x && other.z < winner.z)) {
                        winner = other;
                        winning = value;
                    }
                }
            }
            if (!mismatch) continue;
            errors++;
            if (!repair) continue;
            holders.push_back(coord);
            for (const TileCoord& holder : holders) {
                const std::shared_ptr<WorldTile> data = access->writable(holder);
                if (!data) continue;
                data->setSample(sx - holder.x * n + 1, sz - holder.z * n + 1, winning);
                access->changed(holder);
            }
        }
    }
    if (repair && errors > 0) {
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) syncGutters(offsetCoord(coord, dx, dz));
        }
        markDirty(MapId{MapKind::Height, 0}, grow(tileRect(MapId{MapKind::Height, 0}, coord), 1));
    }
    return errors;
}

LargeEditPrediction TiledMapStore::predictLarge(MapId map, TexelRect rect) const {
    LargeEditPrediction prediction;
    prediction.large = rect.area() > storeOptions.largeEditTexels;
    if (!prediction.large) return prediction;
    for (const TileCoord& coord : tilesOf(map, grow(rect, 1))) {
        if (const std::shared_ptr<const WorldTile> tile = access->tile(coord)) {
            prediction.snapshotBytes += tile->cpuBytes();
            prediction.tiles++;
        }
    }
    prediction.undoable = prediction.snapshotBytes <= storeOptions.snapshotCapBytes;
    return prediction;
}

TerrainFrame TiledMapStore::frame() const {
    return TerrainFrame{worldMaps.maxHeight};
}

MapFormat TiledMapStore::format(MapId map) const {
    const MapGeometry geo = geometry(map);
    MapFormat format;
    format.bounds = TexelRect{0, 0, geo.lattice, geo.lattice};
    switch (map.kind) {
        case MapKind::Height:
            format.channels = 1;
            format.bitsPerChannel = 16;
            format.exists = map.index == 0;
            break;
        case MapKind::Blend:
            format.channels = 4;
            format.bitsPerChannel = 8;
            format.exists = map.index >= 0 && map.index < worldMaps.blendMaps;
            break;
        case MapKind::Hole:
            format.channels = 1;
            format.bitsPerChannel = 8;
            format.exists = map.index == 0 && worldMaps.hasHoles;
            break;
        case MapKind::Density:
            format.channels = 1;
            format.bitsPerChannel = 8;
            format.exists = map.index >= 0 && map.index < worldMaps.densityMaps;
            break;
    }
    return format;
}

bool TiledMapStore::readBytes(MapId map, TexelRect rect, std::span<uint8_t> out) const {
    const MapFormat mapFormat = format(map);
    if (!mapFormat.exists || rect.empty() || !mapFormat.bounds.contains(rect)) return false;
    const MapGeometry geo = geometry(map);
    const int bytes = geo.bytesPerTexel;
    if (out.size() != static_cast<size_t>(rect.area()) * bytes) return false;
    uint8_t fill[4] = {};
    defaultTexel(map, fill);
    for (size_t i = 0; i < out.size(); i += bytes) std::memcpy(out.data() + i, fill, bytes);
    const int gutter = map.kind == MapKind::Height ? 1 : 0;
    for (const TileCoord& coord : tilesOf(map, rect)) {
        const std::shared_ptr<const WorldTile> tile = access->tile(coord);
        if (!tile) continue;
        const TileMap* source = tileMap(*tile, map);
        if (!source) continue;
        const TexelRect own = tileRect(map, coord);
        const TexelRect sub = intersect(rect, own);
        for (int y = sub.y; y < sub.bottom(); y++) {
            const uint8_t* from = source->texel(sub.x - own.x + gutter, y - own.y + gutter);
            uint8_t* to = out.data() + (static_cast<size_t>(y - rect.y) * rect.w + (sub.x - rect.x)) * bytes;
            std::memcpy(to, from, static_cast<size_t>(sub.w) * bytes);
        }
    }
    return true;
}

bool TiledMapStore::read(MapId map, TexelRect rect, std::span<float> out) const {
    const MapFormat mapFormat = format(map);
    if (!mapFormat.exists) return false;
    const int floats = floatChannels(map, mapFormat);
    if (out.size() != static_cast<size_t>(rect.area()) * floats) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(rect.area()) * bytesPerTexel(mapFormat));
    if (!readBytes(map, rect, bytes)) return false;
    const int stride = bytesPerTexel(mapFormat);
    for (int64_t i = 0; i < rect.area(); i++) decodeTexel(map, mapFormat, bytes.data() + i * stride, out.data() + i * floats);
    return true;
}

WriteResult TiledMapStore::write(MapId map, TexelRect rect, std::span<const float> in) {
    const MapFormat mapFormat = format(map);
    if (!mapFormat.exists) return WriteResult{false, "map does not exist", 0};
    const int floats = floatChannels(map, mapFormat);
    if (in.size() != static_cast<size_t>(rect.area()) * floats) return WriteResult{false, "size mismatch", 0};
    const int stride = bytesPerTexel(mapFormat);
    std::vector<uint8_t> bytes(static_cast<size_t>(rect.area()) * stride);
    const int step = mapFormat.bitsPerChannel >= 16 ? 2 : 1;
    for (int64_t i = 0; i < rect.area(); i++) {
        for (int c = 0; c < floats; c++) {
            encodeChannel(bytes.data() + i * stride + c * step, quantizeChannel(in[i * floats + c], mapFormat.bitsPerChannel), mapFormat.bitsPerChannel);
        }
    }
    return writeStored(map, rect, bytes, true);
}

WriteResult TiledMapStore::writeBytes(MapId map, TexelRect rect, std::span<const uint8_t> in) {
    return writeStored(map, rect, in, true);
}

WriteResult TiledMapStore::writeStored(MapId map, TexelRect rect, std::span<const uint8_t> in, bool allowCreate) {
    const MapFormat mapFormat = format(map);
    if (!mapFormat.exists) return WriteResult{false, "map does not exist", 0};
    if (rect.empty() || !mapFormat.bounds.contains(rect)) return WriteResult{false, "rectangle outside the world's lattice", 0};
    const MapGeometry geo = geometry(map);
    const int bytes = geo.bytesPerTexel;
    if (in.size() != static_cast<size_t>(rect.area()) * bytes) return WriteResult{false, "size mismatch", 0};
    const int gutter = map.kind == MapKind::Height ? 1 : 0;
    uint8_t fill[4] = {};
    defaultTexel(map, fill);

    WriteResult result{true, "", 0};
    std::vector<TileCoord> heightTouched;
    for (const TileCoord& coord : tilesOf(map, rect)) {
        const TexelRect own = tileRect(map, coord);
        const TexelRect sub = intersect(rect, own);
        auto incoming = [&](int x, int y) { return in.data() + (static_cast<size_t>(y - rect.y) * rect.w + (x - rect.x)) * bytes; };

        if (!access->present(coord)) {
            if (!allowCreate || !storeOptions.autoAddTiles || map.kind == MapKind::Hole) continue;
            // a change on a shared border only reaches the tiles there: an absent one is made by
            // a change inside it, and copies its borders when it is
            const TexelRect inside = geo.shared ? intersect(sub, grow(own, -1)) : sub;
            bool differs = false;
            for (int y = inside.y; y < inside.bottom() && !differs; y++) {
                for (int x = inside.x; x < inside.right() && !differs; x++) differs = std::memcmp(incoming(x, y), fill, bytes) != 0;
            }
            if (!differs) continue;
            std::string why;
            if (!canCreate(coord, &why)) return WriteResult{false, why, result.tilesCreated};
            access->add(coord, makeTile(coord));
            created.push_back(coord);
            result.tilesCreated++;
            heightTouched.push_back(coord);
        }

        // nothing changes: no writable copy, no notice
        const std::shared_ptr<const WorldTile> current = access->tile(coord);
        if (!current) continue;
        const TileMap* existing = tileMap(*current, map);
        bool changes = false;
        for (int y = sub.y; y < sub.bottom() && !changes; y++) {
            for (int x = sub.x; x < sub.right() && !changes; x++) {
                const uint8_t* now = existing ? existing->texel(x - own.x + gutter, y - own.y + gutter) : fill;
                changes = std::memcmp(incoming(x, y), now, bytes) != 0;
            }
        }
        if (!changes) continue;

        const std::shared_ptr<WorldTile> tile = access->writable(coord);
        if (!tile) {
            std::string why = "tile (" + std::to_string(coord.x) + ", " + std::to_string(coord.z) + ") cannot change now";
            access->mayWrite(coord, &why);
            return WriteResult{false, why, result.tilesCreated};
        }
        TileMap* target = tileMap(*tile, map, true);
        if (!target) continue;
        TexelRect changed;
        for (int y = sub.y; y < sub.bottom(); y++) {
            for (int x = sub.x; x < sub.right(); x++) {
                uint8_t* to = target->texel(x - own.x + gutter, y - own.y + gutter);
                if (std::memcmp(to, incoming(x, y), bytes) == 0) continue;
                std::memcpy(to, incoming(x, y), bytes);
                changed = unite(changed, TexelRect{x, y, 1, 1});
            }
        }
        if (!changed.empty()) markDirty(map, changed);
        access->changed(coord);
        if (map.kind == MapKind::Height) heightTouched.push_back(coord);
    }

    if (!heightTouched.empty()) {
        std::vector<uint32_t> keys;
        for (const TileCoord& coord : heightTouched) {
            for (int dz = -1; dz <= 1; dz++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const TileCoord near = offsetCoord(coord, dx, dz);
                    if (near.addressable()) keys.push_back(near.key());
                }
            }
        }
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
        for (uint32_t key : keys) syncGutters(TileCoord::fromKey(key));
    }
    return result;
}

bool TiledMapStore::canWrite(MapId map, TexelRect rect, std::string* why) const {
    const MapFormat mapFormat = format(map);
    if (!mapFormat.bounds.contains(rect)) {
        if (why) *why = "the rectangle reaches past the world's addressable lattice";
        return false;
    }
    for (const TileCoord& coord : tilesOf(map, rect)) {
        if (access->present(coord) && !access->mayWrite(coord, why)) return false;
    }
    if (!storeOptions.autoAddTiles || map.kind == MapKind::Hole) return true;
    // every absent tile the rectangle reaches could be made: they must fit R6 together
    TileCoord low, high;
    bool any = access->bounds(low, high);
    const bool shared = geometry(map).shared;
    for (const TileCoord& coord : tilesOf(map, rect)) {
        if (access->present(coord)) continue;
        if (shared && intersect(rect, grow(tileRect(map, coord), -1)).empty()) continue;
        if (!coord.addressable() || !tileWithinLimit(worldLattice, coord)) {
            if (why) *why = "tile (" + std::to_string(coord.x) + ", " + std::to_string(coord.z) + ") is past the world's limit of +-16384 m";
            return false;
        }
        if (!any) {
            low = high = coord;
            any = true;
        } else {
            low = TileCoord(std::min(low.x, coord.x), std::min(low.z, coord.z));
            high = TileCoord(std::max(high.x, coord.x), std::max(high.z, coord.z));
        }
    }
    if (any && (high.x - low.x + 1 > storeOptions.maxBoxTiles || high.z - low.z + 1 > storeOptions.maxBoxTiles)) {
        if (why) *why = "the edit would make the world span more than " + std::to_string(storeOptions.maxBoxTiles) + " tiles a side";
        return false;
    }
    return true;
}

bool TiledMapStore::canCreateMap(MapId map, std::string* why) const {
    const bool ok = (map.kind == MapKind::Blend && map.index >= 0 && map.index < 3) || (map.kind == MapKind::Hole && map.index == 0) ||
                    (map.kind == MapKind::Density && map.index >= 0 && map.index < 64);
    if (!ok && why) *why = "a Terrain World has no such map";
    return ok;
}

WriteResult TiledMapStore::createMap(MapId map, MapFormat, float fill) {
    std::string why;
    if (!canCreateMap(map, &why)) return WriteResult{false, why, 0};
    switch (map.kind) {
        case MapKind::Blend:
            worldMaps.blendMaps = std::max(worldMaps.blendMaps, map.index + 1);
            for (int c = 0; c < 3; c++) worldMaps.blendFill[map.index][c] = fill;
            worldMaps.blendFill[map.index][3] = 1.0f;
            break;
        case MapKind::Hole:
            worldMaps.hasHoles = true;
            break;
        case MapKind::Density:
            worldMaps.densityMaps = std::max(worldMaps.densityMaps, map.index + 1);
            break;
        case MapKind::Height:
            break;
    }
    return WriteResult{true, "", 0};
}

void TiledMapStore::markDirty(MapId map, TexelRect rect) {
    for (auto& [id, area] : dirty) {
        if (id == map) {
            area = unite(area, rect);
            return;
        }
    }
    dirty.emplace_back(map, rect);
}

void TiledMapStore::takeDirty(std::vector<std::pair<MapId, TexelRect>>& out) {
    for (const auto& entry : dirty) out.push_back(entry);
    dirty.clear();
}

Vector2 TiledMapStore::worldOfTexel(MapId map, int x, int y) const {
    const MapGeometry geo = geometry(map);
    const float texelSize = worldLattice.tileSize() / static_cast<float>(geo.period);
    const float half = geo.shared ? 0.0f : 0.5f;
    return Vector2((static_cast<float>(x - geo.origin) + half) * texelSize, (static_cast<float>(y - geo.origin) + half) * texelSize);
}

Vector2 TiledMapStore::texelOfWorld(MapId map, float localX, float localZ) const {
    const MapGeometry geo = geometry(map);
    const float perMetre = static_cast<float>(geo.period) / worldLattice.tileSize();
    const float half = geo.shared ? 0.0f : 0.5f;
    return Vector2(localX * perMetre + static_cast<float>(geo.origin) - half, localZ * perMetre + static_cast<float>(geo.origin) - half);
}

TexelRect TiledMapStore::preferredBlock(MapId map) const {
    const MapGeometry geo = geometry(map);
    return TexelRect{geo.origin, geo.origin, geo.period, geo.period};
}

std::unique_ptr<TerrainLargeEdit> TiledMapStore::beginLarge(MapId map, TexelRect rect) {
    const LargeEditPrediction prediction = predictLarge(map, rect);
    if (!prediction.large) return nullptr;
    if (!prediction.undoable) return std::make_unique<UnrecordedLargeEdit>();
    std::vector<std::pair<TileCoord, std::shared_ptr<WorldTile>>> before;
    for (const TileCoord& coord : tilesOf(map, grow(rect, 1))) {
        if (std::shared_ptr<WorldTile> copy = cloneTile(access->tile(coord))) before.emplace_back(coord, std::move(copy));
    }
    return std::make_unique<TiledLargeEdit>(*this, std::move(before));
}

std::unique_ptr<TerrainStoreUndoHook> TiledMapStore::takeUndoHook() {
    if (created.empty()) return nullptr;
    std::vector<TileCoord> tiles = std::move(created);
    created.clear();
    return std::make_unique<CreatedTilesHook>(*this, std::move(tiles));
}
