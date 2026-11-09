// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "terrain/world/TerrainTileFormat.h"

#include "terrain/TerrainLayers.h"
#include "util/Fnv.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::terraincore;

namespace {

    constexpr uint8_t TILE_MAGIC[8] = {'J', 'T', 'I', 'L', 0, 0, 0, 0};
    constexpr size_t TILE_HEADER_BYTES = 8 + 7 * 4;
    constexpr size_t TILE_ENTRY_BYTES = 4 * 4 + 4 * 8;
    // width, height, channels, bits: every section but EDGE starts with it
    constexpr size_t TILE_PREFIX_BYTES = 16;

    size_t alignTo16(size_t value) { return (value + 15) & ~static_cast<size_t>(15); }

    void putU32(std::vector<uint8_t>& out, uint32_t value) {
        for (int i = 0; i < 4; i++) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
    }
    void putU64(std::vector<uint8_t>& out, uint64_t value) {
        for (int i = 0; i < 8; i++) out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
    }
    void putF32(std::vector<uint8_t>& out, float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        putU32(out, bits);
    }
    uint32_t getU32(const uint8_t* at) {
        return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) | (static_cast<uint32_t>(at[2]) << 16) |
               (static_cast<uint32_t>(at[3]) << 24);
    }
    uint64_t getU64(const uint8_t* at) {
        return static_cast<uint64_t>(getU32(at)) | (static_cast<uint64_t>(getU32(at + 4)) << 32);
    }
    float getF32(const uint8_t* at) {
        const uint32_t bits = getU32(at);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    void putPrefix(std::vector<uint8_t>& out, int width, int height, int channels, int bits) {
        putU32(out, static_cast<uint32_t>(width));
        putU32(out, static_cast<uint32_t>(height));
        putU32(out, static_cast<uint32_t>(channels));
        putU32(out, static_cast<uint32_t>(bits));
    }

    void putU16s(std::vector<uint8_t>& out, const std::vector<uint16_t>& values) {
        for (uint16_t value : values) {
            out.push_back(static_cast<uint8_t>(value & 0xFF));
            out.push_back(static_cast<uint8_t>(value >> 8));
        }
    }

    TileSectionPayload mapSection(uint32_t id, uint32_t index, const TileMap& map, TileFilter filter) {
        TileSectionPayload section;
        section.id = id;
        section.index = index;
        section.filter = static_cast<uint32_t>(filter);
        section.raw.reserve(TILE_PREFIX_BYTES + map.bytes.size());
        putPrefix(section.raw, map.width, map.height, map.channels, map.bitsPerChannel);
        section.raw.insert(section.raw.end(), map.bytes.begin(), map.bytes.end());
        return section;
    }

    bool validLatticeHeader(int tileQuads, float spacing) {
        WorldLattice lattice;
        lattice.tileQuads = tileQuads;
        lattice.spacing = spacing;
        return validateLattice(lattice, nullptr);
    }

    size_t minMaxCount(int tileQuads) {
        const int cells = tileQuads / WORLD_GRID_RESOLUTION;
        size_t count = 0;
        for (int side = cells; side >= 1; side /= 2) count += static_cast<size_t>(side) * side * 2;
        return count;
    }

    struct Prefix {
        int width = 0, height = 0, channels = 0, bits = 0;
    };

    bool readPrefix(const std::vector<uint8_t>& raw, Prefix& prefix) {
        if (raw.size() < TILE_PREFIX_BYTES) return false;
        const uint32_t width = getU32(raw.data()), height = getU32(raw.data() + 4);
        const uint32_t channels = getU32(raw.data() + 8), bits = getU32(raw.data() + 12);
        if (width == 0 || height == 0 || width > 8192 || height > 8192) return false;
        if (channels < 1 || channels > 4 || (bits != 8 && bits != 16)) return false;
        prefix = Prefix{static_cast<int>(width), static_cast<int>(height), static_cast<int>(channels), static_cast<int>(bits)};
        const size_t expected = TILE_PREFIX_BYTES + static_cast<size_t>(width) * height * channels * (bits / 8);
        return raw.size() == expected;
    }

    bool toMap(const std::vector<uint8_t>& raw, const Prefix& prefix, TileMap& map) {
        map.width = prefix.width;
        map.height = prefix.height;
        map.channels = prefix.channels;
        map.bitsPerChannel = prefix.bits;
        map.bytes.assign(raw.begin() + TILE_PREFIX_BYTES, raw.end());
        return true;
    }

    void toU16s(const std::vector<uint8_t>& raw, size_t offset, std::vector<uint16_t>& out) {
        const size_t count = (raw.size() - offset) / 2;
        out.resize(count);
        for (size_t i = 0; i < count; i++) {
            out[i] = static_cast<uint16_t>(raw[offset + i * 2] | (raw[offset + i * 2 + 1] << 8));
        }
    }

    uint16_t sharedSample(const WorldTile& tile, int sx, int sz) {
        return tile.sample(sx + 1, sz + 1);
    }

}

