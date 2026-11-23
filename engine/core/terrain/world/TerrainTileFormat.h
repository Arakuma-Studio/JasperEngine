// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef TERRAIN_TILE_FORMAT_H
#define TERRAIN_TILE_FORMAT_H

// T4 R3, R49, R52, D7: one tile of a Terrain World in memory and on disk (.jtile). Little-endian, a
// version header, then a table of sections, each compressed and checked on its own, so a corrupt
// section fails its tile and nothing else. Unknown sections come back on rewrite unchanged. The same
// input and the same zstd give the same bytes. zstd itself is injected (TileCodec): this folder
// depends on no library (T1 R1).

#include "Export.h"
#include "terrain/world/WorldLattice.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace jasperengine::terraincore {

    inline constexpr uint32_t TILE_FORMAT_VERSION = 1;
    // A section larger than this is corrupt, whatever its header says (N = 1024 heights are 2 MiB)
    inline constexpr uint64_t TILE_SECTION_MAX_BYTES = 64ull << 20;
    inline constexpr uint32_t TILE_MAX_SECTIONS = 256;

    constexpr uint32_t tileSectionId(char a, char b, char c, char d) {
        return static_cast<uint32_t>(static_cast<uint8_t>(a)) | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
               (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
    }
    inline constexpr uint32_t TILE_SECTION_HEIGHT = tileSectionId('H', 'G', 'H', 'T');
    inline constexpr uint32_t TILE_SECTION_BLEND = tileSectionId('B', 'L', 'N', 'D');
    inline constexpr uint32_t TILE_SECTION_HOLE = tileSectionId('H', 'O', 'L', 'E');
    inline constexpr uint32_t TILE_SECTION_DENSITY = tileSectionId('D', 'E', 'N', 'S');
    inline constexpr uint32_t TILE_SECTION_OVERVIEW_HEIGHT = tileSectionId('O', 'V', 'R', 'H');
    inline constexpr uint32_t TILE_SECTION_OVERVIEW_COLOUR = tileSectionId('O', 'V', 'R', 'C');
    inline constexpr uint32_t TILE_SECTION_MINMAX = tileSectionId('M', 'N', 'M', 'X');
    inline constexpr uint32_t TILE_SECTION_EDGE = tileSectionId('E', 'D', 'G', 'E');

    enum class TileCodecId : uint32_t { Raw = 0, Zstd = 1 };
    enum class TileFilter : uint32_t { None = 0, Predictor16 = 1 };

    // A map of one tile: texels row by row, 16-bit channels little-endian. Empty: the tile has none
    // and reads the world's default for it.
    struct TileMap {
        int width = 0, height = 0, channels = 1, bitsPerChannel = 8;
        std::vector<uint8_t> bytes;

        bool empty() const { return bytes.empty(); }
        int bytesPerTexel() const { return channels * (bitsPerChannel >= 16 ? 2 : 1); }
        size_t byteCount() const { return static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(bytesPerTexel()); }
        uint8_t* texel(int x, int y) { return bytes.data() + (static_cast<size_t>(y) * width + x) * bytesPerTexel(); }
        const uint8_t* texel(int x, int y) const { return bytes.data() + (static_cast<size_t>(y) * width + x) * bytesPerTexel(); }
        void allocate(int side, int mapChannels, int bits, uint8_t fill) {
            width = side;
            height = side;
            channels = mapChannels;
            bitsPerChannel = bits;
            bytes.assign(byteCount(), fill);
        }
    };

    // A section this build does not know, kept as stored
    struct TileRawSection {
        uint32_t id = 0, index = 0, codec = 0, filter = 0;
        uint64_t rawBytes = 0, checksum = 0;
        std::vector<uint8_t> stored;
    };

    struct JASPERENGINE_API WorldTile {
        TileCoord coord;
        int tileQuads = 0;
        float spacing = 0.0f;
        uint32_t flags = 0;

        // (N + 3)^2 samples with the one-sample gutter (R3), 16-bit
        TileMap height;
        std::vector<TileMap> blend;     // per blend map; RGBA8, (blendQuads + 1)^2
        TileMap holes;                  // R8, holeTexels^2; empty: solid
        std::vector<TileMap> density;   // per foliage layer, R8
        // R52: the exact tile samples at the overview's stride, and their macro colour
        std::vector<uint16_t> overviewHeight;   // (A + 1)^2
        std::vector<uint8_t> overviewColour;    // (A + 1)^2 RGBA8
        // D4: min and max per node at levels 0..T inside the tile, finest first
        std::vector<uint16_t> minMax;
        // D5: the largest difference between an edge sample and any coarser decimation of its
        // edge, normalized over maxHeight
        float skirtDepth = 0.0f;
        std::vector<TileRawSection> unknown;

        uint16_t sample(int texelX, int texelY) const {
            const uint8_t* at = height.texel(texelX, texelY);
            return static_cast<uint16_t>(at[0] | (at[1] << 8));
        }
        void setSample(int texelX, int texelY, uint16_t value) {
            uint8_t* at = height.texel(texelX, texelY);
            at[0] = static_cast<uint8_t>(value & 0xFF);
            at[1] = static_cast<uint8_t>(value >> 8);
        }
        // R34's CPU accounting
        size_t cpuBytes() const;
    };

    // zstd, supplied by whoever links it. decompress fills out exactly.
    struct TileCodec {
        std::function<bool(std::span<const uint8_t> in, int level, std::vector<uint8_t>& out)> compress;
        std::function<bool(std::span<const uint8_t> in, std::span<uint8_t> out)> decompress;
    };

    enum class TileError : uint8_t { None, Truncated, BadMagic, NewerVersion, BadHeader, BadSection, Checksum, Codec, Lattice, Migration };
    JASPERENGINE_API const char* tileErrorName(TileError error);

    struct TileSectionEntry {
        uint32_t id = 0, index = 0, codec = 0, filter = 0;
        uint64_t offset = 0, storedBytes = 0, rawBytes = 0, checksum = 0;
    };

    struct TileHeader {
        uint32_t version = 0;
        int tileX = 0, tileZ = 0;
        int tileQuads = 0;
        float spacing = 0.0f;
        uint32_t flags = 0;
        std::vector<TileSectionEntry> sections;
    };

    // The container under tiles and the overview (D7): header fields, then sections
    struct TileFileHeader {
        int tileX = 0, tileZ = 0;
        int tileQuads = 0;
        float spacing = 0.0f;
        uint32_t flags = 0;
    };

    // One section before it is filtered and compressed: raw is what the checksum covers
    struct TileSectionPayload {
        uint32_t id = 0, index = 0, filter = 0;
        std::vector<uint8_t> raw;
    };

    struct TileEncodeOptions {
        int level = 9;                  // D7: 9 in the editor, 19 in the cook
        bool keepUnknown = true;        // the cook drops what it does not know
    };

    // The container's writer: sections in order, then unknown ones as stored (null: none)
    JASPERENGINE_API bool writeTileSections(const TileFileHeader& header, std::vector<TileSectionPayload>& sections,
                                            const std::vector<TileRawSection>* unknown, const TileCodec& codec, int level,
                                            std::vector<uint8_t>& out, std::string* error);
    // One section's raw bytes: decompressed, unfiltered and checked
    JASPERENGINE_API TileError readTileSectionRaw(std::span<const uint8_t> file, const TileHeader& header, size_t sectionIndex,
                                                  const TileCodec& codec, std::vector<uint8_t>& raw);

    // R49: the header and table, checked against the file's size. A newer version is NewerVersion.
    JASPERENGINE_API TileError parseTileHeader(std::span<const uint8_t> file, TileHeader& header);
    // One section into data; so decoding splits into jobs per section (R17). Unknown ids land in
    // data.unknown. The lattice of a known section is checked against the header's.
    JASPERENGINE_API TileError decodeTileSection(std::span<const uint8_t> file, const TileHeader& header, size_t sectionIndex, const TileCodec& codec,
                                                 WorldTile& data);
    // Header, every section and migrations. sectionMask: decode only these ids (0: all), D8's
    // overview-only read.
    JASPERENGINE_API TileError decodeTile(std::span<const uint8_t> file, const TileCodec& codec, WorldTile& data, std::span<const uint32_t> only = {});
    JASPERENGINE_API bool encodeTile(const WorldTile& data, const TileCodec& codec, const TileEncodeOptions& options, std::vector<uint8_t>& out,
                                     std::string* error = nullptr);

    // R49: a step of the migration table upgrades data decoded at version from to from + 1
    struct TileMigration {
        uint32_t from = 0;
        bool (*apply)(WorldTile& data, std::string* error) = nullptr;
    };
    // This build's table: empty while version 1 is the only one
    JASPERENGINE_API std::span<const TileMigration> tileMigrations();
    // Runs the steps from version up to target; false when a step is missing or fails
    JASPERENGINE_API bool migrateTile(WorldTile& data, uint32_t version, uint32_t target, std::span<const TileMigration> table, std::string* error);

    // D7's height filter: residuals against left + up - upleft, then the low bytes, then the high
    JASPERENGINE_API void predictorEncode16(std::span<const uint8_t> samples, int width, int height, std::vector<uint8_t>& out);
    JASPERENGINE_API bool predictorDecode16(std::span<const uint8_t> filtered, int width, int height, std::vector<uint8_t>& out);

    // A new tile of the lattice, every sample at baseHeight (normalized 16-bit), no other map
    JASPERENGINE_API void initTile(WorldTile& tile, const WorldLattice& lattice, TileCoord coord, uint16_t baseHeight);

    // The derived sections (R52, D4, D5) from the tile's samples; call after a write
    JASPERENGINE_API void buildMinMax(WorldTile& tile, const WorldLattice& lattice);
    JASPERENGINE_API void buildOverviewHeight(WorldTile& tile, const WorldLattice& lattice);
    JASPERENGINE_API void buildSkirtDepth(WorldTile& tile, const WorldLattice& lattice);
    // R52: macro colour per overview sample, the blend-weighted mean of each layer's colour. colours
    // holds RGBA floats: the base first, then layers 0..layerCount-1.
    JASPERENGINE_API void buildOverviewColour(WorldTile& tile, const WorldLattice& lattice, std::span<const float> colours, int layerCount);
    JASPERENGINE_API void buildDerived(WorldTile& tile, const WorldLattice& lattice, std::span<const float> colours, int layerCount);

    // D3, AC9: the vertex shader's reads of a page, mirrored. texelX and texelZ count page texels,
    // gutter included (a tile's first shared sample is texel 1). The bilerp clamps to the page.
    JASPERENGINE_API float tileHeightAt(const WorldTile& tile, float texelX, float texelZ, float maxHeight);
    // At a vertex on texel (texelX, texelZ), deltaSamples away on each axis, held inside the page
    // symmetrically, so a vertex on a border reads its two neighbours on both tiles alike
    JASPERENGINE_API LatticeNormal tileNormalAt(const WorldTile& tile, int texelX, int texelZ, int deltaSamples, float maxHeight, float spacing);

    struct TileMinMax {
        uint16_t min = 0, max = 0;
    };
    // The node of level (0..T) at cell (cellX, cellZ) of the tile, from minMax; the whole tile at T
    JASPERENGINE_API TileMinMax tileNodeMinMax(const WorldTile& tile, const WorldLattice& lattice, int level, int cellX, int cellZ);

}

#endif // TERRAIN_TILE_FORMAT_H
