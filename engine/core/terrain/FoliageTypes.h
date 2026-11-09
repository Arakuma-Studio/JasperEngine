// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef FOLIAGE_TYPES_H
#define FOLIAGE_TYPES_H

// The data foliage is made of (T5 F3, F11): what every target stores and draws, byte for byte. Pure:
// no ECS and no render types (programme D1).

#include <cstdint>

namespace jasperengine::terraincore {

    // Details place in cells of this size (the scatter's hash takes the cell, so it is part of what a seed means)
    inline constexpr float FOLIAGE_DETAIL_CELL_SIZE = 8.0f;
    // At most this many placement attempts per cell (T5 F2, the historical chunk limit)
    inline constexpr unsigned int FOLIAGE_MAX_CELL_ATTEMPTS = 8192;
    // Trees group in cells of this size, in terrain-local space (T5 R14)
    inline constexpr float FOLIAGE_TREE_CELL_SIZE = 32.0f;
    // Instances shrink to nothing from this share of their distance (T5 R10; today's fade)
    inline constexpr float FOLIAGE_FADE_START = 0.75f;
    // Prototypes per Terrain: a mobile budget, not a data limit (T5 R13)
    inline constexpr int FOLIAGE_MAX_PROTOTYPES = 64;
    // Explicit LOD meshes of a prototype, after its own (T5 R13)
    inline constexpr int FOLIAGE_MAX_EXPLICIT_LODS = 3;
    // The scatter's own version: a changed hash needs a new one (T5 risks)
    inline constexpr uint32_t FOLIAGE_ALGORITHM_VERSION = 1;

    // One foliage instance on the GPU; stored and drawn in terrain-local space
    struct FoliageInstance {
        float position[3];
        float scale;            // the uniform (width) scale
        int16_t rotation[4];    // snorm quaternion, x y z w
        uint8_t color[4];       // unorm linear tint; white is no tint
        uint8_t heightRatio;    // height scale = value / 128 (128 is exactly 1)
        uint8_t windPhase;      // from the hash
        uint8_t windBend;       // 0..255, the layer's or prototype's
        uint8_t densityRank;    // R10's stable subset
    };
    static_assert(sizeof(FoliageInstance) == 32);

    enum TreeRecordFlags : uint8_t {
        TREE_SNAP_TO_TERRAIN = 1 << 0,
        TREE_ALIGN_TO_NORMAL = 1 << 1,
        TREE_HIDDEN_BY_HOLE = 1 << 2
    };
    // The upper five bits of flags: the share of the tilt towards the surface, in 31sts. The stored
    // rotation holds only the yaw, and the tilt comes from the surface when the tree is drawn, so
    // re-snap never rewrites a rotation: undoing a sculpt gives the original bytes (T5 R25).
    inline constexpr int TREE_ALIGN_SHARE_SHIFT = 3;

    inline constexpr uint8_t treeAlignFlags(float share) {
        if (!(share > 0.0f)) return 0;
        const int steps = static_cast<int>((share > 1.0f ? 1.0f : share) * 31.0f + 0.5f);
        return steps > 0 ? static_cast<uint8_t>(TREE_ALIGN_TO_NORMAL | (steps << TREE_ALIGN_SHARE_SHIFT)) : uint8_t(0);
    }

    inline constexpr float treeAlignShare(uint8_t flags) {
        return (flags & TREE_ALIGN_TO_NORMAL) ? static_cast<float>(flags >> TREE_ALIGN_SHARE_SHIFT) / 31.0f : 0.0f;
    }

    // The stored tree, 40 bytes (T5 F3)
    struct TreeRecord {
        uint32_t id;
        float u, v;             // 0..1 over the Terrain
        float height;           // cached surface, 0..1 of maxHeight
        float yOffset;          // world units above the surface
        float scale;
        int16_t rotation[4];    // the yaw; a tilt towards the surface comes from flags
        uint16_t prototypeId;
        uint8_t heightRatio;
        uint8_t flags;          // TreeRecordFlags
        uint8_t color[4];
    };
    static_assert(sizeof(TreeRecord) == 40);

    // (uint64_t(regionKey) << 32) | record id; regionKey 0 for one Terrain (T5 F11)
    using TreeId = uint64_t;

    inline constexpr TreeId makeTreeId(uint32_t regionKey, uint32_t recordId) {
        return (static_cast<uint64_t>(regionKey) << 32) | recordId;
    }
    inline constexpr uint32_t treeIdRegion(TreeId id) { return static_cast<uint32_t>(id >> 32); }
    inline constexpr uint32_t treeIdRecord(TreeId id) { return static_cast<uint32_t>(id & 0xFFFFFFFFu); }

    // Runtime additions take ids from here (T5 F10), so they can never meet a stored one
    inline constexpr uint32_t TREE_RUNTIME_ID_BASE = 0x80000000u;

    // Foliage Quality's values (T5 R21), as FoliageQuality in Engine.h
    enum class FoliageTier : uint8_t { Low = 0, Medium = 1, High = 2 };

    // What a tier scales and caps (the table of T5 R21). Never written back to a Terrain.
    struct FoliageTierLimits {
        float detailDensity = 1.0f;
        float fadeLength = 1.0f;
        unsigned int maxDetailInstances = 400000;
        unsigned int maxMeshTreesCap = 500;
        unsigned int maxVisibleTrees = 30000;
        unsigned int maxDraws = 1200;
        bool detailShadows = true;
        bool impostorBlend = true;
    };

    inline constexpr FoliageTierLimits foliageTierLimits(FoliageTier tier) {
        switch (tier) {
            case FoliageTier::Low: return FoliageTierLimits{0.5f, 0.5f, 30000, 60, 8000, 150, false, false};
            case FoliageTier::Medium: return FoliageTierLimits{0.75f, 1.0f, 120000, 200, 15000, 400, true, true};
            case FoliageTier::High: break;
        }
        return FoliageTierLimits{};
    }

}

#endif // FOLIAGE_TYPES_H