const char* jasperengine::terraincore::tileErrorName(TileError error) {
    switch (error) {
        case TileError::None: return "none";
        case TileError::Truncated: return "truncated";
        case TileError::BadMagic: return "not a tile";
        case TileError::NewerVersion: return "made by a newer version";
        case TileError::BadHeader: return "bad header";
        case TileError::BadSection: return "bad section";
        case TileError::Checksum: return "checksum mismatch";
        case TileError::Codec: return "decompression failed";
        case TileError::Lattice: return "lattice mismatch";
        case TileError::Migration: return "migration failed";
    }
    return "unknown";
}

size_t WorldTile::cpuBytes() const {
    size_t bytes = height.bytes.size() + holes.bytes.size() + overviewHeight.size() * 2 + overviewColour.size() + minMax.size() * 2;
    for (const TileMap& map : blend) bytes += map.bytes.size();
    for (const TileMap& map : density) bytes += map.bytes.size();
    for (const TileRawSection& section : unknown) bytes += section.stored.size();
    return bytes;
}

void jasperengine::terraincore::predictorEncode16(std::span<const uint8_t> samples, int width, int height, std::vector<uint8_t>& out) {
    const size_t count = static_cast<size_t>(width) * height;
    out.assign(count * 2, 0);
    auto at = [&](int x, int y) -> uint32_t {
        if (x < 0 || y < 0) return 0;
        const size_t i = (static_cast<size_t>(y) * width + x) * 2;
        return static_cast<uint32_t>(samples[i]) | (static_cast<uint32_t>(samples[i + 1]) << 8);
    };
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const uint32_t predicted = (at(x - 1, y) + at(x, y - 1) - at(x - 1, y - 1)) & 0xFFFF;
            const uint32_t residual = (at(x, y) - predicted) & 0xFFFF;
            const size_t i = static_cast<size_t>(y) * width + x;
            out[i] = static_cast<uint8_t>(residual & 0xFF);
            out[count + i] = static_cast<uint8_t>(residual >> 8);
        }
    }
}

bool jasperengine::terraincore::predictorDecode16(std::span<const uint8_t> filtered, int width, int height, std::vector<uint8_t>& out) {
    const size_t count = static_cast<size_t>(width) * height;
    if (filtered.size() != count * 2) return false;
    out.assign(count * 2, 0);
    auto at = [&](int x, int y) -> uint32_t {
        if (x < 0 || y < 0) return 0;
        const size_t i = (static_cast<size_t>(y) * width + x) * 2;
        return static_cast<uint32_t>(out[i]) | (static_cast<uint32_t>(out[i + 1]) << 8);
    };
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const size_t i = static_cast<size_t>(y) * width + x;
            const uint32_t residual = static_cast<uint32_t>(filtered[i]) | (static_cast<uint32_t>(filtered[count + i]) << 8);
            const uint32_t predicted = (at(x - 1, y) + at(x, y - 1) - at(x - 1, y - 1)) & 0xFFFF;
            const uint32_t value = (predicted + residual) & 0xFFFF;
            out[i * 2] = static_cast<uint8_t>(value & 0xFF);
            out[i * 2 + 1] = static_cast<uint8_t>(value >> 8);
        }
    }
    return true;
}

