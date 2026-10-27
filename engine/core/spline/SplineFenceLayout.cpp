// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Adapted from OpenEngine's Engine/Modules/SplineLayout/Source/FenceLayout.cpp, Copyright (c)
// 2026 Gamecrafters Guild Limited and contributors, MIT License. See NOTICE.md.

#include "SplineFenceLayout.h"

#include "SplineJitter.h"
#include "SplineSpanMitre.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace jasperengine;
using namespace jasperengine::spline;

namespace{

    constexpr float FENCE_PI = 3.14159265358979f;

    // Knots closer than this are the same corner: the run between them is skipped and its shared
    // station emitted once.
    constexpr float MIN_RUN_LENGTH = 1.0e-3f;
    // a pool piece with no measurable length would divide the fill by zero
    constexpr float MIN_PIECE_LENGTH = 1.0e-3f;
    constexpr float MIN_PITCH = 0.05f;
    // ceiling on a run's counted crest cells, so the count always converts to its uint32 ordinal;
    // unreachable (the pitch floor is a millimetre: four thousand kilometres of run)
    constexpr float MAX_CREST_CELLS_PER_RUN = 4.0e9f;
    // a draped sample this far above what a span is planted for (its plant line plus the skirt
    // its piece buries) counts as burying it; under it the difference is chord noise
    constexpr float BURY_TOLERANCE = 0.05f;
    // Pitch past which a span has no roll of its own left to carry and must borrow one: a
    // DEGENERACY threshold just under vertical, not a style dial; racked spans follow the grade
    // however steep it gets.
    constexpr float SPAN_ROLL_DEGENERATE_PITCH_DEGREES = 85.0f;
    // below this squared length a span's forward has no horizontal extent to measure a grade
    // along, and its plant line is its own base plane
    constexpr float MIN_ALONG_GROUND_LENGTH_SQUARED = 1.0e-8f;

    const Vector3 FENCE_WORLD_UP(0.0f, 1.0f, 0.0f);
    // the engine's forward, and what a piece falls back to when its frame carries no direction
    const Vector3 FENCE_WORLD_FORWARD(0.0f, 0.0f, -1.0f);

    float vectorLength(const Vector3& v){ return std::sqrt(v.dotProduct(v)); }

    std::string metresText(float v){
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        return std::string(buf);
    }

    std::string factorText(float v){
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
        return std::string(buf);
    }

