// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Source/TileLayout.cpp and
// PieceEntity.cpp, Copyright (c) 2026 Gamecrafters Guild Limited and contributors, MIT License.
// See NOTICE.md.

#include "SplineTileLayout.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    constexpr float TILE_PI = 3.14159265358979f;

    // The chord window never collapses below this fraction of the spacing, so near-degenerate
    // bounds still yaw from a finite chord.
    constexpr float CHORD_HALF_SPACING_FRACTION = 0.25f;

    // A station may slide at most this fraction of the spacing either way: two neighbours can
    // land on one point but cannot swap, so every chord keeps its direction.
    constexpr float MAX_SPACING_JITTER_FRACTION = 0.5f;

    // Spline Instantiate's floor on the spacing and on a piece's length: a smaller value would
    // divide the run into an unbounded count.
    constexpr float MIN_TILE_SPACING = 0.05f;
    constexpr float MIN_TILE_PIECE_LENGTH = 1.0e-3f;
    constexpr float DEFAULT_TILE_SPACING = 2.0f;

    // Tile mode FixedPitch: a piece overhanging the run end by less than this still fits.
    constexpr float TILE_FIT_TOLERANCE = 1.0e-4f;

    // Authored floats reach the layout unsanitized and NaN passes std::clamp straight through.
    // Every knob below is a magnitude, so a non-finite one means "no opinion".
    float sanitizedMagnitude(float value, float maxValue){
        if (!std::isfinite(value)){
            return 0.0f;
        }
        return std::clamp(value, 0.0f, maxValue);
    }

    // Linear sample of the polyline at a distance along it (cum is its cumulative length).
    SplineCenterSample sampleAtDistance(std::span<const SplineCenterSample> samples, const std::vector<float>& cum, float s){
        s = std::clamp(s, 0.0f, cum.back());
        const auto it = std::lower_bound(cum.begin(), cum.end(), s);
        const size_t hi = std::clamp<size_t>(static_cast<size_t>(it - cum.begin()), size_t{1}, cum.size() - 1u);
        const size_t lo = hi - 1u;
        const float span = cum[hi] - cum[lo];
        const float t = span > 0.0f ? (s - cum[lo]) / span : 0.0f;
        SplineCenterSample out;
        out.position = samples[lo].position + (samples[hi].position - samples[lo].position) * t;
        out.normal = normalizedOrFallback(samples[lo].normal + (samples[hi].normal - samples[lo].normal) * t,
                                          Vector3(0.0f, 1.0f, 0.0f));
        return out;
    }

    void cumulativeLengths(std::span<const SplineCenterSample> samples, std::vector<float>& cum){
        cum.assign(samples.size(), 0.0f);
        for (size_t i = 1; i < samples.size(); i++){
            const Vector3 d = samples[i].position - samples[i - 1].position;
            cum[i] = cum[i - 1] + std::sqrt(d.dotProduct(d));
        }
    }

    // The draped polyline between two distances along it, both as exact endpoints.
    void sliceByDistance(std::span<const SplineCenterSample> samples, const std::vector<float>& cum,
                         float s0, float s1, std::vector<SplineCenterSample>& out){
        out.clear();
        out.push_back(sampleAtDistance(samples, cum, s0));
        for (size_t i = 0; i < samples.size(); i++){
            if (cum[i] <= s0){
                continue;
            }
            if (cum[i] >= s1){
                break;
            }
            out.push_back(samples[i]);
        }
        out.push_back(sampleAtDistance(samples, cum, s1));
    }

    // One Spline Instantiate run, solved before any frame is built: where each station stands,
    // which piece it draws and how it is scaled.
    struct TileRun{
        std::vector<float> distances;
        std::vector<SplinePiece> pieces;
        std::vector<float> lengthScales;
        std::vector<float> scales;
        // the shortest piece as laid: the chord floor and the spacing-jitter clamp of Tile mode
        float shortestLaid = 0.0f;
    };

    void solveSpacingRun(float runLength, float spacing, const SplineTileLayoutParams& params, TileRun& run){
        uint32_t count = 0;
        if (params.fit == SplinePlacementFit::FixedPitch){
            count = std::min<uint32_t>(static_cast<uint32_t>(runLength / spacing) + 1u, params.maxPieces);
        }else{
            const uint32_t rounded = static_cast<uint32_t>(std::lround(runLength / spacing)) + 1u;
            count = std::min<uint32_t>(std::max<uint32_t>(rounded, 2u), params.maxPieces);
        }
        run.distances.reserve(count);
        run.pieces.reserve(count);
        for (uint32_t i = 0; i < count; i++){
            if (params.fit == SplinePlacementFit::FixedPitch){
                run.distances.push_back(static_cast<float>(i) * spacing);
            }else{
                run.distances.push_back(count > 1u ? runLength * static_cast<float>(i) / static_cast<float>(count - 1u) : 0.0f);
            }
            run.pieces.push_back(params.pieces[splinePickPiece(params.seed, SplinePoolRole::Tile, i, params.pieces)]);
        }
        run.shortestLaid = spacing;
    }

    // Pieces end to end by their own lengths. FixedPitch lays each unscaled while it fits whole;
    // FitToLength keeps every piece whose centre still lands inside the run and stretches them
    // all by one factor so the last one closes the run exactly.
    void solveTileRun(float runLength, const SplineTileLayoutParams& params, TileRun& run){
        const bool fit = params.fit == SplinePlacementFit::FitToLength;
        float cumulative = 0.0f;
        float shortest = 0.0f;
        for (uint32_t k = 0; k < params.maxPieces; k++){
            const SplinePiece& piece = params.pieces[splinePickPiece(params.seed, SplinePoolRole::Tile, k, params.pieces)];
            const float length = std::max(piece.length(), MIN_TILE_PIECE_LENGTH);
            if (fit ? (k > 0u && cumulative + length * 0.5f > runLength)
                    : (cumulative + length > runLength + TILE_FIT_TOLERANCE)){
                break;
            }
            run.distances.push_back(cumulative + length * 0.5f);
            run.pieces.push_back(piece);
            shortest = k == 0u ? length : std::min(shortest, length);
            cumulative += length;
        }
        if (run.distances.empty()){
            return;
        }
        float stretch = 1.0f;
        if (fit){
            stretch = runLength / cumulative;
            for (float& distance : run.distances){
                distance *= stretch;
            }
            run.lengthScales.assign(run.distances.size(), stretch);
        }
        run.shortestLaid = shortest * stretch;
    }

    void drawScales(const SplineTileLayoutParams& params, TileRun& run){
        if (!std::isfinite(params.scaleMin) || !std::isfinite(params.scaleMax) ||
            !(params.scaleMin > 0.0f) || !(params.scaleMax > 0.0f) ||
            (params.scaleMin == 1.0f && params.scaleMax == 1.0f)){
            return;
        }
        const float lo = std::min(params.scaleMin, params.scaleMax);
        const float hi = std::max(params.scaleMin, params.scaleMax);
        run.scales.resize(run.distances.size());
        for (size_t i = 0; i < run.scales.size(); i++){
            run.scales[i] = lo + (hi - lo) * splineJitterUnit(params.seed, SplineJitterChannel::Scale, static_cast<uint32_t>(i));
        }
    }

}