TileError jasperengine::terraincore::parseTileHeader(std::span<const uint8_t> file, TileHeader& header) {
    if (file.size() < TILE_HEADER_BYTES) return TileError::Truncated;
    if (std::memcmp(file.data(), TILE_MAGIC, sizeof(TILE_MAGIC)) != 0) return TileError::BadMagic;
    const uint8_t* at = file.data() + 8;
    header.version = getU32(at);
    header.tileX = static_cast<int32_t>(getU32(at + 4));
    header.tileZ = static_cast<int32_t>(getU32(at + 8));
    header.tileQuads = static_cast<int>(getU32(at + 12));
    header.spacing = getF32(at + 16);
    const uint32_t sectionCount = getU32(at + 20);
    header.flags = getU32(at + 24);
    if (header.version == 0) return TileError::BadHeader;
    if (header.version > TILE_FORMAT_VERSION) return TileError::NewerVersion;
    if (!TileCoord(header.tileX, header.tileZ).addressable() || header.tileX != static_cast<int16_t>(header.tileX) ||
        header.tileZ != static_cast<int16_t>(header.tileZ)) {
        return TileError::BadHeader;
    }
    if (!validLatticeHeader(header.tileQuads, header.spacing)) return TileError::BadHeader;
    if (sectionCount > TILE_MAX_SECTIONS) return TileError::BadHeader;
    const size_t tableEnd = TILE_HEADER_BYTES + static_cast<size_t>(sectionCount) * TILE_ENTRY_BYTES;
    if (file.size() < tableEnd) return TileError::Truncated;
    header.sections.resize(sectionCount);
    for (uint32_t i = 0; i < sectionCount; i++) {
        const uint8_t* entry = file.data() + TILE_HEADER_BYTES + static_cast<size_t>(i) * TILE_ENTRY_BYTES;
        TileSectionEntry& section = header.sections[i];
        section.id = getU32(entry);
        section.index = getU32(entry + 4);
        section.codec = getU32(entry + 8);
        section.filter = getU32(entry + 12);
        section.offset = getU64(entry + 16);
        section.storedBytes = getU64(entry + 24);
        section.rawBytes = getU64(entry + 32);
        section.checksum = getU64(entry + 40);
        if (section.storedBytes > TILE_SECTION_MAX_BYTES || section.rawBytes > TILE_SECTION_MAX_BYTES) return TileError::BadSection;
        if (section.offset < tableEnd || section.offset > file.size() || section.storedBytes > file.size() - section.offset) {
            return TileError::Truncated;
        }
        if (section.codec > static_cast<uint32_t>(TileCodecId::Zstd) || section.filter > static_cast<uint32_t>(TileFilter::Predictor16)) {
            return TileError::BadSection;
        }
    }
    return TileError::None;
}

TileError jasperengine::terraincore::readTileSectionRaw(std::span<const uint8_t> file, const TileHeader& header, size_t sectionIndex, const TileCodec& codec,
                                                    std::vector<uint8_t>& raw) {
    if (sectionIndex >= header.sections.size()) return TileError::BadSection;
    const TileSectionEntry& entry = header.sections[sectionIndex];
    const std::span<const uint8_t> stored = file.subspan(static_cast<size_t>(entry.offset), static_cast<size_t>(entry.storedBytes));
    raw.assign(static_cast<size_t>(entry.rawBytes), 0);
    if (entry.codec == static_cast<uint32_t>(TileCodecId::Raw)) {
        if (entry.storedBytes != entry.rawBytes) return TileError::BadSection;
        std::copy(stored.begin(), stored.end(), raw.begin());
    } else {
        if (!codec.decompress || !codec.decompress(stored, raw)) return TileError::Codec;
    }
    if (entry.filter == static_cast<uint32_t>(TileFilter::Predictor16)) {
        Prefix prefix;
        if (raw.size() < TILE_PREFIX_BYTES) return TileError::BadSection;
        prefix.width = static_cast<int>(getU32(raw.data()));
        prefix.height = static_cast<int>(getU32(raw.data() + 4));
        const size_t payload = raw.size() - TILE_PREFIX_BYTES;
        if (prefix.width <= 0 || prefix.height <= 0 || prefix.width > 8192 || prefix.height > 8192 ||
            payload != static_cast<size_t>(prefix.width) * prefix.height * 2) {
            return TileError::BadSection;
        }
        std::vector<uint8_t> plain;
        if (!predictorDecode16(std::span<const uint8_t>(raw).subspan(TILE_PREFIX_BYTES), prefix.width, prefix.height, plain)) {
            return TileError::BadSection;
        }
        std::copy(plain.begin(), plain.end(), raw.begin() + TILE_PREFIX_BYTES);
    }
    if (fnv1a64(raw.data(), raw.size()) != entry.checksum) return TileError::Checksum;
    return TileError::None;
}

