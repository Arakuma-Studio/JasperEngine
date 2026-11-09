// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef WORLD_LATTICE_H
#define WORLD_LATTICE_H

// T4 R2, R3, R6, D3, D5, D12: the one lattice of a Terrain World. Every coordinate here is dyadic:
// the spacing and the tile's quad count are powers of two, so a sample's position, a tile's corner and
// a node's centre are exact in float32 out to the world's limit of 16,384 m (D13). The kernels below
// are the C++ mirror of terrain_world_vs.glsl, in the same order of operations, so a test can compare
// both sides of a tile border bit for bit (AC9).

#include <cmath>
#include <cstdint>
#include <string>

namespace jasperengine::terraincore {

    inline constexpr int WORLD_TILE_MIN = -256;
    inline constexpr int WORLD_TILE_MAX = 255;
    inline constexpr int WORLD_TILES_PER_SIDE = WORLD_TILE_MAX - WORLD_TILE_MIN + 1;
    // R6: past this the editor warns once; at it, an add is refused
    inline constexpr float WORLD_EXTENT_WARN = 8192.0f;
    inline constexpr float WORLD_EXTENT_LIMIT = 16384.0f;
    // R7: vertices a side of the full grid, as a Terrain's resolution
    inline constexpr int WORLD_GRID_RESOLUTION = 32;
    // R15
    inline constexpr int WORLD_MAX_FOCUSES = 8;
    // R6: the bounding box of present tiles, in tiles a side, at overview stride 16
    inline constexpr int WORLD_MAX_BOX_DESKTOP = 256;
    inline constexpr int WORLD_MAX_BOX_MOBILE = 128;

    struct TileCoord {
        int16_t x = 0, z = 0;

        TileCoord() = default;
        TileCoord(int tileX, int tileZ): x(static_cast<int16_t>(tileX)), z(static_cast<int16_t>(tileZ)) {}

        bool operator==(const TileCoord& other) const { return x == other.x && z == other.z; }
        bool operator!=(const TileCoord& other) const { return !(*this == other); }
        bool operator<(const TileCoord& other) const { return z != other.z ? z < other.z : x < other.x; }

        // R27's regionKey and the maps' key: x in the high half, both as two's complement
        uint32_t key() const { return (static_cast<uint32_t>(static_cast<uint16_t>(x)) << 16) | static_cast<uint16_t>(z); }
        static TileCoord fromKey(uint32_t key) {
            return TileCoord(static_cast<int16_t>(static_cast<uint16_t>(key >> 16)), static_cast<int16_t>(static_cast<uint16_t>(key & 0xFFFF)));
        }
        bool addressable() const { return x >= WORLD_TILE_MIN && x <= WORLD_TILE_MAX && z >= WORLD_TILE_MIN && z <= WORLD_TILE_MAX; }
    };

    // floor(value / step) for a positive step, exact for negatives
    inline int floorDiv(int value, int step) {
        return value >= 0 ? value / step : -((-value + step - 1) / step);
    }

    inline bool isPowerOfTwo(int value) { return value > 0 && (value & (value - 1)) == 0; }

    inline int log2Exact(int value) {
        int level = 0;
        while ((1 << level) < value) level++;
        return level;
    }

    struct WorldLattice {
        int tileQuads = 512;            // N
        float spacing = 1.0f;           // h
        int gridResolution = WORLD_GRID_RESOLUTION;     // R
        int overviewStride = 16;        // A: overview samples a tile side, less one
        int blendQuads = 0;             // 0: N / 2
        int holeTexels = 0;             // 0: N
        int densityTexels = 0;          // 0: N

        float tileSize() const { return static_cast<float>(tileQuads) * spacing; }
        // T: the quadtree level whose node is exactly a tile
        int tileLevel() const { return log2Exact(tileQuads / gridResolution); }
        float nodeSize(int level) const { return static_cast<float>(gridResolution) * spacing * std::ldexp(1.0f, level); }
        // the level of the four roots that cover the signed grid of 512 x 512 tiles
        int rootLevel() const { return tileLevel() + log2Exact(WORLD_TILES_PER_SIDE / 2); }

