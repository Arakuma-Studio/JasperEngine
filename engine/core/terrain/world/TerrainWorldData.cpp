// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/world/TerrainWorldData.h"

#include "util/Fnv.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::terraincore;

namespace {

    constexpr uint8_t WORLD_BIN_MAGIC[4] = {'J', 'T', 'W', 'B'};

    struct Writer {
        std::vector<uint8_t>& out;
        void u16(uint16_t value) {
            out.push_back(static_cast<uint8_t>(value & 0xFF));
            out.push_back(static_cast<uint8_t>(value >> 8));
        }
        void u32(uint32_t value) {
            for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
        }
        void u64(uint64_t value) {
            for (int i = 0; i < 8; i++) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
        }
        void f32(float value) {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            u32(bits);
        }
    };

    struct Reader {
        std::span<const uint8_t> in;
        size_t at = 0;
        bool ok = true;
        bool need(size_t bytes) {
            if (!ok || in.size() - at < bytes || at > in.size()) ok = false;
            return ok;
        }
        uint16_t u16() {
            if (!need(2)) return 0;
            const uint16_t value = static_cast<uint16_t>(in[at] | (in[at + 1] << 8));
            at += 2;
            return value;
        }
        uint32_t u32() {
            if (!need(4)) return 0;
            uint32_t value = 0;
            for (int i = 0; i < 4; i++) value |= static_cast<uint32_t>(in[at + i]) << (8 * i);
            at += 4;
            return value;
        }
        uint64_t u64() {
            const uint64_t low = u32();
            return low | (static_cast<uint64_t>(u32()) << 32);
        }
        float f32() {
            const uint32_t bits = u32();
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }
    };

    void putPrefix(std::vector<uint8_t>& raw, int width, int height, int channels, int bits) {
        Writer w{raw};
        w.u32(static_cast<uint32_t>(width));
        w.u32(static_cast<uint32_t>(height));
        w.u32(static_cast<uint32_t>(channels));
        w.u32(static_cast<uint32_t>(bits));
    }

}

void jasperengine::terraincore::encodeWorldBin(const WorldManifest& manifest, std::vector<uint8_t>& out) {
    // byte by byte: GCC 11 misreads an insert of the magic into a cleared vector (-Wstringop-overflow)
    out.clear();
    for (uint8_t byte : WORLD_BIN_MAGIC) out.push_back(byte);
    Writer w{out};
    w.u32(manifest.formatVersion);
    const WorldLattice& lattice = manifest.lattice;
    w.u32(static_cast<uint32_t>(lattice.tileQuads));
    w.f32(lattice.spacing);
    w.u32(static_cast<uint32_t>(lattice.gridResolution));
    w.u32(static_cast<uint32_t>(lattice.overviewStride));
    w.u32(static_cast<uint32_t>(lattice.blendQuadCount()));
    w.u32(static_cast<uint32_t>(lattice.holeSide()));
    w.u32(static_cast<uint32_t>(lattice.densitySide()));
    const WorldMaps& maps = manifest.maps;
    w.f32(maps.maxHeight);
    w.u32(maps.baseHeight);
    w.u32(static_cast<uint32_t>(maps.blendMaps));
    for (const auto& fill : maps.blendFill) {
        for (float channel : fill) w.f32(channel);
    }
    w.u32(maps.hasHoles ? 1u : 0u);
    w.u32(static_cast<uint32_t>(maps.densityMaps));
    w.u64(manifest.layerHash);
    w.f32(manifest.skirtDepth);
    w.u32(static_cast<uint32_t>(manifest.guid.size()));
    out.insert(out.end(), manifest.guid.begin(), manifest.guid.end());
    w.u32(static_cast<uint32_t>(manifest.tiles.size()));
    for (const WorldTileInfo& tile : manifest.tiles) {
        w.u16(static_cast<uint16_t>(tile.coord.x));
        w.u16(static_cast<uint16_t>(tile.coord.z));
        w.u16(tile.minHeight);
        w.u16(tile.maxHeight);
    }
    // R27: the tiles with trees, a tail a world without any leaves out, so its bytes stay as they were
    uint32_t treeTiles = 0;
    for (const WorldTileInfo& tile : manifest.tiles) treeTiles += tile.trees ? 1u : 0u;
    if (treeTiles > 0) {
        w.u32(treeTiles);
        for (const WorldTileInfo& tile : manifest.tiles) {
            if (!tile.trees) continue;
            w.u16(static_cast<uint16_t>(tile.coord.x));
            w.u16(static_cast<uint16_t>(tile.coord.z));
        }
    }
    w.u64(fnv1a64(out.data(), out.size()));
}