TileError jasperengine::terraincore::decodeTileSection(std::span<const uint8_t> file, const TileHeader& header, size_t sectionIndex, const TileCodec& codec,
                                                   WorldTile& data) {
    if (sectionIndex >= header.sections.size()) return TileError::BadSection;
    const TileSectionEntry& entry = header.sections[sectionIndex];
    const std::span<const uint8_t> stored = file.subspan(static_cast<size_t>(entry.offset), static_cast<size_t>(entry.storedBytes));

    const bool known = entry.id == TILE_SECTION_HEIGHT || entry.id == TILE_SECTION_BLEND || entry.id == TILE_SECTION_HOLE ||
                       entry.id == TILE_SECTION_DENSITY || entry.id == TILE_SECTION_OVERVIEW_HEIGHT || entry.id == TILE_SECTION_OVERVIEW_COLOUR ||
                       entry.id == TILE_SECTION_MINMAX || entry.id == TILE_SECTION_EDGE;
    if (!known) {
        TileRawSection raw;
        raw.id = entry.id;
        raw.index = entry.index;
        raw.codec = entry.codec;
        raw.filter = entry.filter;
        raw.rawBytes = entry.rawBytes;
        raw.checksum = entry.checksum;
        raw.stored.assign(stored.begin(), stored.end());
        data.unknown.push_back(std::move(raw));
        return TileError::None;
    }

    std::vector<uint8_t> raw;
    const TileError read = readTileSectionRaw(file, header, sectionIndex, codec, raw);
    if (read != TileError::None) return read;

    const int n = header.tileQuads;
    WorldLattice lattice;
    lattice.tileQuads = n;
    lattice.spacing = header.spacing;

    if (entry.id == TILE_SECTION_EDGE) {
        if (raw.size() != 4) return TileError::BadSection;
        const float depth = getF32(raw.data());
        if (!(depth >= 0.0f && depth <= 1.0f)) return TileError::BadSection;
        data.skirtDepth = depth;
        return TileError::None;
    }

    Prefix prefix;
    if (!readPrefix(raw, prefix)) return TileError::BadSection;
    switch (entry.id) {
        case TILE_SECTION_HEIGHT:
            if (prefix.width != n + 3 || prefix.height != n + 3 || prefix.channels != 1 || prefix.bits != 16) return TileError::Lattice;
            toMap(raw, prefix, data.height);
            break;
        case TILE_SECTION_BLEND:
            if (entry.index >= 8 || prefix.width != prefix.height || prefix.width < 2 || prefix.width > 2 * n + 1 || prefix.channels != 4 ||
                prefix.bits != 8) {
                return TileError::Lattice;
            }
            if (data.blend.size() <= entry.index) data.blend.resize(entry.index + 1);
            toMap(raw, prefix, data.blend[entry.index]);
            break;
        case TILE_SECTION_HOLE:
            if (prefix.width != prefix.height || prefix.width > 2 * n || prefix.channels != 1 || prefix.bits != 8) return TileError::Lattice;
            toMap(raw, prefix, data.holes);
            break;
        case TILE_SECTION_DENSITY:
            if (entry.index >= 64 || prefix.width != prefix.height || prefix.width > 2 * n || prefix.channels != 1 || prefix.bits != 8) {
                return TileError::Lattice;
            }
            if (data.density.size() <= entry.index) data.density.resize(entry.index + 1);
            toMap(raw, prefix, data.density[entry.index]);
            break;
        case TILE_SECTION_OVERVIEW_HEIGHT:
            if (prefix.width != prefix.height || (prefix.width != 17 && prefix.width != 33) || prefix.channels != 1 || prefix.bits != 16) {
                return TileError::Lattice;
            }
            toU16s(raw, TILE_PREFIX_BYTES, data.overviewHeight);
            break;
        case TILE_SECTION_OVERVIEW_COLOUR:
            if (prefix.width != prefix.height || (prefix.width != 17 && prefix.width != 33) || prefix.channels != 4 || prefix.bits != 8) {
                return TileError::Lattice;
            }
            data.overviewColour.assign(raw.begin() + TILE_PREFIX_BYTES, raw.end());
            break;
        case TILE_SECTION_MINMAX:
            if (prefix.height != 1 || prefix.channels != 1 || prefix.bits != 16 || static_cast<size_t>(prefix.width) != minMaxCount(n)) {
                return TileError::Lattice;
            }
            toU16s(raw, TILE_PREFIX_BYTES, data.minMax);
            break;
        default:
            return TileError::BadSection;
    }
    return TileError::None;
}