    std::string wholeDegreesText(float radians){
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(radians * 180.0f / FENCE_PI));
        return std::string(buf);
    }

    // Linear sample of the draped polyline at a fractional index. Run boundaries land on knots,
    // which need not coincide with a dense sample.
    SplineCenterSample sampleAtIndex(const std::vector<SplineCenterSample>& center, float u){
        const float maxIndex = static_cast<float>(center.size() - 1u);
        const float clamped = std::clamp(u, 0.0f, maxIndex);
        const size_t lo = std::min(static_cast<size_t>(clamped), center.size() - 2u);
        const float t = clamped - static_cast<float>(lo);
        SplineCenterSample out;
        out.position = center[lo].position + (center[lo + 1u].position - center[lo].position) * t;
        out.normal = normalizedOrFallback(center[lo].normal + (center[lo + 1u].normal - center[lo].normal) * t, FENCE_WORLD_UP);
        out.surfaceValid = true;
        return out;
    }

    // The polyline between two knots, with both as exact endpoints so the run's stations land on
    // them.
    std::vector<SplineCenterSample> sliceRun(const std::vector<SplineCenterSample>& center, float u0, float u1){
        std::vector<SplineCenterSample> sub;
        sub.push_back(sampleAtIndex(center, u0));
        for (size_t i = 0; i < center.size(); i++){
            const float index = static_cast<float>(i);
            if (index <= u0){
                continue;
            }
            if (index >= u1){
                break;
            }
            sub.push_back(center[i]);
        }
        sub.push_back(sampleAtIndex(center, u1));
        return sub;
    }

    float polylineLength(const std::vector<SplineCenterSample>& samples){
        float total = 0.0f;
        for (size_t i = 1; i < samples.size(); i++){
            total += vectorLength(samples[i].position - samples[i - 1u].position);
        }
        return total;
    }

    // Where one span of a run draws its piece from. A gate with no gate piece to draw is an
    // opening: it keeps the room of the wall it replaced and places nothing in it.
    enum class SpanSource : uint8_t{
        Wall,
        Gate,
        Opening
    };

    // one override as a run's fill reads it
    struct RunOverride{
        uint32_t ordinal = 0;
        SplineSpanOverrideKind kind = SplineSpanOverrideKind::None;
        uint32_t poolSlot = 0;
    };

    // How a run's length is divided into span pieces. The pieces need not share a length, so the
    // count and the sequence are solved together.
    struct RunFill{
        // per span, in order: the active pool slot it draws (a gate pool slot for a gate, the span
        // pool slot of the wall it replaced for an opening) and the pool that slot is in
        std::vector<uint32_t> slots;
        std::vector<SpanSource> sources;
        float naturalLength = 0.0f;
        float stretch = 1.0f;
        bool withinCap = true;
        // the count the search started from; validation names it, because the search only tries
        // this and its two neighbours
        int32_t nominalCount = 1;
    };

    const RunOverride* findRunOverride(std::span<const RunOverride> overrides, uint32_t ordinal){
        for (const RunOverride& entry : overrides){
            if (entry.ordinal == ordinal){
                return &entry;
            }
        }
        return nullptr;
    }

    // What one span draws once its override is applied. The misses are reported where the run is
    // solved (reportRunOverrides), never here: the fill tries three counts.
    void applyRunOverride(const RunOverride& entry, size_t spanPieceCount, size_t gatePieceCount,
                          uint32_t& slot, SpanSource& source){
        if (entry.kind == SplineSpanOverrideKind::Gate){
            if (entry.poolSlot < gatePieceCount){
                slot = entry.poolSlot;
                source = SpanSource::Gate;
            }else{
                source = SpanSource::Opening;
            }
        }else if (entry.kind == SplineSpanOverrideKind::ExplicitPiece && entry.poolSlot < spanPieceCount){
            slot = entry.poolSlot;
        }
    }

    // One attempt at the nominal count and one in each direction, then stop: the cap can be
    // genuinely unsatisfiable (cap 1.25, 2 m piece, 2.6 m run), so the search must not chase it.
    // The best of the three is still built and the caller reports the miss.
    //
    // A gate is a span of the fill at its own length: it takes the run's one shared stretch as
    // every span does. The seeded picks of the other spans are those of the same ordinals without
    // the override, so a gate that leaves its run's span count unchanged reshuffles nothing.
    RunFill solveRunFill(float runLength, uint32_t seed, uint32_t spanBaseOrdinal, std::span<const SplinePiece> pieces,
                         std::span<const SplinePiece> gatePieces, std::span<const RunOverride> overrides,
                         float maxStretch, float minScale){
        RunFill best;
        const uint32_t activeCount = static_cast<uint32_t>(pieces.size());
        if (activeCount == 0u){
            return best;
        }

        // Seed the count from the pool's own pieces, never from the post pitch: a 20 m wall run
        // of 5 m segments at the 2.4 m default pitch would search 7, 8 and 9 and never see 4.
        float meanPieceLength = 0.0f;
        for (const SplinePiece& piece : pieces){
            meanPieceLength += std::max(piece.length(), MIN_PIECE_LENGTH);
        }
        meanPieceLength = std::max(meanPieceLength / static_cast<float>(activeCount), MIN_PIECE_LENGTH);

        const int32_t nominal = std::max(1, static_cast<int32_t>(std::lround(runLength / meanPieceLength)));
        best.nominalCount = nominal;
        const int32_t candidates[3] = {nominal, nominal + 1, nominal - 1};

        float bestDeviation = std::numeric_limits<float>::max();
        for (const int32_t count : candidates){
            if (count < 1){
                continue;
            }

            RunFill trial;
            trial.nominalCount = nominal;
            trial.slots.resize(static_cast<size_t>(count));
            trial.sources.assign(static_cast<size_t>(count), SpanSource::Wall);
            float natural = 0.0f;
            for (int32_t k = 0; k < count; k++){
                uint32_t slot = splinePickPiece(seed, SplinePoolRole::Span, spanBaseOrdinal + static_cast<uint32_t>(k), pieces);
                SpanSource source = SpanSource::Wall;
                if (const RunOverride* entry = findRunOverride(overrides, static_cast<uint32_t>(k))){
                    applyRunOverride(*entry, pieces.size(), gatePieces.size(), slot, source);
                }
                trial.slots[static_cast<size_t>(k)] = slot;
                trial.sources[static_cast<size_t>(k)] = source;
                const SplinePiece& piece = source == SpanSource::Gate ? gatePieces[slot] : pieces[slot];
                natural += std::max(piece.length(), MIN_PIECE_LENGTH);
            }
            trial.naturalLength = natural;
            trial.stretch = runLength / natural;
            trial.withinCap = trial.stretch <= maxStretch && trial.stretch >= minScale;
            if (trial.withinCap){
                return trial;
            }

            const float deviation = std::abs(std::log(std::max(trial.stretch, 1.0e-6f)));
            if (deviation < bestDeviation){
                bestDeviation = deviation;
                best = std::move(trial);
            }
        }
        return best;
    }

    // Everything one run contributes, resolved before any of it is emitted: a shared station's
    // yaw needs BOTH adjoining runs' end directions.
    struct Run{
        std::vector<SplineCenterSample> samples;
        float lengthMetres = 0.0f;
        uint32_t startKnot = 0;
        uint32_t endKnot = 0;
        RunFill fill;
        // 0 .. lengthMetres, one more than the spans
        std::vector<float> stations;
        Vector3 startForward = Vector3(0.0f, 0.0f, -1.0f);
        Vector3 endForward = Vector3(0.0f, 0.0f, -1.0f);
        std::vector<Vector3> stationForwards;
        std::vector<SplineLayoutPose> poses;
        // span frames, resolved before emit so the bury check reads the geometry actually placed;
        // parallel to fill.slots
        std::vector<SplineLayoutPose> spanPoses;
        std::vector<float> spanLengthScales;
        uint32_t rollBorrowedSpans = 0;
        // per span: whether it borrowed its roll, and how each end meets the span beyond its
        // station. The poses and scales above stay the chord's; a mitred end's overhang is applied
        // where the span is emitted, so every rule reading the bay keeps reading the bay.
        std::vector<uint8_t> spanRollBorrowed;
        std::vector<SplineFenceEndPlane> spanStarts;
        std::vector<SplineFenceEndPlane> spanEnds;
        std::vector<float> spanStartOverhangs;
        std::vector<float> spanEndOverhangs;
    };

    // the pool piece span `span` of a run is laid as: its gate piece for a gate, otherwise the
    // span pool piece it drew (for an opening, the wall whose room it keeps)
    const SplinePiece& spanPieceOf(const Run& run, size_t span, const SplineFenceLayoutParams& params){
        return run.fill.sources[span] == SpanSource::Gate ? params.gatePieces[run.fill.slots[span]]
                                                          : params.spanPieces[run.fill.slots[span]];
    }

    bool isOpening(const Run& run, size_t span){
        return run.fill.sources[span] == SpanSource::Opening;
    }

    // How high the ground may stand under a world point before this span counts as buried. The
    // plant line is a PLANE through the span's base: level across travel, climbing along it at
    // the span's own grade, so it describes racked, stepped and borrowed-roll spans alike. The
    // projection is taken on the forward's horizontal part, divided by that part's own length.
    // The allowance above it is the piece's own skirt under Pivot planting (the skirt is already
    // underground at plant time, so burial reaches about twice the skirt before this fires: the
    // kit's own tolerance); BoundsMin stands the lowest vertex on the surface and gets none.
    float spanGroundAllowanceAt(const SplineLayoutPose& span, const SplinePiece& piece, SplinePlantMode plantMode, const Vector3& at){
        const Vector3 alongGround(span.forward.x, 0.0f, span.forward.z);
        const float alongGroundLengthSquared = alongGround.dotProduct(alongGround);
        const float plantLine = alongGroundLengthSquared > MIN_ALONG_GROUND_LENGTH_SQUARED
                                    ? span.base.y + span.forward.y * (at - span.base).dotProduct(alongGround) / alongGroundLengthSquared
                                    : span.base.y;
        const float skirt = plantMode == SplinePlantMode::Pivot ? std::max(piece.skirtDepth(), 0.0f) : 0.0f;
        return plantLine + skirt;
    }

    Vector3 groundDirection(const Vector3& v, const Vector3& fallback){
        return normalizedOrFallback(Vector3(v.x, 0.0f, v.z), fallback);
    }

    // the caller's shared facing for one knot, when it offered a usable one
    void applyKnotForward(std::span<const Vector3> authored, uint32_t knot, Vector3& forward){
        if (knot < authored.size()){
            forward = groundDirection(authored[knot], forward);
        }
    }

    // How far a piece's own pivot stands above the line it is planted on: Pivot puts the pivot
    // on the line and leaves the skirt underground, BoundsMin lifts until the lowest geometry
    // rests on it.
    float plantedPivotLift(const SplinePiece& piece, SplinePlantMode plantMode){
        return plantMode == SplinePlantMode::Pivot ? 0.0f : piece.skirtDepth();
    }

    // where a span mesh's top stands above the line its stations were planted on: what anything
    // laid along that top is lifted by (a skirted mesh shows only what is above its pivot)
    float spanTopAboveBase(const SplinePiece& piece, SplinePlantMode plantMode){
        return plantedPivotLift(piece, plantMode) + piece.center.y + piece.halfExtents.y;
    }

    // The two points a span joins: its stations' bases, both levelled to the lower of the two for
    // a stepped span so it cannot float off its downhill end.
    void spanEndBases(const SplineLayoutPose& a, const SplineLayoutPose& b, SplineSpanGrade grade, Vector3& outFrom, Vector3& outTo){
        outFrom = a.base;
        outTo = b.base;
        if (grade == SplineSpanGrade::Stepped){
            const float level = std::min(outFrom.y, outTo.y);
            outFrom.y = level;
            outTo.y = level;
        }
    }

    // Where a span's pivot stands for its base and scale: footprint-centred along the piece's OWN
    // axes, so the one along travel scales with the span exactly as the mesh does.
    void placeSpanPivot(SplineLayoutPose& pose, const SplinePiece& piece, SplinePlantMode plantMode, float lengthScale){
        const float lift = plantedPivotLift(piece, plantMode);
        const SplinePieceBasis basis = makePieceBasis(pose, piece.axis());
        const SplinePieceAxisScale scale = makePieceAxisScale(piece.axis(), lengthScale);
        pose.position = pose.base + pose.up * lift - basis.localX * (piece.center.x * scale.x) -
                        basis.localZ * (piece.center.z * scale.z);
    }

    // The frame a span takes between two stations, derived entirely from the station poses: the
    // stations already measured the ground, so a span over a terrain gap stays with the posts.
    SplineLayoutPose makeSpanPose(const SplineLayoutPose& a, const SplineLayoutPose& b, SplineSpanGrade grade,
                                  SplinePlantMode plantMode, const SplinePiece& piece, float& outLengthScale, bool& outRollBorrowed){
        Vector3 from;
        Vector3 to;
        spanEndBases(a, b, grade, from, to);

        const Vector3 delta = to - from;
        const Vector3 forward = normalizedOrFallback(delta, a.forward);
        const Vector3 upAverage = normalizedOrFallback(a.up + b.up, FENCE_WORLD_UP);
        const float spanLength = vectorLength(delta);

        // A span standing on end has no roll of its own: every up perpendicular to a vertical
        // forward is horizontal, and deriving it from the cross product lays the piece FLAT while
        // the basis stays orthonormal. Past the degeneracy pitch the span borrows its opening
        // station's frame. Discriminate on the span's own pitch: a cross-product length is still
        // 0.042 one degree off vertical.
        outRollBorrowed = std::abs(forward.dotProduct(FENCE_WORLD_UP)) >
                          std::sin(SPAN_ROLL_DEGENERATE_PITCH_DEGREES * FENCE_PI / 180.0f);

        SplineLayoutPose pose;
        if (outRollBorrowed){
            // with forward parallel to the station's up no roll about it leaves the piece upright,
            // so the span holds the frame its post stands in
            pose.up = a.up;
            pose.right = normalizedOrFallback(forward.crossProduct(pose.up), a.right);
            pose.forward = normalizedOrFallback(pose.up.crossProduct(pose.right), a.forward);
        }else{
            pose.right = normalizedOrFallback(forward.crossProduct(upAverage), a.right);
            pose.up = normalizedOrFallback(pose.right.crossProduct(forward), upAverage);
            pose.forward = normalizedOrFallback(pose.up.crossProduct(pose.right), forward);
        }

        outLengthScale = spanLength / std::max(piece.length(), MIN_PIECE_LENGTH);

        pose.base = from + (to - from) * 0.5f;
        placeSpanPivot(pose, piece, plantMode, outLengthScale);
        return pose;
    }

    // ---- Span joins ------------------------------------------------------------------------

    // A station two spans meet at: the closing end of one and the opening end of the next,
    // inside one run or across a knot.
    struct SpanJoin{
        Run* before = nullptr;
        size_t beforeSpan = 0;
        Run* after = nullptr;
        size_t afterSpan = 0;
    };

    // whether a run's spans reach both of its end stations (a run the station budget cut short
    // ends at no station its neighbour shares)
    bool runReachesItsEnds(const Run& run){
        return run.poses.size() >= 2u && run.poses.size() == run.stations.size() && run.spanPoses.size() + 1u == run.poses.size();
    }

    // every station two spans share, in run order: each interior station of a run, then the knot
    // it closes on where the next run opens there (the closed loop's shared station included)
    std::vector<SpanJoin> collectSpanJoins(std::vector<Run>& runs, bool closed){
        std::vector<SpanJoin> joins;
        const size_t sharedJoints = closed ? runs.size() : runs.size() - 1u;
        for (size_t r = 0; r < runs.size(); r++){
            Run& run = runs[r];
            for (size_t k = 1; k < run.spanPoses.size(); k++){
                joins.push_back({&run, k - 1u, &run, k});
            }
            if (r >= sharedJoints){
                continue;
            }
            Run& next = runs[(r + 1u) % runs.size()];
            if (&next == &run || !runReachesItsEnds(run) || !runReachesItsEnds(next)){
                continue;
            }
            const Vector3 gap = run.poses.back().base - next.poses.front().base;
            if (gap.dotProduct(gap) > MIN_RUN_LENGTH * MIN_RUN_LENGTH){
                continue;
            }
            joins.push_back({&run, run.spanPoses.size() - 1u, &next, 0u});
        }
        return joins;
    }

    // how a join is named to the author: by its knot where it has one
    std::string joinName(const SpanJoin& join){
        if (join.before != join.after){
            return "knot " + std::to_string(join.before->endKnot);
        }
        return "the station " + metresText(join.before->stations[join.afterSpan]) + " m along run " +
               std::to_string(join.before->startKnot) + "->" + std::to_string(join.before->endKnot);
    }

    // What a registered crest adds to a run's spans at a join: the widest crest piece's
    // half-thickness and the heights it covers above the span piece's top. A zero half-thickness
    // is no registered crest.
    struct CrestReach{
        float halfThickness = 0.0f;
        float bottom = 0.0f;
        float top = 0.0f;
    };

    // one span's side of a join, as the join is measured on it
    SplineSpanJoinSide joinSideOf(const Run& run, size_t span, bool atStart, const SplineFenceLayoutParams& params, const CrestReach& crest){
        SplineSpanJoinSide side;
        side.pose = run.spanPoses[span];
        Vector3 from;
        Vector3 to;
        spanEndBases(run.poses[span], run.poses[span + 1u], params.spanGrade, from, to);
        side.baseLineEnd = atStart ? from : to;
        const SplinePiece& piece = spanPieceOf(run, span, params);
        side.halfThickness = piece.halfThickness();
        const float lift = plantedPivotLift(piece, params.plantMode);
        side.bottom = lift + piece.center.y - piece.halfExtents.y;
        side.top = lift + piece.center.y + piece.halfExtents.y;
        // a gate stands in a reserved cell, so no crest stands on it to reach across the join
        if (crest.halfThickness > 0.0f && run.fill.sources[span] == SpanSource::Wall){
            side.crestHalfThickness = crest.halfThickness;
            side.crestBottom = side.top + crest.bottom;
            side.crestTop = side.top + crest.top;
        }
        return side;
    }

    // Decides every span end's join. A join is mitred where all of these hold: two spans meet
    // there (an opening places nothing to meet); no post mesh stands on it (a post covers the
    // join); neither span borrowed its roll (its end is not where the plane is); the notch its
    // square ends would leave exceeds a millimetre (below it the turn is chord noise); the turn is
    // within the mitre limit; and the reach that carries both ends across the plane leaves both
    // spans within the stretch cap. Past the limit or the cap the ends stay square and the author
    // is told.
    void mitreJoinsOfRuns(std::vector<Run>& runs, const SplineFenceLayoutParams& params,
                          const std::vector<CrestReach>& crests, std::vector<std::string>& validation){
        for (Run& run : runs){
            const size_t spans = run.spanPoses.size();
            run.spanStarts.assign(spans, SplineFenceEndPlane{});
            run.spanEnds.assign(spans, SplineFenceEndPlane{});
            run.spanStartOverhangs.assign(spans, 0.0f);
            run.spanEndOverhangs.assign(spans, 0.0f);
        }
        if (!params.postPieces.empty()){
            return;
        }

        const float limitRadians = SPLINE_MITRE_LIMIT_DEGREES * FENCE_PI / 180.0f;
        for (const SpanJoin& join : collectSpanJoins(runs, params.closed)){
            Run& before = *join.before;
            Run& after = *join.after;
            if (before.spanRollBorrowed[join.beforeSpan] != 0u || after.spanRollBorrowed[join.afterSpan] != 0u ||
                isOpening(before, join.beforeSpan) || isOpening(after, join.afterSpan)){
                continue;
            }
            const SplineLayoutPose& beforePose = before.spanPoses[join.beforeSpan];
            const SplineLayoutPose& afterPose = after.spanPoses[join.afterSpan];
            const float turn = signedGroundYaw(beforePose.forward, afterPose.forward);
            if (std::abs(turn) > limitRadians){
                validation.push_back("Fence: " + joinName(join) + " turns " + wholeDegreesText(std::abs(turn)) +
                                     " degrees, past the " + wholeDegreesText(limitRadians) +
                                     " degree mitre limit — the spans meet with square ends there; plant a post at it");
                continue;
            }
            SplineSpanJoinMeasure measure;
            const size_t beforeRun = static_cast<size_t>(&before - runs.data());
            const size_t afterRun = static_cast<size_t>(&after - runs.data());
            if (!measureSpanJoin(joinSideOf(before, join.beforeSpan, false, params, crests[beforeRun]),
                                 joinSideOf(after, join.afterSpan, true, params, crests[afterRun]),
                                 before.poses[join.beforeSpan + 1u].base, measure) ||
                !(measure.notchMetres > MIN_RUN_LENGTH)){
                continue;
            }
            before.spanEnds[join.beforeSpan] = measure.beforeEnd;
            after.spanStarts[join.afterSpan] = measure.afterStart;
            before.spanEndOverhangs[join.beforeSpan] = measure.beforeOverhangMetres;
            after.spanStartOverhangs[join.afterSpan] = measure.afterOverhangMetres;
        }

        // The reach is a stretch the piece takes, so the cap governs it: a join whose reach would
        // carry either span past the cap, where its fill alone did not, keeps square ends. A span
        // whose fill is already past the cap has been reported, so its joins stay mitred. Undoing
        // a join only shortens its spans, so one pass settles every join.
        const float maxStretch = std::max(params.spanMaxStretch, 1.0f);
        const auto mitredScale = [&params](const Run& run, size_t span){
            const float length = std::max(spanPieceOf(run, span, params).length(), MIN_PIECE_LENGTH);
            return (run.spanLengthScales[span] * length + run.spanStartOverhangs[span] + run.spanEndOverhangs[span]) / length;
        };
        const auto overCap = [&](const Run& run, size_t span){
            return mitredScale(run, span) > maxStretch && run.spanLengthScales[span] <= maxStretch;
        };
        for (const SpanJoin& join : collectSpanJoins(runs, params.closed)){
            Run& before = *join.before;
            Run& after = *join.after;
            if (!before.spanEnds[join.beforeSpan].mitred || (!overCap(before, join.beforeSpan) && !overCap(after, join.afterSpan))){
                continue;
            }
            const float needed = std::max(mitredScale(before, join.beforeSpan), mitredScale(after, join.afterSpan));
            before.spanEnds[join.beforeSpan] = SplineFenceEndPlane{};
            after.spanStarts[join.afterSpan] = SplineFenceEndPlane{};
            before.spanEndOverhangs[join.beforeSpan] = 0.0f;
            after.spanStartOverhangs[join.afterSpan] = 0.0f;
            validation.push_back("Fence: mitring " + joinName(join) + " needs a span stretched " + factorText(needed) +
                                 "x, past the " + factorText(maxStretch) +
                                 "x cap — the spans meet with square ends there; plant a post at it or raise Span Max Stretch");
        }
    }

    // Where a leaning plane crosses the piece's centre line at the piece's top: the plane moves
    // along travel by tan(lean) per metre up, further out at a closing end, further in at an
    // opening one.
    void setTopInset(SplineFenceEndPlane& plane, bool atStart, const SplinePiece& piece, float lengthScale){
        const float rise = piece.center.y + piece.halfExtents.y - plane.stationLocalY;
        plane.topInset = plane.stationInset + (atStart ? 1.0f : -1.0f) * std::tan(plane.leanRadians) * rise / lengthScale;
    }

    // A span as it is emitted: laid longer by each mitred end's overhang, its base moved along
    // travel by half their difference so each end reaches its own plane, and its planes stated in
    // its final scale. A span with square ends is exactly its chord pose.
    void shapeEmittedSpan(const Run& run, size_t span, const SplineFenceLayoutParams& params, SplineLayoutPose& outPose,
                          float& outLengthScale, SplineFenceEndPlane& outStart, SplineFenceEndPlane& outEnd){
        outPose = run.spanPoses[span];
        outLengthScale = run.spanLengthScales[span];
        outStart = run.spanStarts[span];
        outEnd = run.spanEnds[span];
        if (!outStart.mitred && !outEnd.mitred){
            return;
        }

        const SplinePiece& piece = spanPieceOf(run, span, params);
        const float startOverhang = run.spanStartOverhangs[span];
        const float endOverhang = run.spanEndOverhangs[span];
        if (startOverhang > 0.0f || endOverhang > 0.0f){
            const float length = std::max(piece.length(), MIN_PIECE_LENGTH);
            outLengthScale = (outLengthScale * length + startOverhang + endOverhang) / length;
            outPose.base = outPose.base + outPose.forward * (0.5f * (endOverhang - startOverhang));
            placeSpanPivot(outPose, piece, params.plantMode, outLengthScale);
        }
        // the measured inset is in the frame's metres; the variant reads it in the piece's
        // unscaled local metres
        const float stationLocalY = -plantedPivotLift(piece, params.plantMode);
        if (outStart.mitred){
            outStart.stationInset /= outLengthScale;
            outStart.stationLocalY = stationLocalY;
            setTopInset(outStart, true, piece, outLengthScale);
        }
        if (outEnd.mitred){
            outEnd.stationInset /= outLengthScale;
            outEnd.stationLocalY = stationLocalY;
            setTopInset(outEnd, false, piece, outLengthScale);
        }
    }

    // Lays one end of an emitted span square at its station: the piece budget stopped emission
    // before the span that end was mitred against, so a cut there would face nothing.
    void squareUnpairedEnd(std::vector<Run>& runs, const SplineFenceLayoutParams& params, bool atStart, SplineFenceSpan& span){
        for (Run& run : runs){
            if (run.startKnot != span.run){
                continue;
            }
            const size_t k = span.ordinalInRun;
            (atStart ? run.spanStarts : run.spanEnds)[k] = SplineFenceEndPlane{};
            (atStart ? run.spanStartOverhangs : run.spanEndOverhangs)[k] = 0.0f;
            shapeEmittedSpan(run, k, params, span.pose, span.lengthScale, span.start, span.end);
            return;
        }
    }

    // ---- Crest row -------------------------------------------------------------------------

    // a stretch of one run's crest axis, in metres from its opening station
    struct RunInterval{
        float lo = 0.0f;
        float hi = 0.0f;
    };

    // The axis a run's crest row is counted, reserved and placed on: metres along the spans,
    // because a crest piece stands on a span and a span is the chord between its stations. Each
    // station sits at its draped station less the drape (draped length minus chord) of every span
    // before it, so the phase stays anchored at the run's opening station; a span within the
    // touching millimetre of its draped length carries no drape.
    struct CrestAxis{
        // parallel to Run::stations
        std::vector<float> stations;
        float lengthMetres = 0.0f;
    };

    void buildCrestAxis(const Run& run, SplineSpanGrade grade, CrestAxis& out){
        out.stations = run.stations;
        float drape = 0.0f;
        for (size_t k = 1; k < out.stations.size(); k++){
            if (k <= run.spanPoses.size()){
                Vector3 from;
                Vector3 to;
                spanEndBases(run.poses[k - 1u], run.poses[k], grade, from, to);
                const float spanDrape = (run.stations[k] - run.stations[k - 1u]) - vectorLength(to - from);
                if (std::abs(spanDrape) > MIN_RUN_LENGTH){
                    drape += spanDrape;
                }
            }
            out.stations[k] = run.stations[k] - drape;
        }
        out.lengthMetres = run.lengthMetres - drape;
    }

    // the pitch the crest cells are laid at, with the piece length one cell has to hold
    struct CrestPitch{
        float metres = 0.0f;
        float longestPiece = 0.0f;
        // the pitch the author typed, 0 for the piece's own length
        float asked = 0.0f;
        // the author asked for less than the longest piece needs: pieces would overlap, so the
        // piece's length is used and the author is told once
        bool raisedToPieceLength = false;
    };

    CrestPitch resolveCrestPitch(std::span<const SplinePiece> pieces, float authored){
        CrestPitch pitch;
        pitch.longestPiece = MIN_PIECE_LENGTH;
        for (const SplinePiece& piece : pieces){
            pitch.longestPiece = std::max(pitch.longestPiece, piece.length());
        }
        // a non-finite pitch carries no intent, so it reads as 0: the piece's own length
        const float asked = std::isfinite(authored) ? authored : 0.0f;
        pitch.asked = std::max(asked, 0.0f);
        pitch.metres = asked > 0.0f ? asked : pitch.longestPiece;
        pitch.raisedToPieceLength = pitch.metres < pitch.longestPiece;
        if (pitch.raisedToPieceLength){
            pitch.metres = pitch.longestPiece;
        }
        return pitch;
    }

    // The longest crest piece a clear stretch holds whole whatever its phase against the cells.
    // A cell holds its piece centred in [kP, (k+1)P], so a stretch S is certain to hold a piece of
    // length L only when S >= P + L; at most half the stretch is certain either way. Floored to
    // the centimetre the report prints, so the printed number fits too.
    float crestPieceThatAlwaysFits(float stretch, float askedPitch){
        const float half = stretch * 0.5f;
        const float fits = askedPitch > 0.0f ? std::min(stretch - askedPitch, half) : half;
        return std::floor(fits * 100.0f) / 100.0f;
    }

    // The report for a crest row that placed no crest piece while a pitch-grid run was long
    // enough to hold one bare: the posts or the caller's pieces took the room.
    std::string emptyCrestRowReport(float stretch, float askedPitch, bool hasPostMesh){
        const std::string wayOut = hasPostMesh ? ", or clear the Post pool." : ", or move what reserves the run.";
        const std::string opening = "Fence: the crest row placed no crest pieces — the longest clear stretch is " +
                                    metresText(stretch) + " m. ";
        const float fits = crestPieceThatAlwaysFits(stretch, askedPitch);
        if (askedPitch > 0.0f && fits < MIN_PIECE_LENGTH){
            return opening + "Crest Pitch " + metresText(askedPitch) + " m leaves no room for a piece in it; lower the pitch" + wayOut;
        }
        // the pitch, not the half, is what binds once it exceeds half the stretch
        if (askedPitch > 0.0f && stretch - askedPitch < stretch * 0.5f){
            return opening + "At a Crest Pitch of " + metresText(askedPitch) + " m, a crest piece no longer than " +
                   metresText(fits) + " m (the stretch less the pitch) always fits; use one" + wayOut;
        }
        return opening + "A crest piece no longer than half of it, " + metresText(fits) + " m, always fits; use one" + wayOut;
    }

    // Everything on one run the crest row keeps clear: the post footprint at every station, the
    // span every gate and opening stands in, and the stretches the caller reserved, all on the
    // crest axis, sorted and merged so a cell tests one list.
    std::vector<RunInterval> mergedRunReservations(const Run& run, const CrestAxis& axis, float postHalfLength,
                                                   std::span<const SplineFenceReservation> reserved){
        std::vector<RunInterval> intervals;
        if (postHalfLength > 0.0f){
            intervals.reserve(axis.stations.size());
            for (const float station : axis.stations){
                intervals.push_back({station - postHalfLength, station + postHalfLength});
            }
        }
        const size_t spans = std::min(run.spanPoses.size(), run.fill.sources.size());
        for (size_t k = 0; k < spans && k + 1u < axis.stations.size(); k++){
            if (run.fill.sources[k] != SpanSource::Wall){
                intervals.push_back({axis.stations[k], axis.stations[k + 1u]});
            }
        }
        for (const SplineFenceReservation& entry : reserved){
            // ordered comparisons throughout, so a NaN bound drops the entry
            if (!(entry.endMetres > entry.startMetres)){
                continue;
            }
            // the part reaching forward from the knot this run opens at, and the part reaching
            // back from the knot it closes on; a closed spline's single run answers to both
            if (entry.knot == run.startKnot && entry.endMetres > 0.0f){
                intervals.push_back({std::max(entry.startMetres, 0.0f), entry.endMetres});
            }
            if (entry.knot == run.endKnot && entry.startMetres < 0.0f){
                intervals.push_back({axis.lengthMetres + entry.startMetres, axis.lengthMetres + std::min(entry.endMetres, 0.0f)});
            }
        }
        std::sort(intervals.begin(), intervals.end(), [](const RunInterval& a, const RunInterval& b){ return a.lo < b.lo; });

        std::vector<RunInterval> merged;
        merged.reserve(intervals.size());
        for (const RunInterval& interval : intervals){
            if (!merged.empty() && interval.lo <= merged.back().hi){
                merged.back().hi = std::max(merged.back().hi, interval.hi);
            }else{
                merged.push_back(interval);
            }
        }
        return merged;
    }

    // the stretches of a run no reservation covers, in run order
    std::vector<RunInterval> freeRunIntervals(float runLength, const std::vector<RunInterval>& reserved){
        std::vector<RunInterval> free;
        free.reserve(reserved.size() + 1u);
        float cursor = 0.0f;
        for (const RunInterval& interval : reserved){
            const float lo = std::clamp(interval.lo, 0.0f, runLength);
            if (lo > cursor){
                free.push_back({cursor, lo});
            }
            cursor = std::max(cursor, std::min(interval.hi, runLength));
        }
        if (cursor < runLength){
            free.push_back({cursor, runLength});
        }
        return free;
    }

    // Cells of one run: one opens at every k * P inside it, counted arithmetically (accumulating
    // floats over thousands of cells would move the last by millimetres). Every cell is counted,
    // including those a reservation removes or the budget never reaches: this is the ordinal the
    // NEXT run's cells are numbered from.
    uint32_t crestCellCount(float runLength, float pitch){
        const float exact = std::ceil(runLength / pitch);
        if (!(exact > 0.0f)){
            return 0u;
        }
        return static_cast<uint32_t>(std::min(exact, MAX_CREST_CELLS_PER_RUN));
    }

    // the cell a cap stands in, whose ordinal it inherits
    uint32_t cellAtDistance(float distance, float pitch, uint32_t cellCount){
        if (cellCount == 0u){
            return 0u;
        }
        const float cell = std::floor(distance / pitch);
        if (!(cell > 0.0f)){
            return 0u;
        }
        return static_cast<uint32_t>(std::min(cell, static_cast<float>(cellCount - 1u)));
    }

    // The cells one run's crest row is counted on: the pitch grid, or, for a run registered to
    // its spans, one cell per span whose stretch of the crest axis IS the cell.
    struct CrestCells{
        float pitch = 0.0f;
        uint32_t count = 0;
        // the run's crest-axis stations when registered to its spans; null on the pitch grid
        const std::vector<float>* spanStations = nullptr;

        bool registeredToSpans() const { return spanStations != nullptr; }

        uint32_t cellAt(float distance) const{
            if (!registeredToSpans()){
                return cellAtDistance(distance, pitch, count);
            }
            if (count == 0u){
                return 0u;
            }
            const auto upper = std::upper_bound(spanStations->begin(), spanStations->end(), distance);
            const size_t after = static_cast<size_t>(upper - spanStations->begin());
            return static_cast<uint32_t>(std::min<size_t>(after == 0u ? 0u : after - 1u, count - 1u));
        }
    };

    // Whether a run's crest row is registered to its spans: the crest cell and every span piece
    // the run draws share one nominal length. A crest at its own pitch would drift off the walls
    // wherever they are stretched (on a curve every chord is), so each crest takes its span's
    // frame and scale instead. Only walls are asked: a gate or opening reserves its own span.
    bool crestRegistersToSpans(const Run& run, const SplineFenceLayoutParams& params, const CrestPitch& pitch){
        // an author who asked for air between the pieces asked for the pitch
        if (run.spanPoses.empty() || pitch.metres > pitch.longestPiece + MIN_RUN_LENGTH){
            return false;
        }
        for (size_t k = 0; k < run.spanPoses.size(); k++){
            if (run.fill.sources[k] != SpanSource::Wall){
                continue;
            }
            const float spanPiece = std::max(params.spanPieces[run.fill.slots[k]].length(), MIN_PIECE_LENGTH);
            if (std::abs(spanPiece - pitch.longestPiece) > MIN_RUN_LENGTH){
                return false;
            }
        }
        return true;
    }

    // decides every span end's join, with what a registered crest adds to each run's spans
    void mitreBareJoins(std::vector<Run>& runs, const SplineFenceLayoutParams& params, std::vector<std::string>& validation){
        std::vector<CrestReach> crests(runs.size());
        if (!params.crestPieces.empty()){
            const CrestPitch pitch = resolveCrestPitch(params.crestPieces, params.crestPitch);
            CrestReach reach;
            reach.bottom = std::numeric_limits<float>::max();
            reach.top = std::numeric_limits<float>::lowest();
            for (const SplinePiece& crest : params.crestPieces){
                reach.halfThickness = std::max(reach.halfThickness, crest.halfThickness());
                reach.bottom = std::min(reach.bottom, crest.center.y - crest.halfExtents.y);
                reach.top = std::max(reach.top, crest.center.y + crest.halfExtents.y);
            }
            for (size_t r = 0; r < runs.size(); r++){
                if (crestRegistersToSpans(runs[r], params, pitch)){
                    crests[r] = reach;
                }
            }
        }
        mitreJoinsOfRuns(runs, params, crests, validation);
    }

    // one piece of one run's crest row, solved on the crest axis before anything is posed
    struct CrestPlacement{
        float centre = 0.0f;
        uint32_t ordinal = 0;
        uint32_t poolSlot = 0;
        bool isCap = false;
        // a cap closing the edge BELOW its stretch (a run start, the far side of a reservation)
        // faces back down the run: a half-turn about the span's up, never a mirror
        bool facesBack = false;
    };

    // The longest cap that fits a remainder: by length and not by the seed, since a remainder is
    // a fit. Touching counts as fitting, by the millimetre everything here does.
    bool chooseCapPiece(std::span<const SplinePiece> caps, float remainder, uint32_t& outSlot, float& outLength){
        bool found = false;
        for (uint32_t slot = 0; slot < static_cast<uint32_t>(caps.size()); slot++){
            const float length = std::max(caps[slot].length(), MIN_PIECE_LENGTH);
            if (length > remainder + MIN_RUN_LENGTH || (found && length <= outLength)){
                continue;
            }
            outSlot = slot;
            outLength = length;
            found = true;
        }
        return found;
    }

    // a cap centred where the remainder leaves it, presenting its front to the edge it closes
    CrestPlacement capAt(float centre, bool facesBack, uint32_t slot, const CrestCells& cells){
        CrestPlacement cap;
        cap.centre = centre;
        cap.ordinal = cells.cellAt(cap.centre);
        cap.poolSlot = slot;
        cap.isCap = true;
        cap.facesBack = facesBack;
        return cap;
    }

    // One run's crest row: the cells that clear its reservations, then the caps that close what
    // each free stretch leaves over, in run order. Cells are laid from the run's OPENING station,
    // never from the spline start, so the row's phase survives an edit elsewhere; a cell that
    // does not fit whole, or stands under a reservation, is removed and keeps its ordinal.
    std::vector<CrestPlacement> solveRunCrest(const Run& run, const CrestAxis& axis, const SplineFenceLayoutParams& params,
                                              const CrestCells& cells, uint32_t cellBase, float& outLongestFreeStretch){
        // a registered crest spans the bay between two posts exactly as its wall does, so the
        // posts at its ends are its join, not a collision: they reserve against the grid only
        const float postHalfLength = !params.postPieces.empty() && !cells.registeredToSpans()
                                         ? params.postPieces[0].length() * 0.5f
                                         : 0.0f;
        const std::vector<RunInterval> reserved = mergedRunReservations(run, axis, postHalfLength, params.reservations);

        std::vector<CrestPlacement> row;
        std::vector<CrestPlacement> kept;
        // parallel to kept
        std::vector<RunInterval> footprints;
        bool keptAny = false;
        uint32_t lastKept = 0;
        for (const RunInterval& free : freeRunIntervals(axis.lengthMetres, reserved)){
            // the widest stretch any piece was offered: the number that says which piece fits
            outLongestFreeStretch = std::max(outLongestFreeStretch, free.hi - free.lo);
            // only the cells that can reach this stretch are built, so a tower covering a
            // kilometre of run costs the arithmetic that steps over its cells
            const int64_t firstCell = cells.registeredToSpans()
                                          ? static_cast<int64_t>(cells.cellAt(free.lo))
                                          : std::max<int64_t>(static_cast<int64_t>(std::floor(free.lo / cells.pitch)) - 1, 0);
            const int64_t lastCell = cells.registeredToSpans()
                                         ? static_cast<int64_t>(cells.cellAt(free.hi))
                                         : std::min<int64_t>(static_cast<int64_t>(std::floor(free.hi / cells.pitch)),
                                                             static_cast<int64_t>(cells.count) - 1);
            kept.clear();
            footprints.clear();
            for (int64_t index = firstCell; index <= lastCell; index++){
                const uint32_t cell = static_cast<uint32_t>(index);
                // two stretches could reach one cell only past a reservation shorter than the
                // slack either side of it; one compare keeps the row single-valued
                if (keptAny && cell <= lastKept){
                    continue;
                }

                CrestPlacement placement;
                placement.ordinal = cell;
                placement.poolSlot = splinePickPiece(params.seed, SplinePoolRole::Crest, cellBase + cell, params.crestPieces);
                float half = std::max(params.crestPieces[placement.poolSlot].length(), MIN_PIECE_LENGTH) * 0.5f;
                if (cells.registeredToSpans()){
                    // centred on its span and scaled as that span is
                    placement.centre = (axis.stations[cell] + axis.stations[cell + 1u]) * 0.5f;
                    half *= run.spanLengthScales[cell];
                }else{
                    placement.centre = static_cast<float>(cell) * cells.pitch + cells.pitch * 0.5f;
                }
                const RunInterval footprint{placement.centre - half, placement.centre + half};
                // whole or not at all: touching an edge is not crossing it
                if (footprint.lo < free.lo - MIN_RUN_LENGTH || footprint.hi > free.hi + MIN_RUN_LENGTH){
                    continue;
                }

                kept.push_back(placement);
                footprints.push_back(footprint);
                keptAny = true;
                lastKept = cell;
            }

            if (params.capPieces.empty()){
                row.insert(row.end(), kept.begin(), kept.end());
                continue;
            }

            // one cap per remainder, flush against the kept cells, so the residual the pool cannot
            // close lies against the post or gate that covers it
            uint32_t slot = 0;
            float length = 0.0f;
            if (kept.empty()){
                // nothing stands here, so the whole stretch is ONE remainder opening at its lower
                // edge: the cap reaches up from that edge and faces it
                if (chooseCapPiece(params.capPieces, free.hi - free.lo, slot, length)){
                    row.push_back(capAt(free.lo + length * 0.5f, true, slot, cells));
                }
                continue;
            }
            const float keptLo = footprints.front().lo;
            const float keptHi = footprints.back().hi;
            if (chooseCapPiece(params.capPieces, keptLo - free.lo, slot, length)){
                row.push_back(capAt(keptLo - length * 0.5f, true, slot, cells));
            }
            row.insert(row.end(), kept.begin(), kept.end());
            if (chooseCapPiece(params.capPieces, free.hi - keptHi, slot, length)){
                row.push_back(capAt(keptHi + length * 0.5f, false, slot, cells));
            }
        }
        return row;
    }

    // Where one crest piece stands: on the top of the span under it, laid along that span (a
    // stepped span is level) at scale 1, moved off the top line only by its pivot compensation.
    bool crestPoseOnRun(const Run& run, const CrestAxis& axis, const SplineFenceLayoutParams& params,
                        const CrestPlacement& placement, const SplinePiece& piece, SplineLayoutPose& outPose){
        if (run.spanPoses.empty()){
            return false;
        }
        const auto upper = std::upper_bound(axis.stations.begin(), axis.stations.end(), placement.centre);
        const size_t after = static_cast<size_t>(upper - axis.stations.begin());
        const size_t index = std::min(after == 0u ? size_t{0} : after - 1u, run.spanPoses.size() - 1u);
        const SplineLayoutPose& span = run.spanPoses[index];

        Vector3 from;
        Vector3 to;
        spanEndBases(run.poses[index], run.poses[index + 1u], params.spanGrade, from, to);
        const float along = axis.stations[index + 1u] - axis.stations[index];
        const float u = std::clamp((placement.centre - axis.stations[index]) / std::max(along, MIN_RUN_LENGTH), 0.0f, 1.0f);

        SplineLayoutPose pose;
        pose.forward = normalizedOrFallback(span.forward, FENCE_WORLD_FORWARD);
        if (placement.facesBack){
            pose.forward = pose.forward * -1.0f;
        }
        pose.right = normalizedOrFallback(pose.forward.crossProduct(span.up), span.right);
        pose.up = normalizedOrFallback(pose.right.crossProduct(pose.forward), span.up);
        pose.base = from + (to - from) * u + span.up * spanTopAboveBase(spanPieceOf(run, index, params), params.plantMode);
        const SplinePieceBasis basis = makePieceBasis(pose, piece.axis());
        pose.position = pose.base - basis.localX * piece.center.x - basis.localZ * piece.center.z;
        outPose = pose;
        return true;
    }

    // Where a crest registered to its span stands: in that span's own frame, lifted to the span
    // mesh's top and at the span's lengthScale, so its ends are the span's ends. A mitred span's
    // crest takes the span's two planes into its own frame.
    void placeCrestOnSpan(const Run& run, const SplineFenceLayoutParams& params, size_t spanIndex,
                          const SplinePiece& piece, SplineFenceCrest& crest){
        SplineLayoutPose span;
        SplineFenceEndPlane spanStart;
        SplineFenceEndPlane spanEnd;
        shapeEmittedSpan(run, spanIndex, params, span, crest.lengthScale, spanStart, spanEnd);

        const SplinePiece& spanPiece = spanPieceOf(run, spanIndex, params);
        const float top = spanTopAboveBase(spanPiece, params.plantMode);
        SplineLayoutPose& pose = crest.pose;
        pose.right = span.right;
        pose.up = span.up;
        pose.forward = span.forward;
        pose.base = span.base + span.up * top;
        const SplinePieceBasis basis = makePieceBasis(pose, piece.axis());
        const SplinePieceAxisScale scale = makePieceAxisScale(piece.axis(), crest.lengthScale);
        pose.position = pose.base - basis.localX * (piece.center.x * scale.x) - basis.localZ * (piece.center.z * scale.z);

        const float endDifference = 0.5f * (piece.length() - spanPiece.length());
        crest.start = spanStart;
        crest.end = spanEnd;
        crest.start.stationInset += endDifference;
        crest.end.stationInset += endDifference;
        crest.start.stationLocalY = -top;
        crest.end.stationLocalY = -top;
        if (crest.start.mitred){
            setTopInset(crest.start, true, piece, crest.lengthScale);
        }else{
            crest.start = SplineFenceEndPlane{};
        }
        if (crest.end.mitred){
            setTopInset(crest.end, false, piece, crest.lengthScale);
        }else{
            crest.end = SplineFenceEndPlane{};
        }
    }

    // What one crest row build owes the author beyond the pieces: how many cells stood, the widest
    // stretch a pitch-grid cell was offered, and which kind of run could hold a cell bare.
    struct CrestRowReport{
        uint32_t cellsPlaced = 0;
        float longestGridStretch = 0.0f;
        // a pitch-grid run is at least one pitch long on the crest axis, so bare it holds a cell:
        // separates "the reservations took the room" from "the run is shorter than its piece"
        bool aGridRunHoldsACell = false;
        // a run registered to its spans holds a crest on every span bare
        bool aRegisteredRunHoldsACell = false;
        bool withinBudget = true;
    };

    // the crest row over every run
    CrestRowReport emitCrestRow(const std::vector<Run>& runs, const SplineFenceLayoutParams& params, const CrestPitch& pitch,
                                uint32_t& pieceBudget, std::vector<SplineFenceCrest>& out){
        CrestRowReport report;
        uint32_t cellBase = 0;
        CrestAxis axis;
        for (const Run& run : runs){
            buildCrestAxis(run, params.spanGrade, axis);
            CrestCells cells;
            cells.pitch = pitch.metres;
            if (crestRegistersToSpans(run, params, pitch)){
                cells.spanStations = &axis.stations;
                cells.count = static_cast<uint32_t>(run.spanPoses.size());
            }else{
                cells.count = crestCellCount(axis.lengthMetres, pitch.metres);
            }
            float runLongestFree = 0.0f;
            const std::vector<CrestPlacement> placements = solveRunCrest(run, axis, params, cells, cellBase, runLongestFree);
            if (cells.registeredToSpans()){
                report.aRegisteredRunHoldsACell = report.aRegisteredRunHoldsACell || cells.count > 0u;
            }else{
                report.longestGridStretch = std::max(report.longestGridStretch, runLongestFree);
                report.aGridRunHoldsACell = report.aGridRunHoldsACell || axis.lengthMetres + MIN_RUN_LENGTH >= pitch.metres;
            }
            for (const CrestPlacement& placement : placements){
                const bool onSpan = cells.registeredToSpans() && !placement.isCap;
                const SplinePiece& piece = placement.isCap ? params.capPieces[placement.poolSlot] : params.crestPieces[placement.poolSlot];
                SplineFenceCrest crest;
                if (onSpan){
                    placeCrestOnSpan(run, params, placement.ordinal, piece, crest);
                }else if (!crestPoseOnRun(run, axis, params, placement, piece, crest.pose)){
                    continue;
                }
                if (pieceBudget == 0u){
                    report.withinBudget = false;
                    return report;
                }
                pieceBudget--;
                if (!placement.isCap){
                    report.cellsPlaced++;
                }

                crest.poolSlot = placement.poolSlot;
                crest.isCap = placement.isCap;
                crest.index = cellBase + placement.ordinal;
                crest.run = run.startKnot;
                crest.ordinalInRun = placement.ordinal;
                out.push_back(crest);
            }
            // the base counts through a run that emitted nothing, so a pick answers to the
            // geometry and not to what a reservation removed
            cellBase += cells.count;
        }
        return report;
    }

    std::string runName(uint32_t startKnot, uint32_t endKnot){
        return "run " + std::to_string(startKnot) + "->" + std::to_string(endKnot);
    }

    // The overrides, sorted to the runs they name: one list per declared run, indexed by the knot
    // opening it. An override on a knot that opens no run, and a second override on a span that
    // already has one, are reported here and read by nothing: the first one written applies.
    std::vector<std::vector<RunOverride>> collectRunOverrides(std::span<const SplineSpanOverride> authored, size_t declaredRuns,
                                                              bool closed, std::vector<std::string>& validation){
        std::vector<std::vector<RunOverride>> byRun(declaredRuns);
        for (const SplineSpanOverride& entry : authored){
            if (entry.kind == SplineSpanOverrideKind::None){
                continue;
            }
            if (entry.knotIndex >= declaredRuns){
                validation.push_back("Fence: a span override names knot " + std::to_string(entry.knotIndex) +
                                     ", which opens no run — it places nothing; move it or remove it");
                continue;
            }
            std::vector<RunOverride>& run = byRun[entry.knotIndex];
            if (findRunOverride(run, entry.spanOrdinal)){
                const uint32_t endKnot = (closed && entry.knotIndex + 1u == declaredRuns) ? 0u : entry.knotIndex + 1u;
                validation.push_back("Fence: two span overrides name span " + std::to_string(entry.spanOrdinal) + " of " +
                                     runName(entry.knotIndex, endKnot) + " — the first is used");
                continue;
            }
            run.push_back({entry.spanOrdinal, entry.kind, entry.poolSlot});
        }
        return byRun;
    }

    // What a run's overrides could not do once its fill is solved: name a span the run does not
    // have, or a pool slot the pool does not hold. A gate over an EMPTY gate pool is not a miss:
    // it is an opening, which is what an empty pool means.
    void reportRunOverrides(const Run& run, std::span<const RunOverride> overrides, const SplineFenceLayoutParams& params,
                            std::vector<std::string>& validation){
        const size_t spans = run.fill.slots.size();
        for (const RunOverride& entry : overrides){
            const std::string spanName = "span " + std::to_string(entry.ordinal) + " of " + runName(run.startKnot, run.endKnot);
            if (entry.ordinal >= spans){
                validation.push_back("Fence: a span override names " + spanName + ", which has " + std::to_string(spans) +
                                     " span(s) — it places nothing; move it or remove it");
                continue;
            }
            // pool pieces are named as their pool rows are, from 1
            if (entry.kind == SplineSpanOverrideKind::Gate && !params.gatePieces.empty() && entry.poolSlot >= params.gatePieces.size()){
                validation.push_back("Fence: the gate on " + spanName + " names Gate " + std::to_string(entry.poolSlot + 1u) +
                                     ", but the Gate pool holds " + std::to_string(params.gatePieces.size()) +
                                     " — the span is left open");
            }else if (entry.kind == SplineSpanOverrideKind::ExplicitPiece && entry.poolSlot >= params.spanPieces.size()){
                validation.push_back("Fence: the explicit piece on " + spanName + " names Span " + std::to_string(entry.poolSlot + 1u) +
                                     ", but the Span pool holds " + std::to_string(params.spanPieces.size()) +
                                     " — it keeps the fence's own pick");
            }
        }
    }

    float sanitizedFloat(float value, float fallback){
        return std::isfinite(value) ? value : fallback;
    }

    // a fence piece's pose: the frame's pivot moved off the frame by the piece's own pivot
    SplinePiecePose fencePiecePose(const SplineLayoutPose& frame, const SplinePiece& piece, float lengthScale,
                                   SplinePieceRole role, uint32_t stationIndex, uint32_t ordinal){
        SplinePiecePose pose = makePiecePose(frame, piece, lengthScale);
        pose.role = role;
        pose.stationIndex = stationIndex;
        pose.ordinal = ordinal;
        return pose;
    }

}

