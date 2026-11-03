// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Include/SplineLayout/SpanMitre.h and
// SeamShear.h (SignedGroundYaw), Copyright (c) 2026 Gamecrafters Guild Limited and contributors,
// MIT License. See NOTICE.md.
//
// The mesh cut that builds a mitred variant is not ported (S18 §2: no mesh bending, TRS
// instances); this module measures the joins, keys the variants and states each cut plane in
// the piece's own frame, which is everything a cutter needs.

#ifndef SPLINE_SPLINESPANMITRE_H
#define SPLINE_SPLINESPANMITRE_H

#include "Export.h"
#include "math/Vector3.h"
#include "SplineFenceLayout.h"
#include "SplineTileLayout.h"
#include <cstdint>
#include <vector>

namespace jasperengine{ namespace spline{

    // Past this deflection a bare join keeps square ends and is reported: a mitre twice the
    // wall's thickness, stated as the turn that produces it (the extrude mitre limit, S18 R6).
    inline constexpr float SPLINE_MITRE_LIMIT_DEGREES = 120.0f;

    // Signed yaw from a to b about +Y after projecting both onto the ground plane, radians;
    // positive where b turns to the RIGHT of a (right = forward x up). Near-vertical and
    // non-finite directions carry no yaw demand (0).
    JASPERENGINE_API float signedGroundYaw(const Vector3& a, const Vector3& b);

    // One span's side of a join: its frame, where its base line meets the station, and the extent
    // of what is laid on it, in the frame's metres.
    struct SplineSpanJoinSide{
        SplineLayoutPose pose;
        // the span's base line at the station: the station base for a racked span; for a stepped
        // span the point level with its lower station, straight below or above the base
        Vector3 baseLineEnd = Vector3(0.0f, 0.0f, 0.0f);
        // half the piece's extent across travel
        float halfThickness = 0.0f;
        // the piece's lowest and highest point along the frame's up from the base line
        float bottom = 0.0f;
        float top = 0.0f;
        // the same for a crest registered to the span (which carries the span's end planes); a
        // zero half-thickness is no crest
        float crestHalfThickness = 0.0f;
        float crestBottom = 0.0f;
        float crestTop = 0.0f;
    };

    // The two end planes of one join, where span `before` closes and span `after` opens. The
    // plane is vertical, through the station, perpendicular to the ground bisector of the two
    // forwards: for two equally thick plumb pieces it holds both the outer and the inner face
    // crossings, so cut on it the faces meet on one edge each; a thinner piece meets a thicker
    // one with the step their thicknesses make.
    struct SplineSpanJoinMeasure{
        // signed ground turn from before to after, positive to the right
        float turnRadians = 0.0f;
        // the gap the two outer faces leave when both ends stay square
        float notchMetres = 0.0f;
        // the planes in each span's own frame; stationInset holds the distance, in the frame's
        // metres, from the span's extended end back to where the plane crosses its base line
        SplineFenceEndPlane beforeEnd;
        SplineFenceEndPlane afterStart;
        // how far past its base line's end each span is laid so its end crosses the plane at
        // every corner of its cross-section, top as well as bottom
        float beforeOverhangMetres = 0.0f;
        float afterOverhangMetres = 0.0f;
    };

    // `station` is the join's station base. False when either span has no ground direction, the
    // two run back on each other, or either frame has no usable forward along the plane.
    JASPERENGINE_API bool measureSpanJoin(const SplineSpanJoinSide& before, const SplineSpanJoinSide& after,
                                          const Vector3& station, SplineSpanJoinMeasure& out);

    // One end plane rounded to the steps at which two joins share one mesh: 0.1 degree of turn,
    // 0.1 mm of inset at the base line and at the piece's top, 0.1 mm of the base line's height
    // (read only for a leaning plane). The lean is carried as the two insets, so the rebuilt plane
    // is within 0.05 mm of the true one over the piece's whole height.
    struct SplineMitreEndKey{
        int32_t turnDeciDegrees = 0;
        int32_t insetTenthMillimetres = 0;
        int32_t topInsetTenthMillimetres = 0;
        int32_t stationLocalYTenthMillimetres = 0;
        bool mitred = false;

        bool operator==(const SplineMitreEndKey&) const = default;
    };

    // Everything a piece's cut variant is a function of besides the piece itself; the length
    // scale is rounded to 1e-3, so the equal spans of a uniform arc share one variant.
    struct SplineMitreShape{
        int32_t lengthScaleThousandths = 1000;
        SplineMitreEndKey start;
        SplineMitreEndKey end;

        bool isMitred() const { return start.mitred || end.mitred; }
        bool operator==(const SplineMitreShape&) const = default;
    };

    JASPERENGINE_API SplineMitreShape quantizeMitreShape(float lengthScale, const SplineFenceEndPlane& start,
                                                         const SplineFenceEndPlane& end);

    // A plane in a piece's local frame (relative to its pivot, unscaled): points p with
    // normal . p > offset lie past the join and are cut away.
    struct SplineMitrePlane{
        Vector3 normal = Vector3(0.0f, 0.0f, 0.0f);
        float offset = 0.0f;
    };

    // The plane one end of a shape cuts the piece on.
    JASPERENGINE_API SplineMitrePlane mitreEndCutPlane(const SplinePiece& piece, const SplineMitreShape& shape, bool atStart);

    inline constexpr uint32_t SPLINE_NO_MITRE_VARIANT = UINT32_MAX;

    // Which pool a variant's piece is drawn from; with the pool slot it names the mesh.
    enum class SplineMitrePieceRole : uint8_t{
        Span,
        Crest,
        Gate
    };

    struct SplineMitreVariantKey{
        SplineMitrePieceRole role = SplineMitrePieceRole::Span;
        uint32_t poolSlot = 0;
        SplineMitreShape shape;

        bool operator==(const SplineMitreVariantKey&) const = default;
    };

    struct SplineMitreVariantPlan{
        // distinct variants in first-use order: the spans in emission order, then the crests
        std::vector<SplineMitreVariantKey> variants;
        // per emitted span and crest: its index in variants, or SPLINE_NO_MITRE_VARIANT to draw
        // the piece as modelled
        std::vector<uint32_t> spanVariants;
        std::vector<uint32_t> crestVariants;
    };

    // Which variant every mitred span and registered crest draws; equal shapes of one piece share
    // a variant, so the equal spans of a uniform arc stay instanced.
    JASPERENGINE_API SplineMitreVariantPlan planMitreVariants(const SplineFenceLayoutResult& layout);

}}

#endif // SPLINE_SPLINESPANMITRE_H