bool jasperengine::terraincore::decodeWorldBin(std::span<const uint8_t> bytes, WorldManifest& out, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (bytes.size() < 16 || std::memcmp(bytes.data(), WORLD_BIN_MAGIC, 4) != 0) return fail("not a world.bin");
    Reader tail{bytes, bytes.size() - 8};
    if (tail.u64() != fnv1a64(bytes.data(), bytes.size() - 8)) return fail("world.bin checksum mismatch");
    Reader r{bytes.first(bytes.size() - 8), 4};
    WorldManifest manifest;
    manifest.formatVersion = r.u32();
    if (manifest.formatVersion == 0 || manifest.formatVersion > WORLD_FORMAT_VERSION) return fail("world.bin made by a newer version");
    WorldLattice& lattice = manifest.lattice;
    lattice.tileQuads = static_cast<int>(r.u32());
    lattice.spacing = r.f32();
    lattice.gridResolution = static_cast<int>(r.u32());
    lattice.overviewStride = static_cast<int>(r.u32());
    lattice.blendQuads = static_cast<int>(r.u32());
    lattice.holeTexels = static_cast<int>(r.u32());
    lattice.densityTexels = static_cast<int>(r.u32());
    WorldMaps& maps = manifest.maps;
    maps.maxHeight = r.f32();
    maps.baseHeight = static_cast<uint16_t>(r.u32());
    maps.blendMaps = static_cast<int>(r.u32());
    for (auto& fill : maps.blendFill) {
        for (float& channel : fill) channel = r.f32();
    }
    maps.hasHoles = r.u32() != 0;
    maps.densityMaps = static_cast<int>(r.u32());
    manifest.layerHash = r.u64();
    manifest.skirtDepth = r.f32();
    const uint32_t guidBytes = r.u32();
    if (guidBytes > 256 || !r.need(guidBytes)) return fail("world.bin is truncated");
    manifest.guid.assign(reinterpret_cast<const char*>(bytes.data() + r.at), guidBytes);
    r.at += guidBytes;
    const uint32_t tileCount = r.u32();
    if (tileCount > static_cast<uint32_t>(WORLD_TILES_PER_SIDE * WORLD_TILES_PER_SIDE) || !r.need(static_cast<size_t>(tileCount) * 8)) {
        return fail("world.bin is truncated");
    }
    manifest.tiles.resize(tileCount);
    for (WorldTileInfo& tile : manifest.tiles) {
        const int16_t x = static_cast<int16_t>(r.u16());
        const int16_t z = static_cast<int16_t>(r.u16());
        tile.coord = TileCoord(x, z);
        tile.minHeight = r.u16();
        tile.maxHeight = r.u16();
        if (!tile.coord.addressable()) return fail("world.bin names a tile past the addressable grid");
    }
    if (r.ok && r.at < r.in.size()) {
        const uint32_t treeTiles = r.u32();
        if (treeTiles > tileCount || !r.need(static_cast<size_t>(treeTiles) * 4)) return fail("world.bin is truncated");
        std::vector<uint32_t> keys(treeTiles);
        for (uint32_t& key : keys) {
            const int16_t x = static_cast<int16_t>(r.u16());
            const int16_t z = static_cast<int16_t>(r.u16());
            key = TileCoord(x, z).key();
        }
        std::sort(keys.begin(), keys.end());
        for (WorldTileInfo& tile : manifest.tiles) tile.trees = std::binary_search(keys.begin(), keys.end(), tile.coord.key());
    }
    if (!r.ok || r.at != r.in.size()) return fail("world.bin is truncated");
    std::string why;
    if (!validateLattice(lattice, &why)) return fail("world.bin: " + why);
    if (!(maps.maxHeight > 0.0f) || maps.blendMaps < 0 || maps.blendMaps > 3 || maps.densityMaps < 0 || maps.densityMaps > 64) {
        return fail("world.bin has invalid maps");
    }
    out = std::move(manifest);
    return true;
}

void jasperengine::terraincore::initOverview(WorldOverview& overview, const WorldLattice& lattice, TileCoord low, TileCoord high, uint16_t baseHeight) {
    overview = WorldOverview();
    overview.low = low;
    overview.tilesX = high.x - low.x + 1;
    overview.tilesZ = high.z - low.z + 1;
    overview.stride = lattice.overviewStride;
    overview.heights.assign(static_cast<size_t>(overview.width()) * overview.height(), baseHeight);
    overview.colour.assign(static_cast<size_t>(overview.width()) * overview.height() * 4, 128);
    for (size_t i = 3; i < overview.colour.size(); i += 4) overview.colour[i] = 255;
}

