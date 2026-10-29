// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Field names and defaults follow OpenEngine's SplinePlacement and SplineFence components
// (Engine/Include/Components/Spline/), Copyright (c) 2026 Gamecrafters Guild Limited and
// contributors, MIT License. See NOTICE.md.

#ifndef SPLINE_INSTANTIATE_COMPONENT_H
#define SPLINE_INSTANTIATE_COMPONENT_H

#include "asset/AssetRef.h"
#include "math/Vector2.h"
#include "SplineExtrudeComponent.h"

#include <cstdint>
#include <vector>

namespace jasperengine{

    // how pieces are laid along the spline (S18 R8)
    enum class SplineInstantiateMode{
        Spacing,        // an item every spacing units
        Tile,           // pieces end to end by their bounds
        Fence           // posts and spans (gates, caps)
    };

    enum class SplineSpacingFit{
        FitToLength,    // the spacing stretched so the items exactly span the length
        FixedPitch      // exactly spacing; the remainder is left at the end
    };

    // where a piece stands on the curve
    enum class SplinePlantMode{
        BoundsMin,      // its bounds' bottom on the curve
        Pivot           // its pivot on the curve
    };

    enum class SplineSpanOverrideKind{
        Gate,           // a piece of the gates pool
        Piece           // a given piece of the spans pool
    };

    // a model (.gltf, .glb, .obj: instanced) or a prefab (.prefab: GameObjects) and its share
    struct SplinePoolEntry{
        AssetRef asset;
        float weight = 1.0f;
    };

    // the span of the knot run starting at knot, the span-th of that run
    struct SplineSpanOverride{
        unsigned int knot = 0;
        unsigned int span = 0;
        SplineSpanOverrideKind kind = SplineSpanOverrideKind::Gate;
        unsigned int piece = 0;             // the pool entry (gates for Gate, spans for Piece)
    };

    // Places copies of models or prefabs along the Spline of its GameObject (Unity's
    // SplineInstantiate; S18 R8, R9). Models are drawn instanced in chunks; prefabs become
    // GameObjects (512 at most per component). A piece runs along its longer horizontal bounds
    // axis (near-square pieces along Z); a fence has a post on every knot.
    struct SplineInstantiateComponent{
        static constexpr unsigned int MAX_POOL_ENTRIES = 6;
        static constexpr unsigned int MAX_SPAN_OVERRIDES = 16;

        // component checkbox: a disabled instantiate frees its pieces
        bool enabled = true;

        SplineInstantiateMode mode = SplineInstantiateMode::Spacing;
        std::vector<SplinePoolEntry> items;     // Spacing and Tile
        std::vector<SplinePoolEntry> posts;     // Fence
        std::vector<SplinePoolEntry> spans;
        std::vector<SplinePoolEntry> gates;
        std::vector<SplinePoolEntry> crests;    // along the tops of the spans (battlements, coping)
        std::vector<SplinePoolEntry> caps;      // close the remainders beside posts and gates

        // Spacing
        float spacing = 2.0f;
        SplineSpacingFit fit = SplineSpacingFit::FitToLength;
        // Fence
        float postPitch = 2.4f;
        float spanMaxStretch = 1.25f;
        SplineGrade fenceGrade = SplineGrade::Racked;
        float crestPitch = 0.0f;            // world units between crests (0: none)
        std::vector<SplineSpanOverride> spanOverrides;

        float lateralOffset = 0.0f;
        float verticalOffset = 0.0f;
        // jitter
        float spacingJitter = 0.0f;         // world units
        float yawJitter = 0.0f;             // degrees
        float lateralJitter = 0.0f;         // world units
        Vector2 scaleRange = Vector2(1.0f, 1.0f);
        float dropout = 0.0f;               // 0-1 chance a piece is left out
        uint32_t seed = 0;
        float endTrim = 0.0f;               // world units kept free at both ends
        SplinePlantMode plantMode = SplinePlantMode::BoundsMin;
        float maxTilt = 35.0f;              // degrees from world up at most

        SplineConform conform = SplineConform::None;
        float slopeBlend = 1.0f;
        float stationSpacing = 1.0f;
    };

}

#endif //SPLINE_INSTANTIATE_COMPONENT_H
