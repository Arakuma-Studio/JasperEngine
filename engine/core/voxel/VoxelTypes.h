// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef VOXEL_TYPES_H
#define VOXEL_TYPES_H

// The vocabulary of the voxel module (T6): pure values, with no ECS, render or physics type
// anywhere under engine/core/voxel (V1, D1).

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace jasperengine::voxel {

    struct Int3 {
        int x = 0, y = 0, z = 0;
        bool operator==(const Int3& other) const { return x == other.x && y == other.y && z == other.z; }
        bool operator!=(const Int3& other) const { return !(*this == other); }
    };

    struct Vec3d {
        double x = 0.0, y = 0.0, z = 0.0;
    };

    // Half-open is awkward for samples, so a box of samples holds both ends. empty() is max < min.
    struct IntBox {
        Int3 min{1, 1, 1};
        Int3 max{0, 0, 0};
        bool empty() const { return max.x < min.x || max.y < min.y || max.z < min.z; }
        void include(const Int3& p) {
            if (empty()) {
                min = p;
                max = p;
                return;
            }
            if (p.x < min.x) min.x = p.x;
            if (p.y < min.y) min.y = p.y;
            if (p.z < min.z) min.z = p.z;
            if (p.x > max.x) max.x = p.x;
            if (p.y > max.y) max.y = p.y;
            if (p.z > max.z) max.z = p.z;
        }
        void include(const IntBox& other) {
            if (other.empty()) return;
            include(other.min);
            include(other.max);
        }
        bool intersects(const IntBox& other) const {
            return !empty() && !other.empty() && min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y
                && max.y >= other.min.y && min.z <= other.max.z && max.z >= other.min.z;
        }
    };

    inline constexpr int BLOCK_SIZE = 16;
    inline constexpr int BLOCK_SHIFT = 4;
    inline constexpr int BLOCK_SAMPLES = BLOCK_SIZE * BLOCK_SIZE * BLOCK_SIZE;
    inline constexpr int MAX_LEVELS = 4;
    inline constexpr int MAX_SLOTS = 16;

    // R2: distance in voxels times 4096, negative solid, positive air, clamped to +/-32767, a band of
    // about +/-8 voxels, which is the sample spacing of level 3
    inline constexpr int SDF_UNITS_PER_VOXEL = 4096;
    inline constexpr int SDF_MAX = 32767;
    inline constexpr double SDF_BAND_VOXELS = static_cast<double>(SDF_MAX) / SDF_UNITS_PER_VOXEL;
    inline constexpr int16_t SDF_AIR = SDF_MAX;
    // R8: what a Closed volume's outer layer holds at least. Any positive value closes the mesh; one
    // voxel keeps the shell's vertices where a true distance field would put them.
    inline constexpr int16_t SDF_SHELL_AIR = SDF_UNITS_PER_VOXEL;

    enum class BoundsMode : uint8_t { Closed, Open };

    struct VolumeDesc {
        Int3 dimensions{64, 64, 64};   // voxels per axis, multiples of 16
        float voxelSize = 1.0f;
        int levels = 1;
        BoundsMode bounds = BoundsMode::Closed;
    };

    struct BlockKey {
        int level = 0;
        int x = 0, y = 0, z = 0;   // in blocks of this level
        bool operator==(const BlockKey& other) const { return level == other.level && x == other.x && y == other.y && z == other.z; }
        bool operator!=(const BlockKey& other) const { return !(*this == other); }
        bool operator<(const BlockKey& other) const {
            if (level != other.level) return level < other.level;
            if (z != other.z) return z < other.z;
            if (y != other.y) return y < other.y;
            return x < other.x;
        }
    };

    // The six sides, in the order of the mesher's side mask bits
    enum Side : uint8_t { SIDE_NEG_X = 0, SIDE_POS_X, SIDE_NEG_Y, SIDE_POS_Y, SIDE_NEG_Z, SIDE_POS_Z, SIDE_COUNT };

    // R3: slot a (4 bits), slot b (4 bits), weight of b (8 bits, 0 means only a)
    inline constexpr uint16_t packMaterial(int slotA, int slotB, int weightB) {
        return static_cast<uint16_t>((slotA & 0xF) | ((slotB & 0xF) << 4) | ((weightB & 0xFF) << 8));
    }
    inline constexpr int materialSlotA(uint16_t value) { return value & 0xF; }
    inline constexpr int materialSlotB(uint16_t value) { return (value >> 4) & 0xF; }
    inline constexpr int materialWeightB(uint16_t value) { return value >> 8; }

    // R2, R28: the one rounding of any kernel, floor(x + 0.5), so output bytes match on every target
    inline int16_t encodeSdf(double voxels) {
        const double scaled = std::floor(voxels * SDF_UNITS_PER_VOXEL + 0.5);
        if (scaled >= SDF_MAX) return static_cast<int16_t>(SDF_MAX);
        if (scaled <= -SDF_MAX) return static_cast<int16_t>(-SDF_MAX);
        return static_cast<int16_t>(scaled);
    }
    inline double decodeSdf(int16_t value) { return static_cast<double>(value) / SDF_UNITS_PER_VOXEL; }

    inline constexpr int sampleIndex(int x, int y, int z) { return x + (y << BLOCK_SHIFT) + (z << (BLOCK_SHIFT * 2)); }

    // R5: levels above a volume's dimensions are not created
    inline int levelsFor(const Int3& dimensions, int requested) {
        int smallest = dimensions.x;
        if (dimensions.y < smallest) smallest = dimensions.y;
        if (dimensions.z < smallest) smallest = dimensions.z;
        int levels = 1;
        while (levels < MAX_LEVELS && (BLOCK_SIZE << levels) <= smallest) levels++;
        if (requested < 1) requested = 1;
        return requested < levels ? requested : levels;
    }

    // FNV-1a 64: the file's trailing check (V9) and the content hash (R69)
    inline constexpr uint64_t FNV_OFFSET = 14695981039346656037ull;
    inline constexpr uint64_t FNV_PRIME = 1099511628211ull;
    inline uint64_t fnv1a(const void* data, size_t size, uint64_t hash = FNV_OFFSET) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; i++) {
            hash ^= bytes[i];
            hash *= FNV_PRIME;
        }
        return hash;
    }

}

#endif
