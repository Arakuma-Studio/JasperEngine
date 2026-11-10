// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Ported from Pachislop src/engine/three/bevel.ts by SafEight, used with the author's permission.
//
// Contract (docs/specs/S86-title-3d.md R7)
// - Purpose: bevel cross-sections, shared by the extruder and the editor's profile thumbnails.
//   Unit space: o = 0 inner edge -> 1 outer edge, h = drop in z as a fraction of depth (negative
//   rises). A profile is a list of pieces joined at facet corners; each piece is a polyline whose
//   points carry the unit-space tangent where the surface is smooth, so the extruder gives them
//   analytic normals.
// - Presets flat, chamfer, round, cove, step, inset, tube (p3 §1.2); `curve` in [-1, 1] bends
//   every piece's drop by h <- h^(4^curve). `custom` reads a document Curve (o -> h, both 0..1),
//   sampled at `segments` uniform steps plus every key, split into pieces at corner keys.
// - Tangents are analytic (Pachislop took a 1e-3 central difference); where the derivative is
//   singular (a bent piece's end) the limit direction is taken a hair inside the piece.
// - Thread-safety: pure.

#pragma once

#include "Export.h"
#include "titleart/TitleArtDoc.h"

#include <optional>
#include <string_view>
#include <vector>

namespace jasperengine::titleart::three_d {

    enum class BevelProfileKind : uint8_t { Flat, Chamfer, Round, Cove, Step, Inset, Tube, Custom };
    constexpr int BEVEL_MAX_SEGMENTS = 24;

    JASPERENGINE_API const char* bevelProfileName(BevelProfileKind kind);
    // false for an unknown name
    JASPERENGINE_API bool bevelProfileFromName(std::string_view name, BevelProfileKind& out);
    // every profile name, presets first ("flat" .. "tube", "custom")
    JASPERENGINE_API const std::vector<std::string>& bevelProfileNames();

    struct UnitPoint {
        double o = 0.0, h = 0.0;
        bool smooth = false;            // t is valid (the surface is smooth there)
        double to = 0.0, th = 0.0;      // unit-space tangent (do, dh)
    };
    using UnitPiece = std::vector<UnitPoint>;

    struct BevelShape {
        BevelProfileKind profile = BevelProfileKind::Round;
        int segments = 6;
        double curve = 0.0;
        TitleCurve custom;              // Custom only
    };

    // segments clamped to 1..24
    JASPERENGINE_API int bevelSegments(int segments);
    // the facet count at an output scale: min(24, ceil(segments x sqrt(max(1, outputScale))))
    JASPERENGINE_API int effectiveSegments(int segments, double outputScale);
    // the pieces of a profile in unit space at `segments` facets
    JASPERENGINE_API std::vector<UnitPiece> unitBevel(const BevelShape& shape, int segments);

}