SplinePiece spline::makeSplinePiece(uint32_t poolIndex, const AABB& localBounds, const Vector3& pivot, float weight){
    SplinePiece piece;
    piece.poolIndex = poolIndex;
    piece.pivot = pivot;
    piece.weight = weight;
    if (localBounds.isFinite()){
        piece.center = localBounds.getCenter() - pivot;
        piece.halfExtents = localBounds.getHalfSize();
    }
    return piece;
}

SplinePiecePose spline::makePiecePose(const SplineLayoutPose& frame, const SplinePiece& piece, float lengthScale, float scale){
    const SplinePieceAxis axis = piece.axis();
    const SplinePieceBasis basis = makePieceBasis(frame, axis);
    const SplinePieceAxisScale axisScale = makePieceAxisScale(axis, lengthScale);

    SplinePiecePose pose;
    pose.rotation = Quaternion(basis.localX, basis.localY, basis.localZ);
    pose.rotation.normalize();
    pose.scale = Vector3(axisScale.x * scale, scale, axisScale.z * scale);
    // frame.position is where the pivot goes; the mesh origin stands off it by the scaled pivot
    pose.position = frame.position - basis.localX * (piece.pivot.x * pose.scale.x) -
                    basis.localY * (piece.pivot.y * pose.scale.y) - basis.localZ * (piece.pivot.z * pose.scale.z);
    pose.poolIndex = piece.poolIndex;
    pose.stationIndex = frame.stationIndex;
    return pose;
}