        int heightSide() const { return tileQuads + 3; }
        int blendQuadCount() const { return blendQuads > 0 ? blendQuads : tileQuads / 2; }
        int blendSide() const { return blendQuadCount() + 1; }
        int holeSide() const { return holeTexels > 0 ? holeTexels : tileQuads; }
        int densitySide() const { return densityTexels > 0 ? densityTexels : tileQuads; }
        int overviewSide() const { return overviewStride + 1; }
        // tile samples between two overview samples
        int overviewStep() const { return tileQuads / overviewStride; }

        // D12: global height texels are sample + M, never negative
        int heightOrigin() const { return -WORLD_TILE_MIN * tileQuads; }
        int heightLatticeSide() const { return 2 * heightOrigin() + 1; }
        int blendOrigin() const { return -WORLD_TILE_MIN * blendQuadCount(); }
        int blendLatticeSide() const { return 2 * blendOrigin() + 1; }
        int holeOrigin() const { return -WORLD_TILE_MIN * holeSide(); }
        int holeLatticeSide() const { return 2 * holeOrigin(); }
        int densityOrigin() const { return -WORLD_TILE_MIN * densitySide(); }
        int densityLatticeSide() const { return 2 * densityOrigin(); }

        // a tile's minimum corner, entity-local
        float tileOriginX(int tileX) const { return static_cast<float>(tileX) * tileSize(); }
        // sample s (signed, tile 0's corner is 0) sits at s * h
        float localOfSample(int sample) const { return static_cast<float>(sample) * spacing; }

        bool operator==(const WorldLattice& other) const {
            return tileQuads == other.tileQuads && spacing == other.spacing && gridResolution == other.gridResolution &&
                   overviewStride == other.overviewStride && blendQuadCount() == other.blendQuadCount() && holeSide() == other.holeSide() &&
                   densitySide() == other.densitySide();
        }
    };

    // R2: N in {64 .. 1024}, h a power of two in [0.25, 8], A 16 or 32; the maps' own sides divide evenly
    inline bool validateLattice(const WorldLattice& lattice, std::string* why) {
        auto fail = [&](const std::string& message) {
            if (why) *why = message;
            return false;
        };
        if (!isPowerOfTwo(lattice.tileQuads) || lattice.tileQuads < 64 || lattice.tileQuads > 1024) {
            return fail("tile_quads must be 64, 128, 256, 512 or 1024");
        }
        const float h = lattice.spacing;
        bool spacingOk = false;
        for (float allowed = 0.25f; allowed <= 8.0f; allowed *= 2.0f) spacingOk = spacingOk || h == allowed;
        if (!spacingOk) return fail("spacing must be a power of two from 0.25 to 8 metres");
        if (lattice.gridResolution != WORLD_GRID_RESOLUTION) return fail("the grid resolution is 32");
        if (lattice.overviewStride != 16 && lattice.overviewStride != 32) return fail("the overview stride is 16 or 32");
        const int blend = lattice.blendQuadCount();
        if (!isPowerOfTwo(blend) || blend > lattice.tileQuads || blend < 16) return fail("blend quads must be a power of two from 16 to tile_quads");
        if (!isPowerOfTwo(lattice.holeSide()) || lattice.holeSide() > 2 * lattice.tileQuads) return fail("hole texels must be a power of two up to 2 x tile_quads");
        if (!isPowerOfTwo(lattice.densitySide()) || lattice.densitySide() > 2 * lattice.tileQuads) {
            return fail("density texels must be a power of two up to 2 x tile_quads");
        }
        return true;
    }