bool jasperengine::terraincore::setOverviewPatch(WorldOverview& overview, const WorldTile& tile) {
    if (!overview.covers(tile.coord)) return false;
    const int side = overview.stride + 1;
    if (tile.overviewHeight.size() != static_cast<size_t>(side) * side) return false;
    const int originX = (tile.coord.x - overview.low.x) * overview.stride;
    const int originZ = (tile.coord.z - overview.low.z) * overview.stride;
    const int width = overview.width();
    const bool colour = tile.overviewColour.size() == static_cast<size_t>(side) * side * 4;
    for (int j = 0; j < side; j++) {
        for (int i = 0; i < side; i++) {
            const size_t to = static_cast<size_t>(originZ + j) * width + (originX + i);
            const size_t from = static_cast<size_t>(j) * side + i;
            overview.heights[to] = tile.overviewHeight[from];
            if (colour) std::memcpy(overview.colour.data() + to * 4, tile.overviewColour.data() + from * 4, 4);
        }
    }
    overview.skirtDepth = std::max(overview.skirtDepth, tile.skirtDepth);
    return true;
}

SurfaceSample jasperengine::terraincore::sampleOverview(const WorldOverview& overview, const WorldLattice& lattice, float maxHeight, float localX, float localZ) {
    SurfaceSample sample;
    sample.exact = false;
    if (overview.empty()) return sample;
    const float size = lattice.tileSize();
    const float step = size / static_cast<float>(overview.stride);
    const float u = (localX - static_cast<float>(overview.low.x) * size) / step;
    const float v = (localZ - static_cast<float>(overview.low.z) * size) / step;
    const int width = overview.width(), height = overview.height();
    sample.present = true;
    sample.inside = overview.covers(TileCoord(tileOfLocal(localX, size), tileOfLocal(localZ, size)));
    auto at = [&](int x, int z) {
        x = std::min(std::max(x, 0), width - 1);
        z = std::min(std::max(z, 0), height - 1);
        return heightOfSample(overview.heights[static_cast<size_t>(z) * width + x], maxHeight);
    };
    const float cu = std::min(std::max(u, 0.0f), static_cast<float>(width - 1));
    const float cv = std::min(std::max(v, 0.0f), static_cast<float>(height - 1));
    const int x0 = std::min(static_cast<int>(std::floor(cu)), width - 2 < 0 ? 0 : width - 2);
    const int z0 = std::min(static_cast<int>(std::floor(cv)), height - 2 < 0 ? 0 : height - 2);
    const float fx = cu - static_cast<float>(x0), fz = cv - static_cast<float>(z0);
    // the drawn overview and the coarse collider split each quad along one diagonal (R22, R23)
    sample.height = latticeTriangle(at(x0, z0), at(x0 + 1, z0), at(x0, z0 + 1), at(x0 + 1, z0 + 1), fx, fz);
    const int nx = static_cast<int>(std::lround(cu)), nz = static_cast<int>(std::lround(cv));
    const LatticeNormal normal = latticeNormal(at(nx - 1, nz), at(nx + 1, nz), at(nx, nz - 1), at(nx, nz + 1), step, step);
    sample.normal = Vector3(normal.x, normal.y, normal.z);
    return sample;
}

bool jasperengine::terraincore::encodeOverview(const WorldOverview& overview, const WorldLattice& lattice, const TileCodec& codec, int level,
                                           std::vector<uint8_t>& out, std::string* error) {
    if (overview.heights.size() != static_cast<size_t>(overview.width()) * overview.height() || overview.colour.size() != overview.heights.size() * 4) {
        if (error) *error = "the overview's sizes do not match its box";
        return false;
    }
    TileFileHeader header;
    header.tileX = overview.low.x;
    header.tileZ = overview.low.z;
    header.tileQuads = lattice.tileQuads;
    header.spacing = lattice.spacing;
    header.flags = TILE_FLAG_OVERVIEW;
    std::vector<TileSectionPayload> sections(3);
    sections[0].id = TILE_SECTION_OVERVIEW_INFO;
    {
        Writer w{sections[0].raw};
        w.u32(static_cast<uint32_t>(overview.tilesX));
        w.u32(static_cast<uint32_t>(overview.tilesZ));
        w.u32(static_cast<uint32_t>(overview.stride));
        w.f32(overview.skirtDepth);
    }
    sections[1].id = TILE_SECTION_OVERVIEW_HEIGHT;
    sections[1].filter = static_cast<uint32_t>(TileFilter::Predictor16);
    putPrefix(sections[1].raw, overview.width(), overview.height(), 1, 16);
    {
        Writer w{sections[1].raw};
        for (uint16_t value : overview.heights) w.u16(value);
    }
    sections[2].id = TILE_SECTION_OVERVIEW_COLOUR;
    putPrefix(sections[2].raw, overview.width(), overview.height(), 4, 8);
    sections[2].raw.insert(sections[2].raw.end(), overview.colour.begin(), overview.colour.end());
    return writeTileSections(header, sections, nullptr, codec, level, out, error);
}

