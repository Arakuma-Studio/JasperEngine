// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Include/SplineLayout/TileLayout.h,
// PieceAxis.h, PieceBasis.h and PieceEntity.h, and Engine/Include/Components/Spline/
// SplinePlacement.h, Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License.
// See NOTICE.md.
//
// Handedness (S18 D6): JasperEngine is right-handed with +Y up; right = normalize(forward x up),
// so travelling -Z the right of travel is +X, and a positive lateral offset is on the right.
// A piece is laid with its local +Y on the station up and its run axis along travel: a piece run
// along local Z has its +Z on forward and its +X on the LEFT of travel (the engine's own facing,
// Quaternion::lookRotation); a piece run along local X has its +X on forward and its +Z on the
// right. Both are rotations, never mirrors, so asymmetric kit pieces keep their handedness.

#ifndef SPLINE_SPLINETILELAYOUT_H
#define SPLINE_SPLINETILELAYOUT_H

#include "Export.h"
#include "math/AABB.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"
#include "SplineCenterline.h"
#include "SplineJitter.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace jasperengine{ namespace spline{

    // Pieces per pool (Spline Instantiate, each fence role): largest measured kit family + 1.
    inline constexpr uint32_t SPLINE_POOL_CAPACITY = 6;

    // Ceiling, in degrees, on how far a placed piece's up may depart from world up. A tile takes
    // its up from the surface normal and a fence station from its ground, and past this angle
    // neither reads as resting on ground: a fully aligned tile on a near-vertical face turns its
    // width axis skyward and renders as a standing sheet. Height still conforms past it.
    inline constexpr float SPLINE_DEFAULT_MAX_TILT_DEGREES = 35.0f;

    // How Spacing mode distributes stations along the draped length.
    enum class SplinePlacementFit : uint8_t{
        // round the count to fit the length exactly; stations land on both ends
        FitToLength = 0,
        // step exactly `spacing` metres from the start; the last partial step is cut
        FixedPitch
    };

    // Which plane of a placed piece lands on the centerline.
    enum class SplinePlantMode : uint8_t{
        // lift until the piece's lowest point touches: kits authored with bounds-min at ground
        BoundsMin = 0,
        // land the pivot's plane (local y = pivot.y) on the surface and let what is below it bury:
        // kits whose pivot is the authored plant line and whose bounds dip below it (a fence
        // panel's planting skirt)
        Pivot
    };

    // How Spline Instantiate lays its pool along the spline.
    enum class SplineTileMode : uint8_t{
        // one piece every `spacing` metres (fit FitToLength or FixedPitch)
        Spacing = 0,
        // pieces end to end by their own bounds along their run axis; FitToLength stretches them
        // so the run is exactly covered, FixedPitch lays them unscaled and leaves the remainder
        Tile
    };

    // Which of a piece's local horizontal axes runs along the path (see the header comment for
    // the sign convention).
    enum class SplinePieceAxis : uint8_t{
        X,
        Z
    };

    // Which pool a placed piece was drawn from.
    enum class SplinePieceRole : uint8_t{
        Tile = 0,
        Post,
        Span,
        Gate,
        Crest,
        Cap
    };

    // How far one horizontal extent must exceed the other before it counts as the piece's run
    // direction. Below it the piece is near-square and its bounds carry no authoring intent
    // (measured footpath tile 2.62 x 2.38 m: 1.10; standalone posts 1.00); every measured piece
    // that genuinely runs somewhere clears it (1.87 for the shortest hedge, 15.8 for a panel).
    inline constexpr float SPLINE_PIECE_AXIS_DOMINANCE = 1.25f;

    // The single rule for a piece's run axis: orientation and length measurement both go through
    // it. Near-square pieces take Z, the engine's facing axis. A NaN extent compares false and
    // lands on Z.
    inline SplinePieceAxis choosePieceAxis(const Vector3& halfExtents){
        return halfExtents.x > halfExtents.z * SPLINE_PIECE_AXIS_DOMINANCE ? SplinePieceAxis::X : SplinePieceAxis::Z;
    }

    // the piece's authored length: its extent along its run axis
    inline float pieceLength(const Vector3& halfExtents, SplinePieceAxis axis){
        return (axis == SplinePieceAxis::X ? halfExtents.x : halfExtents.z) * 2.0f;
    }

    // Per-local-axis scale of a piece stretched along its run axis.
    struct SplinePieceAxisScale{
        float x = 1.0f;
        float z = 1.0f;
    };

    inline SplinePieceAxisScale makePieceAxisScale(SplinePieceAxis axis, float lengthScale){
        SplinePieceAxisScale scale;
        (axis == SplinePieceAxis::X ? scale.x : scale.z) = lengthScale;
        return scale;
    }

    // One pool entry as the layouts see it: the stub of a model or prefab reference. The engine's
    // asset reference lives in the component; the layouts read only its bounds and pivot and
    // echo poolIndex in every pose they place from it.
    struct SplinePiece{
        // the caller's pool slot (its asset reference), echoed in SplinePiecePose::poolIndex
        uint32_t poolIndex = 0;
        // local bounds, measured from the pivot: centre and half extents
        Vector3 center = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 halfExtents = Vector3(0.0f, 0.0f, 0.0f);
        // the mesh-local point laid on the station (its origin by default)
        Vector3 pivot = Vector3(0.0f, 0.0f, 0.0f);
        // relative pick weight within its pool (equal weights pick uniformly)
        float weight = 1.0f;

        // derived from the extents, never stored beside them: an axis set independently could
        // contradict the bounds it describes
        SplinePieceAxis axis() const { return choosePieceAxis(halfExtents); }
        // authored length along travel
        float length() const { return pieceLength(halfExtents, axis()); }
        // half the extent across travel: how far each side face stands off the path
        float halfThickness() const { return axis() == SplinePieceAxis::X ? halfExtents.z : halfExtents.x; }
        // authored height
        float height() const { return halfExtents.y * 2.0f; }
        // depth of the lowest geometry below the pivot, as a positive magnitude (the planting
        // skirt); negative for bounds that stop above the pivot. BoundsMin lifts by exactly this
        // and the fence bury check allows it, so the two cannot drift.
        float skirtDepth() const { return halfExtents.y - center.y; }
    };

    // A pool entry from a mesh's local bounds (a null box gives zero extents) and its pivot.
    JASPERENGINE_API SplinePiece makeSplinePiece(uint32_t poolIndex, const AABB& localBounds,
                                                 const Vector3& pivot = Vector3(0.0f, 0.0f, 0.0f), float weight = 1.0f);

    // The pool pick for one ordinal: weighted by SplinePiece::weight, uniform (splinePoolSelect)
    // when the weights are equal. pool must not be empty.
    inline uint32_t splinePickPiece(uint32_t seed, SplinePoolRole role, uint32_t ordinal, std::span<const SplinePiece> pool){
        return splinePoolSelectWeighted(seed, role, ordinal, static_cast<uint32_t>(pool.size()),
                                        [pool](uint32_t i){ return pool[i].weight; });
    }

    // Every pose is built from this, so it converts an unusable direction into the fallback
    // rather than into a NaN: a NaN squared length fails every comparison and an infinite one
    // (also from finite components that overflow when squared) would normalize to NaN or zero.
    inline Vector3 normalizedOrFallback(const Vector3& v, const Vector3& fallback){
        const float lenSq = v.dotProduct(v);
        if (!std::isfinite(lenSq) || lenSq < 1.0e-8f){
            return fallback;
        }
        return v * (1.0f / std::sqrt(lenSq));
    }

    // Holds `up` inside a cone of maxTiltDegrees around world up, rotating it back along its own
    // tilt plane so the azimuth of the lean survives. A piece's width axis comes from crossing
    // against this up, so bounding the up is the one place the near-vertical fix belongs. A
    // non-finite ceiling takes the default; an upside-down up (no azimuth) returns world up.
    inline Vector3 clampTiltFromWorldUp(const Vector3& up, float maxTiltDegrees){
        const Vector3 worldUp(0.0f, 1.0f, 0.0f);
        const float maxTilt = std::isfinite(maxTiltDegrees) ? std::clamp(maxTiltDegrees, 0.0f, 90.0f)
                                                            : SPLINE_DEFAULT_MAX_TILT_DEGREES;
        const float cosMax = std::cos(maxTilt * 3.14159265358979f / 180.0f);
        const float cosTilt = up.dotProduct(worldUp);
        if (!std::isfinite(cosTilt) || cosTilt >= cosMax){
            return up;
        }
        const Vector3 across = up - worldUp * cosTilt;
        const float acrossLenSq = across.dotProduct(across);
        if (!std::isfinite(acrossLenSq) || acrossLenSq < 1.0e-8f){
            return worldUp;
        }
        const Vector3 azimuth = across * (1.0f / std::sqrt(acrossLenSq));
        const float sinMax = std::sqrt(std::max(0.0f, 1.0f - cosMax * cosMax));
        return worldUp * cosMax + azimuth * sinMax;
    }

    // What the tilt ceiling held back over one layout, for the inspector warning.
    struct SplineTiltClampReport{
        uint32_t clampedStations = 0;
        // steepest departure from world up seen, before clamping
        float steepestDegrees = 0.0f;
        Vector3 steepestAt = Vector3(0.0f, 0.0f, 0.0f);
    };

    // The frame one piece is laid in, in world space. right/up/forward are orthonormal and
    // right-handed as the engine is (right = forward x up).
    struct SplineLayoutPose{
        // where the piece's pivot goes: base moved by footprint centering and planting
        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 right = Vector3(1.0f, 0.0f, 0.0f);
        Vector3 up = Vector3(0.0f, 1.0f, 0.0f);
        Vector3 forward = Vector3(0.0f, 0.0f, -1.0f);
        // the conformed centerline point the piece stands on, before centering and planting
        Vector3 base = Vector3(0.0f, 0.0f, 0.0f);
        // the station's ordinal along the spline BEFORE dropout removed any (not its index in
        // the result once stations disappear): everything keyed per station keys on this
        uint32_t stationIndex = 0;
    };

    // Where a piece's own local axes point once laid on a frame.
    struct SplinePieceBasis{
        Vector3 localX;
        Vector3 localY;
        Vector3 localZ;
    };

    inline SplinePieceBasis makePieceBasis(const SplineLayoutPose& pose, SplinePieceAxis axis){
        if (axis == SplinePieceAxis::X){
            return {pose.forward, pose.up, pose.right};
        }
        return {pose.right * -1.0f, pose.up, pose.forward};
    }

    // One placed instance, TRS only (InstanceData): world position of the mesh origin, rotation,
    // per-local-axis scale.
    struct SplinePiecePose{
        Vector3 position = Vector3(0.0f, 0.0f, 0.0f);
        Quaternion rotation;
        Vector3 scale = Vector3(1.0f, 1.0f, 1.0f);
        // SplinePiece::poolIndex of the piece placed
        uint32_t poolIndex = 0;
        // the station the piece stands on: its pre-dropout ordinal for Spline Instantiate, the
        // global station index for fence posts, the opening station of a fence span
        uint32_t stationIndex = 0;
        // the ordinal the piece was picked with within its role: the station ordinal for tiles,
        // the global span or crest-cell ordinal for fence spans, gates, crests and caps
        uint32_t ordinal = 0;
        SplinePieceRole role = SplinePieceRole::Tile;
    };

    // The TRS a piece takes on a frame: its run axis on forward (stretched by lengthScale), its
    // +Y on up, scaled uniformly by `scale`, and its mesh origin placed so its pivot is at
    // frame.position. The frame must have been centred for this piece (the layouts do).
    JASPERENGINE_API SplinePiecePose makePiecePose(const SplineLayoutPose& frame, const SplinePiece& piece,
                                                   float lengthScale, float scale = 1.0f);

    // Re-reads the true surface under one station. `at` is the station's interpolated
    // centerline point; on a hit the probe writes the surface altitude and normal under its XZ
    // and returns true, on a miss it leaves both alone. It runs after the stations are fixed and
    // cannot move one in XZ.
    using SplineStationProbe = std::function<bool(const Vector3& at, float& outAltitude, Vector3& outNormal)>;

    // Everything buildTileFrames needs beyond the centerline.
    struct SplineTileFrameParams{
        // metres between stations along the draped centerline (> 0; Spline Instantiate sanitizes)
        float spacing = 2.0f;
        SplinePlacementFit fit = SplinePlacementFit::FitToLength;
        // blend each station's up toward the surface normal (Terrain-HeightAndSlope)
        bool alignToSurfaceNormal = false;
        float slopeBlend = 1.0f;
        // ceiling on the blend result (clampTiltFromWorldUp)
        float maxTiltDegrees = SPLINE_DEFAULT_MAX_TILT_DEGREES;
        // counts the stations the ceiling held; null = don't count
        SplineTiltClampReport* outTiltClamp = nullptr;
        // the piece's local bounds (relative to its pivot): the centre compensates hostile pivots
        // (footprint centering + base lift) and the extents decide the run axis, which sets the
        // chord window
        Vector3 meshBoundsCenter = Vector3(0.0f, 0.0f, 0.0f);
        Vector3 meshBoundsHalfExtents = Vector3(0.0f, 0.0f, 0.0f);
        SplinePlantMode plantMode = SplinePlantMode::BoundsMin;
        uint32_t maxTiles = 2048;
        // caller-supplied station distances along the draped centerline, ascending; empty =
        // derive them from spacing/fit. A caller that solved a fill of mixed-length pieces is the
        // only one that knows where its stations land.
        std::span<const float> stationDistances;
        // optional yaw source per station, parallel to stationDistances: a near-zero entry keeps
        // the chord. A shared corner station takes the bisector of its two runs' end directions.
        std::span<const Vector3> stationForwards;
        // optional per-station piece, parallel to stationDistances, in place of meshBounds*: the
        // bounds that station is chord-windowed, centred and planted with
        std::span<const SplinePiece> stationPieces;
        // optional per-station stretch along the run axis and uniform scale, parallel to
        // stationDistances (1 where empty): the centering follows the piece as it is scaled
        std::span<const float> stationLengthScales;
        std::span<const float> stationScales;
        // re-reads the surface under each station; null leaves stations on the interpolated
        // centerline, whose chords between dense samples cut across crests and kerbs
        const SplineStationProbe* probe = nullptr;

        // per-station variation, all no-ops at their defaults
        // salts every draw below (and the caller's pool pick)
        uint32_t seed = 0;
        // metres a station may slide along the centerline either way, clamped to half the
        // spacing so neighbours can meet but never cross
        float spacingJitterMetres = 0.0f;
        // degrees a station may turn about world up either way
        float yawJitterDegrees = 0.0f;
        // metres to the right of travel every station is moved (negative = left), and the metres
        // it may slide across travel either way; both before the probe, so a moved station
        // conforms to the ground it lands on
        float lateralOffset = 0.0f;
        float lateralJitterMetres = 0.0f;
        // fraction of stations left empty, 0..1
        float dropoutChance = 0.0f;
        // metres at each end over which the keep probability ramps to zero, so a run thins out
        // rather than stopping square; 0 disables
        float endTaperMetres = 0.0f;
    };

    // Walks the draped centerline by true 3D length and builds one frame per station: yaw from
    // the chord across the piece's own footprint (not the instantaneous tangent), pitch and roll
    // from the blended surface normal, the footprint centred on the line and planted per
    // plantMode. Unmeasured samples hold a measured neighbour's altitude first. Stations are
    // thinned (dropout, end taper) and wobbled (spacing, yaw, lateral) by draws that are pure
    // functions of (seed, channel, station ordinal); with every knob at its default no draw is
    // taken and the frames are bit-identical to a build without them. `out` is cleared first;
    // it is left empty when the centerline has fewer than two samples or no length.
    JASPERENGINE_API void buildTileFrames(std::span<const SplineCenterSample> center,
                                          const SplineTileFrameParams& params,
                                          std::vector<SplineLayoutPose>& out);

    // Spline Instantiate in Spacing or Tile mode (S18 R8).
    struct SplineTileLayoutParams{
        SplineTileMode mode = SplineTileMode::Spacing;
        // the pool; members of a Spacing pool should share footprint and pivot convention
        std::span<const SplinePiece> pieces;
        // Spacing mode: metres between pieces along the draped centerline
        float spacing = 2.0f;
        // Spacing mode: FitToLength rounds the count to land on both ends, FixedPitch cuts the
        // last partial step. Tile mode: FitToLength stretches the pieces to cover the length
        // exactly, FixedPitch lays them unscaled while they fit whole.
        SplinePlacementFit fit = SplinePlacementFit::FitToLength;
        // Terrain-HeightAndSlope: tilt each piece toward the surface normal by slopeBlend
        bool alignToSurfaceNormal = false;
        float slopeBlend = 1.0f;
        float maxTiltDegrees = SPLINE_DEFAULT_MAX_TILT_DEGREES;
        SplinePlantMode plantMode = SplinePlantMode::BoundsMin;
        // metres to the right of travel (negative = left)
        float lateralOffset = 0.0f;
        // salts every draw and the pool pick: same seed, same layout
        uint32_t seed = 0;
        float spacingJitterMetres = 0.0f;
        float yawJitterDegrees = 0.0f;
        float lateralJitterMetres = 0.0f;
        // uniform scale drawn per piece from [scaleMin, scaleMax] (1, 1 = none). Tile mode's
        // exact cover holds at scale 1.
        float scaleMin = 1.0f;
        float scaleMax = 1.0f;
        float dropoutChance = 0.0f;
        float endTaperMetres = 0.0f;
        // metres left empty at the start and at the end of the spline
        float startTrimMetres = 0.0f;
        float endTrimMetres = 0.0f;
        uint32_t maxPieces = 2048;
        const SplineStationProbe* probe = nullptr;
        SplineTiltClampReport* outTiltClamp = nullptr;
    };

    // Places the pool along the draped centerline and appends one TRS pose per piece to `out`
    // (cleared first), in station order. Pool picks and every draw are keyed by (seed, channel,
    // station ordinal), so raising the dropout never moves a survivor. Allocates per call, never
    // per piece; reuse `out` across rebuilds to keep its capacity.
    JASPERENGINE_API void buildTileLayout(std::span<const SplineCenterSample> center,
                                          const SplineTileLayoutParams& params,
                                          std::vector<SplinePiecePose>& out);

}}

#endif // SPLINE_SPLINETILELAYOUT_H