    // R6: the farthest corner of a tile from the entity origin stays within the hard limit
    inline bool tileWithinLimit(const WorldLattice& lattice, TileCoord tile) {
        if (!tile.addressable()) return false;
        const float size = lattice.tileSize();
        const float lowX = static_cast<float>(tile.x) * size, highX = lowX + size;
        const float lowZ = static_cast<float>(tile.z) * size, highZ = lowZ + size;
        return std::fabs(lowX) <= WORLD_EXTENT_LIMIT && std::fabs(highX) <= WORLD_EXTENT_LIMIT &&
               std::fabs(lowZ) <= WORLD_EXTENT_LIMIT && std::fabs(highZ) <= WORLD_EXTENT_LIMIT;
    }

    inline bool tilePastWarning(const WorldLattice& lattice, TileCoord tile) {
        const float size = lattice.tileSize();
        const float lowX = static_cast<float>(tile.x) * size, lowZ = static_cast<float>(tile.z) * size;
        return std::fabs(lowX) > WORLD_EXTENT_WARN || std::fabs(lowX + size) > WORLD_EXTENT_WARN ||
               std::fabs(lowZ) > WORLD_EXTENT_WARN || std::fabs(lowZ + size) > WORLD_EXTENT_WARN;
    }

    // The tile holding a local position; a point on a border belongs to the higher tile
    inline int tileOfLocal(float local, float tileSize) {
        return static_cast<int>(std::floor(local / tileSize));
    }

    // D3: a 16-bit sample in RG8, high byte in red, so the shader decodes (r * 256 + g) / 65535
    inline void encodeHeightRG8(uint16_t value, uint8_t* rg) {
        rg[0] = static_cast<uint8_t>(value >> 8);
        rg[1] = static_cast<uint8_t>(value & 0xFF);
    }
    inline uint16_t decodeHeightRG8(const uint8_t* rg) {
        return static_cast<uint16_t>((static_cast<uint16_t>(rg[0]) << 8) | rg[1]);
    }
    // The shader's decode, in its order: highp (r * 255 rounded) * 256 + (g * 255 rounded), over 65535
    inline float heightOfSample(uint16_t value, float maxHeight) {
        const float hi = static_cast<float>(value >> 8);
        const float lo = static_cast<float>(value & 0xFF);
        return (hi * 256.0f + lo) / 65535.0f * maxHeight;
    }

    // The vertex shader's manual bilerp (D3), in its order of operations
    inline float latticeBilerp(float h00, float h10, float h01, float h11, float fx, float fz) {
        const float low = h00 + (h10 - h00) * fx;
        const float high = h01 + (h11 - h01) * fx;
        return low + (high - low) * fz;
    }

    // R23: the collider's and the mesh's split of a quad, the diagonal from (low x, low z) to (high x,
    // high z), as Jolt's height field and the grid's index order have it
    inline float latticeTriangle(float h00, float h10, float h01, float h11, float fx, float fz) {
        if (fx >= fz) return h00 + (h10 - h00) * fx + (h11 - h10) * fz;
        return h00 + (h01 - h00) * fz + (h11 - h01) * fx;
    }

    struct LatticeNormal {
        float x = 0.0f, y = 1.0f, z = 0.0f;
        bool operator==(const LatticeNormal& other) const { return x == other.x && y == other.y && z == other.z; }
    };

    // The shader's normal at a vertex: central differences of the four nearest samples, deltaX and
    // deltaZ metres away (D3), normalize((left - right) dz, 2 dx dz, (back - front) dx)
    inline LatticeNormal latticeNormal(float left, float right, float back, float front, float deltaX, float deltaZ) {
        const float x = (left - right) * deltaZ;
        const float y = 2.0f * deltaX * deltaZ;
        const float z = (back - front) * deltaX;
        const float length = std::sqrt(x * x + y * y + z * z);
        return LatticeNormal{x / length, y / length, z / length};
    }

}

#endif // WORLD_LATTICE_H
