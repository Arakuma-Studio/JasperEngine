// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Include/SplineLayout/FenceLayout.h and
// Engine/Include/Components/Spline/{SplineFence,SplineFenceSpanPiece}.h, Copyright (c) 2026
// Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.
//
// Handedness and the piece axis convention are SplineTileLayout.h's. The Sheared grade is not
// ported: our instances are TRS only (S18 §2 out of scope).

#ifndef SPLINE_SPLINEFENCELAYOUT_H
#define SPLINE_SPLINEFENCELAYOUT_H

#include "Export.h"
#include "math/Vector3.h"
#include "SplineCenterline.h"
#include "SplineTileLayout.h"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace jasperengine{ namespace spline{

    // Span overrides a fence carries (S18 R8).
    inline constexpr uint32_t SPLINE_FENCE_MAX_SPAN_OVERRIDES = 16;

    // How a span meets the grade between the two stations it joins.
    enum class SplineSpanGrade : uint8_t{
        // the span pitches to follow the line between its station bases: rails, hedges
        Racked = 0,
        // the span stays level at the LOWER of its two station altitudes, so the height steps at
        // the stations and no span floats off its downhill end: brick walls, panels, castles
        Stepped
    };

    // What an override substitutes for the span it names.
    enum class SplineSpanOverrideKind : uint8_t{
        // an empty slot of the override table
        None = 0,
        // draw from the gate pool instead of the span pool; an empty gate pool leaves an opening
        Gate,
        // pin this span to a chosen span pool slot, overriding the seeded pick
        ExplicitPiece
    };

    // One authored "this span is a gate" / "this span is that piece", addressed by (the knot
    // opening its run, ordinal within that run's fill) rather than by distance, so it survives a
    // knot drag.
    struct SplineSpanOverride{
        uint32_t knotIndex = 0;
        uint16_t spanOrdinal = 0;
        SplineSpanOverrideKind kind = SplineSpanOverrideKind::None;
        // ExplicitPiece: span pool slot; Gate: gate pool slot
        uint8_t poolSlot = 0;

        bool operator==(const SplineSpanOverride&) const = default;
    };

    // A station is a post position whether or not a post mesh covers it: kits that model the post
    // into each panel's ends ship no post, and an empty post pool still articulates the spans.
    struct SplineFenceStation{
        SplineLayoutPose pose;
        // global ordinal along the whole spline: the post pick's key
        uint32_t index = 0;
        // the knot this station coincides with (meaningful when isKnot)
        uint32_t knot = 0;
        bool isKnot = false;
    };

    // Where one end of a span, or of a crest registered to it, is cut: on the vertical plane
    // through its station's base, perpendicular to the ground bisector of the two spans meeting
    // there (SplineSpanMitre.h). Described in the piece's own frame.
    struct SplineFenceEndPlane{
        // false keeps the end square: no span beyond it, a post over the join, a notch under a
        // millimetre, a turn past the mitre limit, or a span that borrowed its roll
        bool mitred = false;
        // the plane's yaw away from square, radians: the signed turn at the station, positive
        // where the fence turns right
        float turnRadians = 0.0f;
        // the plane's tilt about the piece's right, radians: a racked span's own pitch; zero on
        // level ground and for stepped spans
        float leanRadians = 0.0f;
        // how far inside the piece's modelled end the plane crosses the span's base line, along
        // its own axis in unscaled local metres
        float stationInset = 0.0f;
        // the same at the piece's top (its bounds' highest local y)
        float topInset = 0.0f;
        // the base line's height in the piece's local y (minus the planted lift for a span, minus
        // the wall top for a crest standing on it); only a leaning plane reads it
        float stationLocalY = 0.0f;
    };

    struct SplineFenceSpan{
        SplineLayoutPose pose;
        // scale along the piece's run axis; 1 = its authored length
        float lengthScale = 1.0f;
        // active pool slot drawn: a span pool slot, or a gate pool slot when isGate
        uint32_t poolSlot = 0;
        // an override made this span a gate, drawn from the gate pool
        bool isGate = false;
        // global span ordinal; an opening (a gate with no gate piece) places nothing but keeps
        // its ordinal, so the ordinals after it do not move when the gate pool is filled
        uint32_t index = 0;
        // the knot opening the run, and the position within that run's fill
        uint32_t run = 0;
        uint32_t ordinalInRun = 0;
        // the global index of the station opening the span
        uint32_t station = 0;
        // the planes the two ends are cut on where the span meets a neighbour no post covers; a
        // mitred end was laid longer by its overhang, which lengthScale already counts
        SplineFenceEndPlane start;
        SplineFenceEndPlane end;
    };

    // One piece of the crest row (battlements, coping, lamps along the tops of the spans) or the
    // cap closing a remainder beside a post, a tower or a gate. On the pitch grid a crest piece is
    // whole and centred in its cell; where the crest cell and every span piece of a run share one
    // nominal length the row is registered to the spans instead: one crest per span, in that
    // span's frame and at its lengthScale.
    struct SplineFenceCrest{
        SplineLayoutPose pose;
        // exactly 1 on the pitch grid and for every cap; a registered crest takes its span's
        float lengthScale = 1.0f;
        // active crest pool slot, or the cap pool slot when isCap
        uint32_t poolSlot = 0;
        // drawn from the cap pool to close a remainder, chosen by length and not by the seed
        bool isCap = false;
        // global cell ordinal along the whole spline, counted BEFORE reservations removed any: a
        // tower or a gate removes cells without renumbering the survivors
        uint32_t index = 0;
        uint32_t run = 0;
        uint32_t ordinalInRun = 0;
        // a registered crest's ends are its span's ends; square on the pitch grid and for caps
        SplineFenceEndPlane start;
        SplineFenceEndPlane end;
    };

    // A stretch of fence that a piece the CALLER planted already occupies: the crest cells whose
    // footprint it covers are removed, never shifted. Measured from a knot, signed about it and
    // along the spans: {i, -h, +h} is a tower of half-length h planted at knot i, {i, -h, 0}
    // covers only the run closing on it.
    struct SplineFenceReservation{
        uint32_t knot = 0;
        float startMetres = 0.0f;
        float endMetres = 0.0f;
    };

    struct SplineFenceLayoutParams{
        // Where the knots fall in the centerline, as ascending (fractional) sample indices: one
        // per knot, plus, for a closed spline, the sample that closes the loop. Every knot is a
        // station (S18 D5), so for a centerline built from stations these are the knots' station
        // indices. Two entries describe one run; the posts on authored corners stand here.
        std::span<const float> knotStations;
        // a closed spline's last boundary is its first station: emitted once and yawed by the
        // bisector of both adjoining runs, like every interior corner
        bool closed = false;

        // target metres between stations within a run; runs bend it so their stations land on
        // both knots
        float postPitch = 2.4f;
        // cap on a span's length scale (and, reciprocally, its floor) before the run re-walks
        // with one more or one fewer span
        float spanMaxStretch = 1.25f;
        // optional compression floor independent of spanMaxStretch: a finite value in (0, 1];
        // anything else keeps 1 / spanMaxStretch
        float spanMinScale = 0.0f;
        uint32_t seed = 0;
        SplineSpanGrade spanGrade = SplineSpanGrade::Racked;
        SplinePlantMode plantMode = SplinePlantMode::Pivot;
        // 0 = plumb posts (what a real fence does), 1 = posts lean into the slope
        bool alignToSurfaceNormal = false;
        float slopeBlend = 0.0f;
        // ceiling on how far a STATION may lean from world up; a span's pitch follows its grade
        float maxTiltDegrees = SPLINE_DEFAULT_MAX_TILT_DEGREES;
        SplineTiltClampReport* outTiltClamp = nullptr;

        // The active pools, in slot order. Post: may be empty (no post mesh; every station pose
        // then lies on the draped centerline); its first entry's footprint centres the stations
        // (pool members share footprint and pivot convention). Span: empty = no spans, the
        // stations are walked at the pitch. Gate: what a gate override draws; empty leaves every
        // gate an opening. Crest: empty = no crest row. Cap: offered to the remainders each run
        // leaves beside a post, tower or gate; the longest that fits closes it.
        std::span<const SplinePiece> postPieces;
        std::span<const SplinePiece> spanPieces;
        std::span<const SplinePiece> gatePieces;
        std::span<const SplinePiece> crestPieces;
        std::span<const SplinePiece> capPieces;
        // the fence's span overrides (any order; kind None slots are empty)
        std::span<const SplineSpanOverride> spanOverrides;
        // metres between crest cells, from each run's opening knot; 0 takes the longest crest
        // piece's own length (flush); a smaller value is raised to it and reported
        float crestPitch = 0.0f;
        // stretches the caller's own pieces occupy
        std::span<const SplineFenceReservation> reservations;
        // optional shared facing per knot, indexed by knot: a zero entry keeps the bisector
        std::span<const Vector3> knotForwards;

        uint32_t maxPieces = 4096;
        // re-reads the true surface under each station; null leaves stations on the centerline
        const SplineStationProbe* probe = nullptr;
    };

    struct SplineFenceLayoutResult{
        std::vector<SplineFenceStation> stations;
        std::vector<SplineFenceSpan> spans;
        // the crest row and its caps, in run order, emitted after every station and span: one
        // piece budget, and decoration is what it cuts first
        std::vector<SplineFenceCrest> crests;
        // spans per run's fill, openings included, indexed by the knot opening the run (0 for a
        // skipped run): what an editor reads to keep an override in place after a knot edit
        std::vector<uint32_t> runSpanCounts;
        // author-facing problems, each naming the knot or run it concerns: unsatisfiable
        // stretch, zero-length runs, buried spans, stepped risers taller than their cover,
        // overrides that cannot apply, the piece budget. Never a silent drop.
        std::vector<std::string> validation;
        // spans whose travel stood too near vertical to carry a roll and so borrowed their
        // opening station's frame (they no longer join their two posts)
        uint32_t rollBorrowedSpans = 0;
    };

    // Walks one run per pair of knots, plants a station at every knot and at the pitch fill
    // between them, derives each span's frame from the two stations it joins (racked spans pitch
    // to the grade, stepped spans stay level and step), mitres bare joins, and lays the crest row
    // along the tops of the spans. The conform gaps are held on the WHOLE centerline before it is
    // split into runs.
    JASPERENGINE_API SplineFenceLayoutResult buildFenceLayout(std::span<const SplineCenterSample> center,
                                                              const SplineFenceLayoutParams& params);

    // The scalar knobs read through one gate: non-finite floats take their defaults and finite
    // ones are clamped into their domain (postPitch >= 0.05, spanMaxStretch >= 1, slopeBlend
    // 0..1, crestPitch >= 0). A component compares sanitized values, so a NaN never makes a
    // recipe unequal to itself.
    JASPERENGINE_API SplineFenceLayoutParams sanitizeFenceParams(const SplineFenceLayoutParams& authored);

    // Every piece of a fence layout as TRS poses, appended to `out` in emission order: a post per
    // station (when the post pool has one; drawn by the seeded pick on the station index and
    // centred for its own bounds), then the spans and gates, then the crests and caps. `params`
    // must be the one the layout was built with.
    JASPERENGINE_API void appendFencePiecePoses(const SplineFenceLayoutResult& layout, const SplineFenceLayoutParams& params,
                                                std::vector<SplinePiecePose>& out);

}}

#endif // SPLINE_SPLINEFENCELAYOUT_H
