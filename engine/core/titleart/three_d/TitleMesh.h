// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S86-title-3d.md R13, R15, R37, R38, §6.3, §6.7)
// - Purpose: the mesh of one 3D title layer as the renderer draws it and `.titlemesh` stores it:
//   indexed 24-byte vertices (layer-space position FLOAT3, octahedral normal SHORT2N, ids
//   USHORT4N: line span, char span, part, slot | role << 12), an optional wrap-UV stream (HALF2),
//   16-bit indices up to 65,535 vertices (else 32-bit), per-slot index ranges, the span table
//   (line and char boxes), the part table (a solid's z range, its rig, whether it shears) and the
//   rigs (the CharAdjust key and the pivot of each rigged character).
// - Slots: a material block is [face, bevel, side, band i front, band i side, ...]; the layer's
//   block starts at slot 0 and each character with material overrides has a block of its own.
//   A slot's material comes from the renderer's slot table, so a material edit touches no vertex.
// - Determinism (R15): equal inputs give byte-identical buffers on one platform.
// - Thread-safety: an immutable value once built (shared_ptr<const TitleMesh>).

#pragma once

#include "Export.h"
#include "titleart/TitleArtTypes.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    // the quality a 3D layer renders at: the editor's bake, or a Realtime tier (§6.7)
    enum class TitleTier : uint8_t { Bake, High, Medium, Low };
    JASPERENGINE_API const char* titleTierName(TitleTier tier);            // bake, high, medium, low
    JASPERENGINE_API bool titleTierFromName(const std::string& name, TitleTier& out);

    // bumped when meshes, cooked environments or the 3D look change meaning (bake recipes, caches)
    constexpr uint32_t TITLE_3D_VERSION = 1;
    // material slots of one layer (blocks of 3 + 2 x bands slots)
    constexpr uint32_t TITLE_3D_MAX_SLOTS = 64;
    constexpr uint32_t TITLE_3D_MAX_RIGS = 64;
    constexpr uint32_t TITLE_3D_MAX_BANDS = 8;

    enum class TitleSlotRole : uint8_t { Face, Bevel, Side, BandFront, BandSide };

    struct TitleMeshVertex {
        float position[3];
        int16_t normal[2];          // signed octahedral (snorm16)
        uint16_t ids[4];            // line span, char span, part, slot | role << 12
    };
    static_assert(sizeof(TitleMeshVertex) == 24, "the 3D title vertex is 24 bytes");

    struct TitleMeshSlot {
        uint32_t firstIndex = 0, indexCount = 0;
        TitleSlotRole role = TitleSlotRole::Face;
        int16_t band = -1;          // BandFront / BandSide: the band's index
        uint16_t block = 0;         // the first slot of its material block (0: the layer's)
    };

    struct TitleMeshSpan {
        float line[4];              // x0, y0, x1, y1 (layer space, y up)
        float chr[4];
    };

    struct TitleMeshPart {
        float zFront = 0, zBack = 0;
        uint16_t rig = 0;           // 0: none, else 1 + the rig's index
        uint16_t flags = 0;         // TITLE_PART_NO_SHEAR
    };
    constexpr uint16_t TITLE_PART_NO_SHEAR = 1u << 0;   // the orb: never sheared by the skew

    struct TitleMeshRig {
        uint32_t charIndex = 0;     // the CharAdjust key
        float pivot[3] = {0, 0, 0}; // em-cell centre, mid-depth (layer space)
    };

    struct TitleMeshStats {
        uint32_t triangles = 0, vertices = 0, solids = 0;
        float buildMs = 0.0f;
        bool fallbackUsed = false;  // an outward band was cut by the Clipper difference (mesh_fallback)
        float acmrBefore = 0.0f, acmrAfter = 0.0f;  // meshoptimizer's vertex-cache miss ratio (32-entry FIFO)
    };

    struct TitleMesh {
        std::vector<TitleMeshVertex> vertices;
        std::vector<uint16_t> wrap;             // half2 per vertex (texture/TexturePrepare.h floatToHalf), or empty
        std::vector<uint16_t> indices16;        // one of the two, by `wideIndices`
        std::vector<uint32_t> indices32;
        bool wideIndices = false;
        std::vector<TitleMeshSlot> slots;       // by slot number
        std::vector<TitleMeshSpan> spans;
        std::vector<TitleMeshPart> parts;
        std::vector<TitleMeshRig> rigs;
        std::array<float, 3> boundsMin{0, 0, 0}, boundsMax{0, 0, 0};
        TitleMeshStats stats;
        TitleTier tier = TitleTier::Bake;
        uint64_t sourceHash = 0;                // the geometry key it was built from
        float pivotZ = 0.0f;                    // mid-depth of the letter's solid (rig pivots)

        size_t indexCount() const { return wideIndices ? indices32.size() : indices16.size(); }
        uint32_t index(size_t i) const { return wideIndices ? indices32[i] : indices16[i]; }
        // vertex, wrap and index bytes (the tiers' mesh memory)
        size_t bytes() const;
    };

    // octahedral snorm16 encoding of a unit normal (includes/octahedral.glsl's mapping) and back
    JASPERENGINE_API std::array<int16_t, 2> encodeTitleNormal(float x, float y, float z);
    JASPERENGINE_API std::array<float, 3> decodeTitleNormal(int16_t x, int16_t y);

}