SplineFenceLayoutParams spline::sanitizeFenceParams(const SplineFenceLayoutParams& authored){
    const SplineFenceLayoutParams defaults{};
    SplineFenceLayoutParams clean = authored;
    clean.postPitch = std::max(sanitizedFloat(authored.postPitch, defaults.postPitch), MIN_PITCH);
    clean.slopeBlend = std::clamp(sanitizedFloat(authored.slopeBlend, defaults.slopeBlend), 0.0f, 1.0f);
    clean.spanMaxStretch = std::max(sanitizedFloat(authored.spanMaxStretch, defaults.spanMaxStretch), 1.0f);
    // a negative pitch spaces nothing, so it reads as the default 0 (the longest crest piece)
    clean.crestPitch = std::max(sanitizedFloat(authored.crestPitch, defaults.crestPitch), 0.0f);
    return clean;
}

SplineFenceLayoutResult spline::buildFenceLayout(std::span<const SplineCenterSample> center, const SplineFenceLayoutParams& params){
    SplineFenceLayoutResult result;
    if (center.size() < 2u || params.knotStations.size() < 2u){
        return result;
    }

    // Hold-then-split: the gap patch runs on the WHOLE draped polyline before it is cut into
    // runs, so a run starting inside a terrain hole inherits the altitude its neighbour ends on.
    std::vector<SplineCenterSample> held(center.begin(), center.end());
    holdSurfaceAcrossGaps(held);
    for (SplineCenterSample& sample : held){
        sample.surfaceValid = true;
    }

    const bool hasPostMesh = !params.postPieces.empty();
    const SplinePiece postPiece = hasPostMesh ? params.postPieces[0] : SplinePiece{};
    const float pitch = std::max(params.postPitch, MIN_PITCH);
    const float maxStretch = std::max(params.spanMaxStretch, 1.0f);
    const bool explicitMinScale = std::isfinite(params.spanMinScale) && params.spanMinScale > 0.0f;
    const float minScale = explicitMinScale ? std::min(params.spanMinScale, 1.0f) : 1.0f / maxStretch;
    const size_t declaredRuns = params.knotStations.size() - 1u;

    // ---- Pass A: slice each run and solve its fill. -----------------------------------------
    const std::vector<std::vector<RunOverride>> overridesByRun =
        collectRunOverrides(params.spanOverrides, declaredRuns, params.closed, result.validation);
    std::vector<Run> runs;
    runs.reserve(declaredRuns);
    result.runSpanCounts.assign(declaredRuns, 0u);
    uint32_t spanOrdinal = 0;
    for (size_t r = 0; r < declaredRuns; r++){
        const float u0 = params.knotStations[r];
        const float u1 = params.knotStations[r + 1u];

        Run run;
        run.startKnot = static_cast<uint32_t>(r);
        run.endKnot = (params.closed && r + 1u == declaredRuns) ? 0u : static_cast<uint32_t>(r + 1u);
        if (u1 > u0){
            run.samples = sliceRun(held, u0, u1);
            run.lengthMetres = polylineLength(run.samples);
        }

        if (run.lengthMetres <= MIN_RUN_LENGTH){
            result.validation.push_back("Fence: knots " + std::to_string(run.startKnot) + " and " + std::to_string(run.endKnot) +
                                        " are coincident — the run between them is skipped and the station emitted once");
            // the run holds no span, so nothing its overrides name exists
            if (!overridesByRun[r].empty()){
                result.validation.push_back("Fence: " + std::to_string(overridesByRun[r].size()) + " span override(s) on " +
                                            runName(run.startKnot, run.endKnot) + " place nothing — the run has no length; move knot " +
                                            std::to_string(run.endKnot) + " or remove them");
            }
            continue;
        }

        run.fill = solveRunFill(run.lengthMetres, params.seed, spanOrdinal, params.spanPieces, params.gatePieces,
                                overridesByRun[r], maxStretch, minScale);
        // The fill answers for its own stretch; the reach a mitred end adds is checked against the
        // same cap where the joins are decided.
        if (!run.fill.slots.empty() && !run.fill.withinCap){
            // which bound was missed decides the wording: a compression is under the floor
            const bool overCap = run.fill.stretch > maxStretch;
            const std::string bound = overCap ? "past the " + factorText(maxStretch) + "x cap"
                                              : "under the " + factorText(minScale) + "x floor" +
                                                    (explicitMinScale ? " (explicit compression limit)"
                                                                      : " (the " + factorText(maxStretch) + "x cap's reciprocal)");
            result.validation.push_back("Fence: run " + std::to_string(run.startKnot) + "->" + std::to_string(run.endKnot) +
                                        " is " + metresText(run.lengthMetres) + " m and its closest fill of " +
                                        std::to_string(run.fill.slots.size()) + " span piece(s) is " +
                                        metresText(run.fill.naturalLength) + " m — needs " + factorText(run.fill.stretch) + "x, " +
                                        bound + "; no count within one of the nominal " + std::to_string(run.fill.nominalCount) +
                                        " satisfies it, so the run is built at the required stretch");
        }

        // Stations: cumulative piece lengths scaled to close the run exactly on both knots, which
        // are mandatory stations. With no span pieces the walk falls back to the pitch.
        if (run.fill.slots.empty()){
            const int64_t rounded = static_cast<int64_t>(std::lround(run.lengthMetres / pitch)) + 1;
            const uint32_t count = static_cast<uint32_t>(std::max<int64_t>(int64_t{2}, rounded));
            run.stations.reserve(count);
            for (uint32_t k = 0; k < count; k++){
                run.stations.push_back(run.lengthMetres * static_cast<float>(k) / static_cast<float>(count - 1u));
            }
        }else{
            run.stations.reserve(run.fill.slots.size() + 1u);
            float cumulative = 0.0f;
            run.stations.push_back(0.0f);
            for (size_t k = 0; k < run.fill.slots.size(); k++){
                cumulative += std::max(spanPieceOf(run, k, params).length(), MIN_PIECE_LENGTH);
                run.stations.push_back(run.lengthMetres * cumulative / run.fill.naturalLength);
            }
            run.stations.back() = run.lengthMetres;
            spanOrdinal += static_cast<uint32_t>(run.fill.slots.size());
        }
        reportRunOverrides(run, overridesByRun[r], params, result.validation);
        result.runSpanCounts[r] = static_cast<uint32_t>(run.fill.slots.size());

        runs.push_back(std::move(run));
    }

    if (runs.empty()){
        return result;
    }

    // ---- Pass B: each run's end directions, with no probe. ----------------------------------
    // The chord is a pure function of the polyline, so this costs no rays; running it through the
    // same layout keeps the chord rule in one place. The tilt report is not on the template: only
    // the real station pass reports.
    SplineTileFrameParams stationTemplate;
    stationTemplate.spacing = pitch;
    stationTemplate.alignToSurfaceNormal = params.alignToSurfaceNormal;
    stationTemplate.slopeBlend = params.slopeBlend;
    stationTemplate.meshBoundsCenter = postPiece.center;
    stationTemplate.meshBoundsHalfExtents = postPiece.halfExtents;
    stationTemplate.plantMode = params.plantMode;
    stationTemplate.maxTiles = params.maxPieces;
    stationTemplate.maxTiltDegrees = params.maxTiltDegrees;

    std::vector<SplineLayoutPose> endPoses;
    for (Run& run : runs){
        const float ends[2] = {0.0f, run.lengthMetres};
        SplineTileFrameParams endParams = stationTemplate;
        endParams.stationDistances = std::span<const float>(ends, 2u);
        buildTileFrames(run.samples, endParams, endPoses);
        if (endPoses.size() == 2u){
            run.startForward = groundDirection(endPoses[0].forward, run.startForward);
            run.endForward = groundDirection(endPoses[1].forward, run.endForward);
        }
    }

    // ---- Pass C: bisector yaw at every shared station. --------------------------------------
    // Each run's own chord would give the same physical post two different answers.
    for (Run& run : runs){
        run.stationForwards.assign(run.stations.size(), Vector3(0.0f, 0.0f, 0.0f));
    }

    // a closed spline joins its last run back to its first, so every one of its stations is a
    // two-run corner
    const size_t sharedJoints = params.closed ? runs.size() : runs.size() - 1u;
    for (size_t joint = 0; joint < sharedJoints; joint++){
        const size_t next = (joint + 1u) % runs.size();
        if (next == joint){
            continue;
        }
        const Vector3 bisector = normalizedOrFallback(runs[joint].endForward + runs[next].startForward, runs[joint].endForward);
        runs[joint].stationForwards.back() = bisector;
        runs[next].stationForwards.front() = bisector;
    }

    // a node several splines meet at faces one way in all of their layouts, which no single
    // spline's bisector can know; an entry left at zero keeps the bisector
    if (!params.knotForwards.empty()){
        for (Run& run : runs){
            applyKnotForward(params.knotForwards, run.startKnot, run.stationForwards.front());
            applyKnotForward(params.knotForwards, run.endKnot, run.stationForwards.back());
        }
    }

    // ---- Pass D: the real station poses, probed. --------------------------------------------
    for (Run& run : runs){
        SplineTileFrameParams runParams = stationTemplate;
        runParams.stationDistances = std::span<const float>(run.stations);
        runParams.stationForwards = std::span<const Vector3>(run.stationForwards);
        runParams.probe = params.probe;
        runParams.outTiltClamp = params.outTiltClamp;
        buildTileFrames(run.samples, runParams, run.poses);
    }

    // ---- Pass D2: span frames. --------------------------------------------------------------
    for (Run& run : runs){
        const size_t spanCount = run.poses.size() < 2u ? size_t{0} : std::min(run.fill.slots.size(), run.poses.size() - 1u);
        run.spanPoses.reserve(spanCount);
        run.spanLengthScales.reserve(spanCount);
        for (size_t k = 0; k < spanCount; k++){
            float lengthScale = 1.0f;
            bool rollBorrowed = false;
            run.spanPoses.push_back(makeSpanPose(run.poses[k], run.poses[k + 1u], params.spanGrade, params.plantMode,
                                                 spanPieceOf(run, k, params), lengthScale, rollBorrowed));
            run.spanLengthScales.push_back(lengthScale);
            run.spanRollBorrowed.push_back(rollBorrowed ? 1u : 0u);
            if (rollBorrowed){
                run.rollBorrowedSpans++;
            }
        }
    }

    // ---- Pass D3: span joins. ---------------------------------------------------------------
    mitreBareJoins(runs, params, result.validation);

    // ---- Pass E: emit. ----------------------------------------------------------------------
    // A shared station belongs to both runs; it is emitted with the run that opens it, so the
    // run that closes on it contributes only its spans.
    uint32_t stationIndex = 0;
    uint32_t spanIndex = 0;
    uint32_t pieceBudget = params.maxPieces;
    // two different cuts, two latches
    bool budgetReported = false;
    bool truncationReported = false;
    bool shortRunReported = false;
    // true while another piece may be emitted; reports the cut exactly once
    const auto takePieceBudget = [&](){
        if (pieceBudget > 0u){
            pieceBudget--;
            return true;
        }
        if (!budgetReported){
            budgetReported = true;
            result.validation.push_back("Fence: hit the " + std::to_string(params.maxPieces) +
                                        " piece budget; the rest of the spline is not built — raise the post pitch or shorten the spline");
        }
        return false;
    };

    for (const Run& run : runs){
        if (run.poses.size() < 2u){
            if (!shortRunReported){
                shortRunReported = true;
                result.validation.push_back("Fence: run " + std::to_string(run.startKnot) + "->" + std::to_string(run.endKnot) +
                                            " produced no usable stations and contributes nothing — its centerline has no measurable "
                                            "length after draping");
            }
            continue;
        }
        // the station walk has its own budget, and a run long enough to exhaust it comes back
        // short: emit what it produced and say so
        if (run.poses.size() != run.stations.size() && !truncationReported){
            truncationReported = true;
            result.validation.push_back("Fence: run " + std::to_string(run.startKnot) + "->" + std::to_string(run.endKnot) +
                                        " needed " + std::to_string(run.stations.size()) + " stations but the layout budget allows " +
                                        std::to_string(run.poses.size()) + " — raise the post pitch");
        }

        const uint32_t runFirstStation = stationIndex;
        const size_t emitCount = run.poses.size() - 1u;
        for (size_t k = 0; k < emitCount; k++){
            if (!takePieceBudget()){
                break;
            }
            SplineFenceStation station;
            station.pose = run.poses[k];
            station.index = stationIndex++;
            station.isKnot = (k == 0u);
            station.knot = run.startKnot;
            result.stations.push_back(station);
        }

        // spans the emission reached, openings included: an opening places no piece and spends
        // no budget, but it keeps its ordinal
        size_t spansEmitted = 0;
        for (size_t k = 0; k < run.spanPoses.size(); k++){
            if (isOpening(run, k)){
                spanIndex++;
                spansEmitted++;
                continue;
            }
            if (!takePieceBudget()){
                break;
            }
            SplineFenceSpan span;
            span.poolSlot = run.fill.slots[k];
            span.isGate = run.fill.sources[k] == SpanSource::Gate;
            shapeEmittedSpan(run, k, params, span.pose, span.lengthScale, span.start, span.end);
            span.index = spanIndex++;
            span.run = run.startKnot;
            span.ordinalInRun = static_cast<uint32_t>(k);
            span.station = runFirstStation + static_cast<uint32_t>(k);
            result.spans.push_back(span);
            spansEmitted++;
        }
        result.rollBorrowedSpans += run.rollBorrowedSpans;

        // stepped risers need a station tall enough to cover them; with no post mesh the span's
        // own height is the cover
        if (params.spanGrade == SplineSpanGrade::Stepped && !run.fill.slots.empty()){
            float worstStep = 0.0f;
            for (size_t k = 0; k + 1u < run.poses.size(); k++){
                worstStep = std::max(worstStep, std::abs(run.poses[k + 1u].base.y - run.poses[k].base.y));
            }
            float cover = hasPostMesh ? postPiece.height() : 0.0f;
            if (cover <= 0.0f){
                cover = spanPieceOf(run, 0u, params).height();
            }
            if (cover > 0.0f && worstStep > cover){
                result.validation.push_back("Fence: run " + std::to_string(run.startKnot) + "->" + std::to_string(run.endKnot) +
                                            " steps " + metresText(worstStep) + " m between stations, more than the " +
                                            metresText(cover) + " m piece height covers — the stepped grade leaves a gap at the riser");
            }
        }

        // Buried spans: the run's draped samples are already dense, so checking whether the ground
        // rises past what a span is planted for is nearly free. The line comes from the SPAN'S OWN
        // POSE and pool piece (a span that borrowed its roll does not lie on the post-to-post
        // line), and the walk keeps the WORST sample: the one station worth adding.
        float cumulative = 0.0f;
        uint32_t buriedSamples = 0;
        float worstDepth = 0.0f;
        size_t worstSpanOrdinal = 0;
        float worstArcMetres = 0.0f;
        Vector3 worstPos(0.0f, 0.0f, 0.0f);
        for (size_t i = 1; i + 1u < run.samples.size(); i++){
            cumulative += vectorLength(run.samples[i].position - run.samples[i - 1u].position);
            const auto upper = std::upper_bound(run.stations.begin(), run.stations.end(), cumulative);
            if (upper == run.stations.begin() || upper == run.stations.end()){
                continue;
            }
            const size_t hi = static_cast<size_t>(upper - run.stations.begin());
            if (hi > spansEmitted || hi > run.spanPoses.size() || isOpening(run, hi - 1u)){
                continue;
            }
            const SplinePiece& piece = spanPieceOf(run, hi - 1u, params);
            const float allowed = spanGroundAllowanceAt(run.spanPoses[hi - 1u], piece, params.plantMode, run.samples[i].position);
            const float depth = run.samples[i].position.y - allowed;
            if (depth <= BURY_TOLERANCE){
                continue;
            }
            buriedSamples++;
            // ties go to the first such sample, in walk order along the run
            if (buriedSamples == 1u || depth > worstDepth){
                worstDepth = depth;
                worstSpanOrdinal = hi - 1u;
                worstArcMetres = cumulative;
                worstPos = run.samples[i].position;
            }
        }
        if (buriedSamples > 0u){
            result.validation.push_back("Fence: the ground rises through the spans of run " + std::to_string(run.startKnot) + "->" +
                                        std::to_string(run.endKnot) + " at " + std::to_string(buriedSamples) +
                                        " sample(s) — worst at span " + std::to_string(worstSpanOrdinal) + ", " +
                                        metresText(worstDepth) + " m deeper than the piece is planted for, " +
                                        metresText(worstArcMetres) + " m along the run at (" + metresText(worstPos.x) + ", " +
                                        metresText(worstPos.z) +
                                        ") — add a knot there so a station lands on the bump, or the fence is buried at that span");
        }
    }

    // emission is in order, so where the budget cut it the one span whose partner is missing is
    // the last emitted, and on a closed spline the first, whose partner is the loop's last span
    if (budgetReported && !result.spans.empty()){
        if (result.spans.back().end.mitred){
            squareUnpairedEnd(runs, params, false, result.spans.back());
        }
        if (params.closed && result.spans.front().start.mitred){
            squareUnpairedEnd(runs, params, true, result.spans.front());
        }
    }

    // an open spline's final station closes the last run and belongs to no following one; a
    // closed spline's would be its first station again
    if (!params.closed){
        const Run& last = runs.back();
        if (last.poses.size() >= 2u && takePieceBudget()){
            SplineFenceStation station;
            station.pose = last.poses.back();
            station.index = stationIndex++;
            station.isKnot = true;
            station.knot = last.endKnot;
            result.stations.push_back(station);
        }
    }

    // ---- Pass F: the crest row. -------------------------------------------------------------
    // after every station and span: one piece budget, and decoration is what it cuts first
    if (!params.crestPieces.empty()){
        const CrestPitch crest = resolveCrestPitch(params.crestPieces, params.crestPitch);
        if (crest.raisedToPieceLength){
            result.validation.push_back("Fence: Crest Pitch " + metresText(params.crestPitch) + " m is below the longest crest piece, " +
                                        metresText(crest.longestPiece) + " m; the piece length is used");
        }
        if (params.spanPieces.empty()){
            result.validation.push_back("Fence: the crest row is laid along the tops of the spans and the span pool is empty — no "
                                        "crest piece is placed");
        }else{
            const CrestRowReport row = emitCrestRow(runs, params, crest, pieceBudget, result.crests);
            if (!row.withinBudget && !budgetReported){
                budgetReported = true;
                result.validation.push_back("Fence: hit the " + std::to_string(params.maxPieces) +
                                            " piece budget; the rest of the crest row is not built — raise the crest pitch or shorten "
                                            "the spline");
            }else if (row.withinBudget && row.cellsPlaced == 0u && row.aGridRunHoldsACell){
                // a run could hold a cell bare and not one stood: the posts or the caller's pieces
                // took the room, which the result alone does not show. A run SHORTER than a pitch
                // stays silent: the wall itself shows why.
                result.validation.push_back(emptyCrestRowReport(row.longestGridStretch, crest.asked, hasPostMesh));
            }else if (row.withinBudget && row.cellsPlaced == 0u && row.aRegisteredRunHoldsACell){
                result.validation.push_back("Fence: the crest row placed no crest pieces — a reserved stretch reaches every wall they "
                                            "stand on; move what reserves the run.");
            }
        }
    }

    return result;
}