void spline::buildTileFrames(std::span<const SplineCenterSample> center, const SplineTileFrameParams& params,
                             std::vector<SplineLayoutPose>& out){
    out.clear();
    if (center.size() < 2u){
        return;
    }

    // only a centerline that actually has a gap pays for the patched copy
    const bool anyGap = std::any_of(center.begin(), center.end(), [](const SplineCenterSample& s){ return !s.surfaceValid; });
    std::vector<SplineCenterSample> patched;
    if (anyGap){
        patched.assign(center.begin(), center.end());
        holdSurfaceAcrossGaps(patched);
    }
    const std::span<const SplineCenterSample> samples = anyGap ? std::span<const SplineCenterSample>(patched) : center;

    std::vector<float> cum;
    cumulativeLengths(samples, cum);
    const float drapedLength = cum.back();
    if (drapedLength <= 0.0f){
        return;
    }

    const Vector3 worldUp(0.0f, 1.0f, 0.0f);

    std::vector<float> stations;
    if (!params.stationDistances.empty()){
        // a caller that solved its own fill owns the stations outright
        const size_t count = std::min<size_t>(params.stationDistances.size(), params.maxTiles);
        stations.assign(params.stationDistances.begin(), params.stationDistances.begin() + count);
    }else if (params.fit == SplinePlacementFit::FixedPitch){
        const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(drapedLength / params.spacing) + 1u, params.maxTiles);
        stations.reserve(count);
        for (uint32_t i = 0; i < count; i++){
            stations.push_back(static_cast<float>(i) * params.spacing);
        }
    }else{
        // FitToLength: rounded count, stations land on both ends
        const uint32_t rounded = static_cast<uint32_t>(std::lround(drapedLength / params.spacing)) + 1u;
        const uint32_t count = std::min<uint32_t>(std::max<uint32_t>(rounded, 2u), params.maxTiles);
        stations.reserve(count);
        for (uint32_t i = 0; i < count; i++){
            stations.push_back(count > 1u ? drapedLength * static_cast<float>(i) / static_cast<float>(count - 1u) : 0.0f);
        }
    }

    // Per-station variation, sanitized once. Each gate keeps the default path from even taking
    // a draw, so the frames are then bit-identical to a build without it.
    const float spacingJitter = sanitizedMagnitude(params.spacingJitterMetres, MAX_SPACING_JITTER_FRACTION * params.spacing);
    const float yawJitterRadians = sanitizedMagnitude(params.yawJitterDegrees, 180.0f) * TILE_PI / 180.0f;
    const float lateralJitter = sanitizedMagnitude(params.lateralJitterMetres, drapedLength);
    const float lateralOffset = std::isfinite(params.lateralOffset) ? params.lateralOffset : 0.0f;
    const float dropoutChance = sanitizedMagnitude(params.dropoutChance, 1.0f);
    const float taperMetres = sanitizedMagnitude(params.endTaperMetres, drapedLength);
    const bool anyThinning = dropoutChance > 0.0f || taperMetres > 0.0f;

    out.reserve(stations.size());
    for (size_t stationOrdinal = 0; stationOrdinal < stations.size(); stationOrdinal++){
        // the ordinal, not the surviving index: it keys every draw and the caller's pool pick,
        // so a dropped station leaves its neighbours exactly as they were
        const uint32_t stationIndex = static_cast<uint32_t>(stationOrdinal);
        const float nominal = stations[stationOrdinal];

        if (anyThinning){
            // dropout thins the whole run evenly; the taper multiplies that by a ramp reaching
            // zero AT each end, so a tapered run stops by fading
            float keep = 1.0f - dropoutChance;
            if (taperMetres > 0.0f){
                const float fromNearestEnd = std::min(nominal, drapedLength - nominal);
                keep *= std::clamp(fromNearestEnd / taperMetres, 0.0f, 1.0f);
            }
            if (splineJitterUnit(params.seed, SplineJitterChannel::Dropout, stationIndex) >= keep){
                continue;
            }
        }

        // the piece this station lays, and how it is scaled
        const bool ownPiece = stationOrdinal < params.stationPieces.size();
        const Vector3 boundsCenter = ownPiece ? params.stationPieces[stationOrdinal].center : params.meshBoundsCenter;
        const Vector3 boundsHalf = ownPiece ? params.stationPieces[stationOrdinal].halfExtents : params.meshBoundsHalfExtents;
        const float lengthScale = stationOrdinal < params.stationLengthScales.size() ? params.stationLengthScales[stationOrdinal] : 1.0f;
        const float scale = stationOrdinal < params.stationScales.size() ? params.stationScales[stationOrdinal] : 1.0f;
        const SplinePieceAxis pieceAxis = choosePieceAxis(boundsHalf);
        const SplinePieceAxisScale axisScale = makePieceAxisScale(pieceAxis, lengthScale);

        // Yaw comes from the chord across the piece's own footprint: a rigid piece is then the
        // chord of its own arc window, which removes the mid-piece sagitta on bends. The window
        // is half the piece's extent along its run axis.
        const float chordHalf = std::max(pieceLength(boundsHalf, pieceAxis) * 0.5f * lengthScale * scale,
                                         CHORD_HALF_SPACING_FRACTION * params.spacing);

        // BoundsMin lifts the origin until the lowest point lands on the surface; Pivot lands
        // the pivot itself and lets anything below it bury (a skirted mesh)
        const float meshBaseLift = params.plantMode == SplinePlantMode::Pivot ? 0.0f : boundsHalf.y - boundsCenter.y;

        // jitter along the run moves where the station stands, before anything is sampled, so
        // the piece yaws from the chord around where it actually is
        float s = nominal;
        if (spacingJitter > 0.0f){
            s += spacingJitter * splineJitterSigned(params.seed, SplineJitterChannel::Spacing, stationIndex);
        }

        SplineCenterSample c = sampleAtDistance(samples, cum, s);
        const SplineCenterSample a = sampleAtDistance(samples, cum, s - chordHalf);
        const SplineCenterSample b = sampleAtDistance(samples, cum, s + chordHalf);
        const Vector3 chord = b.position - a.position;
        // ground-plane travel direction from the chord; a degenerate chord (vertical or zero)
        // falls back to the engine's forward. A caller-supplied direction wins over the chord; a
        // zero entry means "no opinion".
        Vector3 forward = normalizedOrFallback(Vector3(chord.x, 0.0f, chord.z), Vector3(0.0f, 0.0f, -1.0f));
        if (stationOrdinal < params.stationForwards.size()){
            const Vector3& authored = params.stationForwards[stationOrdinal];
            forward = normalizedOrFallback(Vector3(authored.x, 0.0f, authored.z), forward);
        }
        if (yawJitterRadians > 0.0f){
            // a right-handed turn about world up (+Z toward +X for a positive angle), applied to
            // the ground direction before the surface basis is built around it
            const float angle = yawJitterRadians * splineJitterSigned(params.seed, SplineJitterChannel::Yaw, stationIndex);
            const float cosA = std::cos(angle);
            const float sinA = std::sin(angle);
            forward = normalizedOrFallback(Vector3(forward.x * cosA + forward.z * sinA, 0.0f, forward.z * cosA - forward.x * sinA),
                                           forward);
        }
        if (lateralJitter > 0.0f || lateralOffset != 0.0f){
            // forward x worldUp is the right of travel
            const Vector3 right = normalizedOrFallback(forward.crossProduct(worldUp), Vector3(1.0f, 0.0f, 0.0f));
            float lateral = lateralOffset;
            if (lateralJitter > 0.0f){
                lateral += lateralJitter * splineJitterSigned(params.seed, SplineJitterChannel::Lateral, stationIndex);
            }
            c.position = c.position + right * lateral;
        }

        if (params.probe && *params.probe){
            // the polyline is a chord approximation, so interpolating it cuts any feature between
            // two dense samples: re-read the surface under the station, after the slides above
            float altitude = c.position.y;
            Vector3 normal = c.normal;
            if ((*params.probe)(c.position, altitude, normal)){
                c.position.y = altitude;
                c.normal = normalizedOrFallback(normal, worldUp);
            }
        }

        Vector3 up = worldUp;
        if (params.alignToSurfaceNormal){
            const float blend = std::clamp(params.slopeBlend, 0.0f, 1.0f);
            up = normalizedOrFallback(worldUp + (c.normal - worldUp) * blend, worldUp);
            // ground steep enough to lay a piece on its side is held at the ceiling: right comes
            // from crossing against up, so an up leaning this far points the WIDTH axis skyward
            const Vector3 held = clampTiltFromWorldUp(up, params.maxTiltDegrees);
            if (params.outTiltClamp && held.dotProduct(up) < 1.0f - 1.0e-6f){
                SplineTiltClampReport& report = *params.outTiltClamp;
                report.clampedStations++;
                const float tiltDegrees = std::acos(std::clamp(up.dotProduct(worldUp), -1.0f, 1.0f)) * 180.0f / TILE_PI;
                if (tiltDegrees > report.steepestDegrees){
                    report.steepestDegrees = tiltDegrees;
                    report.steepestAt = c.position;
                }
            }
            up = held;
        }

        SplineLayoutPose pose;
        pose.stationIndex = stationIndex;
        // yaw from the chord, pitch and roll from the surface; forward is re-completed so the
        // basis stays orthonormal around the surface up
        pose.right = normalizedOrFallback(forward.crossProduct(up), Vector3(1.0f, 0.0f, 0.0f));
        pose.forward = normalizedOrFallback(up.crossProduct(pose.right), forward);
        pose.up = normalizedOrFallback(pose.right.crossProduct(pose.forward), worldUp);
        // centre the FOOTPRINT on the centerline point (kit tiles are corner-pivoted, and a raw
        // pivot on the curve hangs the slab off one side), cancelling the offsets along the
        // piece's OWN local axes so they follow it however it is laid; then plant along its up
        pose.base = c.position;
        const SplinePieceBasis basis = makePieceBasis(pose, pieceAxis);
        pose.position = c.position + pose.up * (meshBaseLift * scale) -
                        basis.localX * (boundsCenter.x * axisScale.x * scale) -
                        basis.localZ * (boundsCenter.z * axisScale.z * scale);
        out.push_back(pose);
    }
}