TileError jasperengine::terraincore::decodeOverview(std::span<const uint8_t> file, const TileCodec& codec, WorldOverview& out) {
    TileHeader header;
    const TileError parsed = parseTileHeader(file, header);
    if (parsed != TileError::None) return parsed;
    if ((header.flags & TILE_FLAG_OVERVIEW) == 0) return TileError::BadHeader;
    WorldOverview overview;
    overview.low = TileCoord(header.tileX, header.tileZ);
    bool info = false, heights = false, colours = false;
    for (size_t i = 0; i < header.sections.size(); i++) {
        std::vector<uint8_t> raw;
        const uint32_t id = header.sections[i].id;
        if (id != TILE_SECTION_OVERVIEW_INFO && id != TILE_SECTION_OVERVIEW_HEIGHT && id != TILE_SECTION_OVERVIEW_COLOUR) continue;
        const TileError read = readTileSectionRaw(file, header, i, codec, raw);
        if (read != TileError::None) return read;
        Reader r{raw};
        if (id == TILE_SECTION_OVERVIEW_INFO) {
            overview.tilesX = static_cast<int>(r.u32());
            overview.tilesZ = static_cast<int>(r.u32());
            overview.stride = static_cast<int>(r.u32());
            overview.skirtDepth = r.f32();
            if (!r.ok || overview.tilesX < 1 || overview.tilesZ < 1 || overview.tilesX > WORLD_TILES_PER_SIDE || overview.tilesZ > WORLD_TILES_PER_SIDE ||
                (overview.stride != 16 && overview.stride != 32)) {
                return TileError::BadSection;
            }
            info = true;
            continue;
        }
        const uint32_t width = r.u32(), height = r.u32(), channels = r.u32(), bits = r.u32();
        if (!r.ok || width == 0 || height == 0 || width > 8193 || height > 8193) return TileError::BadSection;
        const size_t texels = static_cast<size_t>(width) * height;
        if (id == TILE_SECTION_OVERVIEW_HEIGHT) {
            if (channels != 1 || bits != 16 || raw.size() != 16 + texels * 2) return TileError::BadSection;
            overview.heights.resize(texels);
            for (uint16_t& value : overview.heights) value = r.u16();
            heights = true;
        } else {
            if (channels != 4 || bits != 8 || raw.size() != 16 + texels * 4) return TileError::BadSection;
            overview.colour.assign(raw.begin() + 16, raw.end());
            colours = true;
        }
    }
    if (!info || !heights || !colours) return TileError::BadSection;
    if (overview.heights.size() != static_cast<size_t>(overview.width()) * overview.height() || overview.colour.size() != overview.heights.size() * 4) {
        return TileError::Lattice;
    }
    out = std::move(overview);
    return TileError::None;
}

uint64_t jasperengine::terraincore::layerColourHash(std::span<const float> colours) {
    return fnv1a64(colours.data(), colours.size_bytes());
}

WorldPlatformBudget jasperengine::terraincore::worldPlatformBudget(WorldPlatform platform) {
    WorldPlatformBudget budget;
    if (platform == WorldPlatform::Desktop) return budget;
    budget.poolSize = platform == WorldPlatform::Mobile ? 20 : 16;
    budget.maxTileQuads = platform == WorldPlatform::Mobile ? 512 : 256;
    budget.gpuCapBytes = platform == WorldPlatform::Mobile ? (32ull << 20) : (16ull << 20);
    budget.cpuCapBytes = platform == WorldPlatform::Mobile ? (32ull << 20) : (16ull << 20);
    budget.nearTiles = 6;
    budget.uploadBytes = 2ull << 20;
    budget.inFlight = platform == WorldPlatform::WebSingleThread ? 1 : 2;
    budget.lodDistance = 0.5f;
    budget.collisionRadius = platform == WorldPlatform::Mobile ? 96.0f : 64.0f;
    budget.overviewMaxSide = WORLD_OVERVIEW_MAX_MOBILE;
    budget.maxBoxTiles = WORLD_MAX_BOX_MOBILE;
    return budget;
}