void spline::appendFencePiecePoses(const SplineFenceLayoutResult& layout, const SplineFenceLayoutParams& params,
                                   std::vector<SplinePiecePose>& out){
    out.reserve(out.size() + layout.stations.size() + layout.spans.size() + layout.crests.size());
    if (!params.postPieces.empty()){
        for (const SplineFenceStation& station : layout.stations){
            // the station was centred for the pool's first post; a post of other bounds is
            // re-centred on the same base for its own
            const SplinePiece& post = params.postPieces[splinePickPiece(params.seed, SplinePoolRole::Post, station.index, params.postPieces)];
            SplineLayoutPose frame = station.pose;
            const SplinePieceBasis basis = makePieceBasis(frame, post.axis());
            frame.position = frame.base + frame.up * plantedPivotLift(post, params.plantMode) -
                             basis.localX * post.center.x - basis.localZ * post.center.z;
            out.push_back(fencePiecePose(frame, post, 1.0f, SplinePieceRole::Post, station.index, station.index));
        }
    }
    for (const SplineFenceSpan& span : layout.spans){
        const SplinePiece& piece = span.isGate ? params.gatePieces[span.poolSlot] : params.spanPieces[span.poolSlot];
        out.push_back(fencePiecePose(span.pose, piece, span.lengthScale, span.isGate ? SplinePieceRole::Gate : SplinePieceRole::Span,
                                     span.station, span.index));
    }
    for (const SplineFenceCrest& crest : layout.crests){
        const SplinePiece& piece = crest.isCap ? params.capPieces[crest.poolSlot] : params.crestPieces[crest.poolSlot];
        out.push_back(fencePiecePose(crest.pose, piece, crest.lengthScale, crest.isCap ? SplinePieceRole::Cap : SplinePieceRole::Crest,
                                     crest.index, crest.index));
    }
}