TileError jasperengine::terraincore::decodeTile(std::span<const uint8_t> file, const TileCodec& codec, WorldTile& data, std::span<const uint32_t> only) {
    TileHeader header;
    const TileError parsed = parseTileHeader(file, header);
    if (parsed != TileError::None) return parsed;
    data = WorldTile();
    data.coord = TileCoord(header.tileX, header.tileZ);
    data.tileQuads = header.tileQuads;
    data.spacing = header.spacing;
    data.flags = header.flags;
    for (size_t i = 0; i < header.sections.size(); i++) {
        if (!only.empty() && std::find(only.begin(), only.end(), header.sections[i].id) == only.end()) continue;
        const TileError error = decodeTileSection(file, header, i, codec, data);
        if (error != TileError::None) return error;
    }
    // the samples are the tile; without them it is not one
    if (only.empty() && data.height.empty()) return TileError::BadSection;
    if (header.version < TILE_FORMAT_VERSION && !migrateTile(data, header.version, TILE_FORMAT_VERSION, tileMigrations(), nullptr)) {
        return TileError::Migration;
    }
    return TileError::None;
}

bool jasperengine::terraincore::writeTileSections(const TileFileHeader& header, std::vector<TileSectionPayload>& pending, const std::vector<TileRawSection>* unknown,
                                              const TileCodec& codec, int level, std::vector<uint8_t>& out, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    struct Stored {
        TileSectionEntry entry;
        std::vector<uint8_t> bytes;
    };
    std::vector<Stored> stored;
    for (TileSectionPayload& section : pending) {
        Stored item;
        item.entry.id = section.id;
        item.entry.index = section.index;
        item.entry.filter = section.filter;
        item.entry.rawBytes = section.raw.size();
        item.entry.checksum = fnv1a64(section.raw.data(), section.raw.size());
        std::vector<uint8_t> payload;
        if (section.filter == static_cast<uint32_t>(TileFilter::Predictor16)) {
            const int width = static_cast<int>(getU32(section.raw.data()));
            const int height = static_cast<int>(getU32(section.raw.data() + 4));
            std::vector<uint8_t> filtered;
            predictorEncode16(std::span<const uint8_t>(section.raw).subspan(TILE_PREFIX_BYTES), width, height, filtered);
            payload.assign(section.raw.begin(), section.raw.begin() + TILE_PREFIX_BYTES);
            payload.insert(payload.end(), filtered.begin(), filtered.end());
        } else {
            payload = std::move(section.raw);
        }
        if (codec.compress) {
            if (!codec.compress(payload, level, item.bytes)) return fail("compression failed");
            item.entry.codec = static_cast<uint32_t>(TileCodecId::Zstd);
        } else {
            item.bytes = std::move(payload);
            item.entry.codec = static_cast<uint32_t>(TileCodecId::Raw);
        }
        item.entry.storedBytes = item.bytes.size();
        stored.push_back(std::move(item));
    }
    if (unknown) {
        for (const TileRawSection& section : *unknown) {
            Stored item;
            item.entry.id = section.id;
            item.entry.index = section.index;
            item.entry.codec = section.codec;
            item.entry.filter = section.filter;
            item.entry.rawBytes = section.rawBytes;
            item.entry.checksum = section.checksum;
            item.entry.storedBytes = section.stored.size();
            item.bytes = section.stored;
            stored.push_back(std::move(item));
        }
    }
    if (stored.size() > TILE_MAX_SECTIONS) return fail("too many sections");

    // byte by byte: GCC 11 misreads an insert of the magic into a cleared vector (-Wstringop-overflow)
    out.clear();
    for (uint8_t byte : TILE_MAGIC) out.push_back(byte);
    putU32(out, TILE_FORMAT_VERSION);
    putU32(out, static_cast<uint32_t>(static_cast<int32_t>(header.tileX)));
    putU32(out, static_cast<uint32_t>(static_cast<int32_t>(header.tileZ)));
    putU32(out, static_cast<uint32_t>(header.tileQuads));
    putF32(out, header.spacing);
    putU32(out, static_cast<uint32_t>(stored.size()));
    putU32(out, header.flags);
    size_t offset = alignTo16(TILE_HEADER_BYTES + stored.size() * TILE_ENTRY_BYTES);
    for (Stored& item : stored) {
        item.entry.offset = offset;
        offset = alignTo16(offset + item.bytes.size());
    }
    for (const Stored& item : stored) {
        putU32(out, item.entry.id);
        putU32(out, item.entry.index);
        putU32(out, item.entry.codec);
        putU32(out, item.entry.filter);
        putU64(out, item.entry.offset);
        putU64(out, item.entry.storedBytes);
        putU64(out, item.entry.rawBytes);
        putU64(out, item.entry.checksum);
    }
    for (const Stored& item : stored) {
        out.resize(static_cast<size_t>(item.entry.offset), 0);
        out.insert(out.end(), item.bytes.begin(), item.bytes.end());
    }
    out.resize(alignTo16(out.size()), 0);
    return true;
}