void spline::buildTileLayout(std::span<const SplineCenterSample> center, const SplineTileLayoutParams& params,
                             std::vector<SplinePiecePose>& out){
    out.clear();
    if (center.size() < 2u || params.pieces.empty() || params.maxPieces == 0u){
        return;
    }

    // hold the conform gaps on the whole centerline before it is trimmed, so a trim starting in
    // a hole inherits the altitude its neighbour measured
    std::vector<SplineCenterSample> held(center.begin(), center.end());
    holdSurfaceAcrossGaps(held);
    for (SplineCenterSample& sample : held){
        sample.surfaceValid = true;
    }
    std::vector<float> cum;
    cumulativeLengths(held, cum);
    const float drapedLength = cum.back();
    if (!(drapedLength > 0.0f)){
        return;
    }

    const float startTrim = sanitizedMagnitude(params.startTrimMetres, drapedLength);
    const float endTrim = sanitizedMagnitude(params.endTrimMetres, drapedLength);
    if (!(drapedLength - startTrim - endTrim > TILE_FIT_TOLERANCE)){
        return;
    }
    std::vector<SplineCenterSample> trimmed;
    if (startTrim > 0.0f || endTrim > 0.0f){
        sliceByDistance(held, cum, startTrim, drapedLength - endTrim, trimmed);
        cumulativeLengths(trimmed, cum);
    }
    const std::span<const SplineCenterSample> run = trimmed.empty() ? std::span<const SplineCenterSample>(held)
                                                                    : std::span<const SplineCenterSample>(trimmed);
    // the length the frame walk measures, computed the same way, so FitToLength lands on it
    const float runLength = cum.back();

    const float spacing = std::isfinite(params.spacing) && params.spacing > 0.0f ? std::max(params.spacing, MIN_TILE_SPACING)
                                                                                : DEFAULT_TILE_SPACING;
    TileRun solved;
    if (params.mode == SplineTileMode::Tile){
        solveTileRun(runLength, params, solved);
    }else{
        solveSpacingRun(runLength, spacing, params, solved);
    }
    if (solved.distances.empty()){
        return;
    }
    drawScales(params, solved);

    SplineTileFrameParams frameParams;
    frameParams.spacing = std::max(solved.shortestLaid, MIN_TILE_PIECE_LENGTH);
    frameParams.fit = params.fit;
    frameParams.alignToSurfaceNormal = params.alignToSurfaceNormal;
    frameParams.slopeBlend = params.slopeBlend;
    frameParams.maxTiltDegrees = params.maxTiltDegrees;
    frameParams.outTiltClamp = params.outTiltClamp;
    frameParams.plantMode = params.plantMode;
    frameParams.maxTiles = static_cast<uint32_t>(solved.distances.size());
    frameParams.stationDistances = solved.distances;
    frameParams.stationPieces = solved.pieces;
    frameParams.stationLengthScales = solved.lengthScales;
    frameParams.stationScales = solved.scales;
    frameParams.probe = params.probe;
    frameParams.seed = params.seed;
    frameParams.spacingJitterMetres = params.spacingJitterMetres;
    frameParams.yawJitterDegrees = params.yawJitterDegrees;
    frameParams.lateralOffset = params.lateralOffset;
    frameParams.lateralJitterMetres = params.lateralJitterMetres;
    frameParams.dropoutChance = params.dropoutChance;
    frameParams.endTaperMetres = params.endTaperMetres;

    std::vector<SplineLayoutPose> frames;
    buildTileFrames(run, frameParams, frames);

    out.reserve(frames.size());
    for (const SplineLayoutPose& frame : frames){
        const uint32_t k = frame.stationIndex;
        const float lengthScale = k < solved.lengthScales.size() ? solved.lengthScales[k] : 1.0f;
        const float scale = k < solved.scales.size() ? solved.scales[k] : 1.0f;
        SplinePiecePose pose = makePiecePose(frame, solved.pieces[k], lengthScale, scale);
        pose.ordinal = k;
        pose.role = SplinePieceRole::Tile;
        out.push_back(pose);
    }
}