bool jasperengine::terraincore::encodeTile(const WorldTile& data, const TileCodec& codec, const TileEncodeOptions& options, std::vector<uint8_t>& out,
                                       std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (!validLatticeHeader(data.tileQuads, data.spacing)) return fail("the tile's lattice is not valid");
    if (data.height.empty() || data.height.width != data.tileQuads + 3 || data.height.height != data.tileQuads + 3 || data.height.bitsPerChannel != 16) {
        return fail("the tile has no samples of its lattice");
    }

    std::vector<TileSectionPayload> pending;
    pending.push_back(mapSection(TILE_SECTION_HEIGHT, 0, data.height, TileFilter::Predictor16));
    for (size_t i = 0; i < data.blend.size(); i++) {
        if (!data.blend[i].empty()) pending.push_back(mapSection(TILE_SECTION_BLEND, static_cast<uint32_t>(i), data.blend[i], TileFilter::None));
    }
    if (!data.holes.empty()) pending.push_back(mapSection(TILE_SECTION_HOLE, 0, data.holes, TileFilter::None));
    for (size_t i = 0; i < data.density.size(); i++) {
        if (!data.density[i].empty()) pending.push_back(mapSection(TILE_SECTION_DENSITY, static_cast<uint32_t>(i), data.density[i], TileFilter::None));
    }
    if (!data.overviewHeight.empty()) {
        const int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(data.overviewHeight.size()))));
        TileSectionPayload section;
        section.id = TILE_SECTION_OVERVIEW_HEIGHT;
        section.filter = static_cast<uint32_t>(TileFilter::Predictor16);
        putPrefix(section.raw, side, side, 1, 16);
        putU16s(section.raw, data.overviewHeight);
        pending.push_back(std::move(section));
    }
    if (!data.overviewColour.empty()) {
        const int side = static_cast<int>(std::lround(std::sqrt(static_cast<double>(data.overviewColour.size() / 4))));
        TileSectionPayload section;
        section.id = TILE_SECTION_OVERVIEW_COLOUR;
        putPrefix(section.raw, side, side, 4, 8);
        section.raw.insert(section.raw.end(), data.overviewColour.begin(), data.overviewColour.end());
        pending.push_back(std::move(section));
    }
    if (!data.minMax.empty()) {
        TileSectionPayload section;
        section.id = TILE_SECTION_MINMAX;
        putPrefix(section.raw, static_cast<int>(data.minMax.size()), 1, 1, 16);
        putU16s(section.raw, data.minMax);
        pending.push_back(std::move(section));
    }
    {
        TileSectionPayload section;
        section.id = TILE_SECTION_EDGE;
        putF32(section.raw, data.skirtDepth);
        pending.push_back(std::move(section));
    }

    TileFileHeader header;
    header.tileX = data.coord.x;
    header.tileZ = data.coord.z;
    header.tileQuads = data.tileQuads;
    header.spacing = data.spacing;
    header.flags = data.flags;
    return writeTileSections(header, pending, options.keepUnknown ? &data.unknown : nullptr, codec, options.level, out, error);
}

std::span<const TileMigration> jasperengine::terraincore::tileMigrations() {
    // version 1 is the first: nothing to upgrade from yet
    return {};
}

bool jasperengine::terraincore::migrateTile(WorldTile& data, uint32_t version, uint32_t target, std::span<const TileMigration> table, std::string* error) {
    for (uint32_t at = version; at < target; at++) {
        const TileMigration* step = nullptr;
        for (const TileMigration& candidate : table) {
            if (candidate.from == at) step = &candidate;
        }
        if (!step || !step->apply) {
            if (error) *error = "no migration from tile version " + std::to_string(at);
            return false;
        }
        if (!step->apply(data, error)) return false;
    }
    return true;
}

void jasperengine::terraincore::initTile(WorldTile& tile, const WorldLattice& lattice, TileCoord coord, uint16_t baseHeight) {
    tile = WorldTile();
    tile.coord = coord;
    tile.tileQuads = lattice.tileQuads;
    tile.spacing = lattice.spacing;
    tile.height.allocate(lattice.heightSide(), 1, 16, 0);
    for (int y = 0; y < lattice.heightSide(); y++) {
        for (int x = 0; x < lattice.heightSide(); x++) tile.setSample(x, y, baseHeight);
    }
    buildMinMax(tile, lattice);
    buildOverviewHeight(tile, lattice);
    buildSkirtDepth(tile, lattice);
}

void jasperengine::terraincore::buildMinMax(WorldTile& tile, const WorldLattice& lattice) {
    const int n = lattice.tileQuads;
    const int r = lattice.gridResolution;
    const int cells = n / r;
    tile.minMax.assign(minMaxCount(n), 0);
    // level 0: each cell's R + 1 samples a side, borders shared with the next cell
    size_t at = 0;
    for (int cz = 0; cz < cells; cz++) {
        for (int cx = 0; cx < cells; cx++) {
            uint16_t low = 0xFFFF, high = 0;
            for (int z = cz * r; z <= cz * r + r; z++) {
                for (int x = cx * r; x <= cx * r + r; x++) {
                    const uint16_t value = sharedSample(tile, x, z);
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
            }
            tile.minMax[at++] = low;
            tile.minMax[at++] = high;
        }
    }
    size_t previous = 0;
    for (int side = cells / 2; side >= 1; side /= 2) {
        const int childSide = side * 2;
        for (int cz = 0; cz < side; cz++) {
            for (int cx = 0; cx < side; cx++) {
                uint16_t low = 0xFFFF, high = 0;
                for (int k = 0; k < 4; k++) {
                    const size_t child = previous + (static_cast<size_t>(cz * 2 + k / 2) * childSide + (cx * 2 + k % 2)) * 2;
                    low = std::min(low, tile.minMax[child]);
                    high = std::max(high, tile.minMax[child + 1]);
                }
                tile.minMax[at++] = low;
                tile.minMax[at++] = high;
            }
        }
        previous += static_cast<size_t>(childSide) * childSide * 2;
    }
}

TileMinMax jasperengine::terraincore::tileNodeMinMax(const WorldTile& tile, const WorldLattice& lattice, int level, int cellX, int cellZ) {
    const int cells = lattice.tileQuads / lattice.gridResolution;
    size_t offset = 0;
    int side = cells;
    for (int l = 0; l < level && side > 1; l++) {
        offset += static_cast<size_t>(side) * side * 2;
        side /= 2;
    }
    if (cellX < 0 || cellZ < 0 || cellX >= side || cellZ >= side) return TileMinMax{0, 0xFFFF};
    const size_t at = offset + (static_cast<size_t>(cellZ) * side + cellX) * 2;
    if (at + 1 >= tile.minMax.size()) return TileMinMax{0, 0xFFFF};
    return TileMinMax{tile.minMax[at], tile.minMax[at + 1]};
}

void jasperengine::terraincore::buildOverviewHeight(WorldTile& tile, const WorldLattice& lattice) {
    const int side = lattice.overviewSide();
    const int step = lattice.overviewStep();
    tile.overviewHeight.resize(static_cast<size_t>(side) * side);
    for (int j = 0; j < side; j++) {
        for (int i = 0; i < side; i++) tile.overviewHeight[static_cast<size_t>(j) * side + i] = sharedSample(tile, i * step, j * step);
    }
}

void jasperengine::terraincore::buildSkirtDepth(WorldTile& tile, const WorldLattice& lattice) {
    const int n = lattice.tileQuads;
    std::vector<uint16_t> edge(static_cast<size_t>(n) + 1);
    int worst = 0;
    for (int side = 0; side < 4; side++) {
        for (int k = 0; k <= n; k++) {
            switch (side) {
                case 0: edge[k] = sharedSample(tile, k, 0); break;
                case 1: edge[k] = sharedSample(tile, k, n); break;
                case 2: edge[k] = sharedSample(tile, 0, k); break;
                default: edge[k] = sharedSample(tile, n, k); break;
            }
        }
        for (int step = 2; step <= n; step *= 2) {
            for (int k = 0; k <= n; k++) {
                const int low = (k / step) * step;
                if (low == k) continue;
                const int a = edge[low], b = edge[low + step];
                // integer numerator: the difference to a linear edge, in sample units times step
                const int interpolatedTimesStep = a * step + (b - a) * (k - low);
                const int difference = std::abs(edge[k] * step - interpolatedTimesStep);
                worst = std::max(worst, (difference + step - 1) / step);
            }
        }
    }
    tile.skirtDepth = static_cast<float>(worst) / 65535.0f;
}

void jasperengine::terraincore::buildOverviewColour(WorldTile& tile, const WorldLattice& lattice, std::span<const float> colours, int layerCount) {
    const int side = lattice.overviewSide();
    const int blendQuads = lattice.blendQuadCount();
    tile.overviewColour.assign(static_cast<size_t>(side) * side * 4, 255);
    auto colour = [&](int which, int channel) -> float {
        const size_t index = static_cast<size_t>(which) * 4 + channel;
        return index < colours.size() ? colours[index] : 1.0f;
    };
    for (int j = 0; j < side; j++) {
        for (int i = 0; i < side; i++) {
            // the blend texel at the overview sample: blend samples are shared corners too
            const int bx = static_cast<int>((static_cast<int64_t>(i) * blendQuads + side / 2) / (side - 1));
            const int bz = static_cast<int>((static_cast<int64_t>(j) * blendQuads + side / 2) / (side - 1));
            float weights[TERRAIN_MAX_LAYERS] = {};
            float sum = 0.0f;
            for (int layer = 0; layer < layerCount && layer < TERRAIN_MAX_LAYERS; layer++) {
                const LayerChannel channel = terrainLayerChannel(layer);
                if (channel.map >= static_cast<int>(tile.blend.size()) || tile.blend[channel.map].empty()) continue;
                const TileMap& map = tile.blend[channel.map];
                const int x = std::min(bx, map.width - 1), z = std::min(bz, map.height - 1);
                const float stored = map.texel(x, z)[channel.channel] / 255.0f;
                weights[layer] = channel.inverted ? 1.0f - stored : stored;
                sum += weights[layer];
            }
            const float scale = sum > 1.0f ? 1.0f / sum : 1.0f;
            const float baseWeight = sum < 1.0f ? 1.0f - sum : 0.0f;
            uint8_t* out = tile.overviewColour.data() + (static_cast<size_t>(j) * side + i) * 4;
            for (int c = 0; c < 3; c++) {
                float value = colour(0, c) * baseWeight;
                for (int layer = 0; layer < layerCount && layer < TERRAIN_MAX_LAYERS; layer++) value += colour(layer + 1, c) * weights[layer] * scale;
                value = std::min(std::max(value, 0.0f), 1.0f);
                out[c] = static_cast<uint8_t>(std::lround(value * 255.0f));
            }
            out[3] = 255;
        }
    }
}

void jasperengine::terraincore::buildDerived(WorldTile& tile, const WorldLattice& lattice, std::span<const float> colours, int layerCount) {
    buildMinMax(tile, lattice);
    buildOverviewHeight(tile, lattice);
    buildSkirtDepth(tile, lattice);
    buildOverviewColour(tile, lattice, colours, layerCount);
}

float jasperengine::terraincore::tileHeightAt(const WorldTile& tile, float texelX, float texelZ, float maxHeight) {
    const int last = tile.height.width - 1;
    const float fx0 = std::floor(texelX), fz0 = std::floor(texelZ);
    const int x0 = std::min(std::max(static_cast<int>(fx0), 0), last - 1);
    const int z0 = std::min(std::max(static_cast<int>(fz0), 0), last - 1);
    const float fx = std::min(std::max(texelX - static_cast<float>(x0), 0.0f), 1.0f);
    const float fz = std::min(std::max(texelZ - static_cast<float>(z0), 0.0f), 1.0f);
    return latticeBilerp(heightOfSample(tile.sample(x0, z0), maxHeight), heightOfSample(tile.sample(x0 + 1, z0), maxHeight),
                         heightOfSample(tile.sample(x0, z0 + 1), maxHeight), heightOfSample(tile.sample(x0 + 1, z0 + 1), maxHeight), fx, fz);
}

LatticeNormal jasperengine::terraincore::tileNormalAt(const WorldTile& tile, int texelX, int texelZ, int deltaSamples, float maxHeight, float spacing) {
    const int last = tile.height.width - 1;
    const int dx = std::max(1, std::min(deltaSamples, std::min(texelX, last - texelX)));
    const int dz = std::max(1, std::min(deltaSamples, std::min(texelZ, last - texelZ)));
    auto height = [&](int x, int z) { return heightOfSample(tile.sample(std::min(std::max(x, 0), last), std::min(std::max(z, 0), last)), maxHeight); };
    return latticeNormal(height(texelX - dx, texelZ), height(texelX + dx, texelZ), height(texelX, texelZ - dz), height(texelX, texelZ + dz),
                         static_cast<float>(dx) * spacing, static_cast<float>(dz) * spacing);
}
