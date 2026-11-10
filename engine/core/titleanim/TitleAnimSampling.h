// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R3-R10, D2-D4, §6.6)
// - Purpose: the pure maths of title animation: the wrap with a loop section (D3), key tracks
//   compiled into KeyframeSampling's layout and sampled by its functions unchanged (D2: eases,
//   tangents and the step slack behave as clip tracks do), drives, reseeds, stagger orders and
//   delays (R8), marker crossings on the unwrapped time line (D4), and the title's draw values
//   (R13) with the companion's affine.
// - Determinism (R31): no clock, no global state but the test flag; one animation, time and
//   platform give bit-identical samples. sampleAnimation allocates nothing once the pose is
//   sized (R35).
// - Thread-safety: pure functions; the test flag is set before other threads sample.

#ifndef TITLE_ANIM_SAMPLING_H
#define TITLE_ANIM_SAMPLING_H

#include "Export.h"
#include "animation/KeyframeSampling.h"
#include "titleanim/TitleAnimTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::titleanim{

    // D3: a local time for `time` seconds since the start (>= 0; negative clamps to 0). The first
    // pass plays 0..D; then `loop` gives loopStart + fmod(t - loopStart, D - loopStart),
    // `ping_pong` runs back and forth over [loopStart, D], `once` clamps to D.
    JASPERENGINE_API double wrapAnimationTime(double time, double duration, double loopStart, AnimWrap wrap);
    JASPERENGINE_API double wrapAnimationTime(double time, const CompiledTitleAnimation& animation);

    // ---- compiling ----

    // Fills a numeric track's key arrays (times, easings, values `width` per key, tangents) from
    // time-sorted keys, the way an .anim track is normalized: computed tangent modes get their
    // tangents (anim::applyTangentModes), keys without tangents in a track where some have them
    // get linear ones, and a key with tangents drops its ease (tangents win over the ease, except
    // step). Returns the number of eases dropped that way.
    JASPERENGINE_API size_t compileNumericKeys(const std::vector<ClipKey>& keys, size_t width, CompiledTitleTrack& out);
    // A stepped track: each key's JSON text in stepValues, the key index as its value, STEP eases
    JASPERENGINE_API void compileSteppedKeys(const std::vector<TitleKey>& keys, CompiledTitleTrack& out);

    // ---- stagger (R8) ----

    // the rank of each of `n` units in `order` (rank 0 starts first); center_out and edges_in give
    // the two units at one distance from the centre one rank. `random` is a Fisher-Yates
    // permutation from SplitMix64 of the document seed and the stagger seed.
    JASPERENGINE_API void staggerRanks(StaggerOrder order, size_t n, uint64_t documentSeed, uint32_t staggerSeed, std::vector<uint32_t>& ranks);
    // delay_i = rank_i x each, or with `over`, over x ease(rank_i / max rank); one unit: 0
    JASPERENGINE_API void staggerDelays(const TitleStagger& stagger, size_t n, uint64_t documentSeed, std::vector<float>& delays);

    // ---- sampling ----

    // One track's raw value at local time `time` into out[0..width): keys (held before the first
    // and after the last), drive or reseed, neither combined with the rest nor clamped (the pose
    // does both). A stepped track writes the index of the key that shows. `spanStart` / `spanLength` are
    // the animation's loop span (drives with cycles). `cursor` is a search hint (results never
    // depend on it).
    JASPERENGINE_API void sampleTrackValue(const CompiledTitleTrack& track, double time, double spanStart, double spanLength, double* out,
                                           anim::KeyCursor* cursor = nullptr);
    // Every track at local time `time` (already wrapped): each unit of a staggered track reads at
    // time - delay. Relative tracks are combined with their rest, then every value is clamped to
    // its field's range. `out` must be sized for the
    // animation (TitleAnimPose::size); nothing is allocated.
    JASPERENGINE_API void sampleAnimation(const CompiledTitleAnimation& animation, double time, TitleAnimPose& out);
    // A relative or absolute value combined with the rest: Add, Multiply or Replace per element
    JASPERENGINE_API void combineWithRest(TrackCombine combine, const double* sample, const float* rest, size_t width, double* out);

    // ---- markers (R10, R28, D4) ----

    struct CrossedMarker{
        size_t index = 0;           // into CompiledTitleAnimation::markers
        double position = 0.0;      // where on the unwrapped time line it was crossed
    };
    // Markers crossed when the unwrapped local time went from `from` to `to`, in firing order:
    // collectCrossedEvents' rules on the unwrapped line split into passes (the first 0..D, then
    // each pass of the loop section): forward fires from < t <= to, backward to <= t < from;
    // `includeStart` fires those exactly at `from` too (a start); each marker at most once per pass
    // and per call; a marker before loopStart only in the first pass. Appends to `out`.
    JASPERENGINE_API void collectCrossedMarkers(const CompiledTitleAnimation& animation, double from, double to, bool includeStart,
                                                std::vector<CrossedMarker>& out);
    // the same under another wrap than the animation's (a play's loop override)
    JASPERENGINE_API void collectCrossedMarkers(const CompiledTitleAnimation& animation, AnimWrap wrap, double from, double to,
                                                bool includeStart, std::vector<CrossedMarker>& out);
    // tests (AC6's red control): crossings on the wrapped local times, without the pass split
    JASPERENGINE_API void setMarkerPassSplitDisabledForTesting(bool disabled);

    // ---- the title's draw values (R5, R13) ----

    // Adds the title-level tracks of a sampled pose to `summary` (relative or forced relative:
    // combined with what is there; absolute: replacing it). `liveRest` holds the instance's Live
    // Effect values by TitleField (null: the fields' defaults), the rest of a relative live
    // track that no earlier animation wrote. Title tracks' pose values are their raw samples
    // (a relative track's rest is its combination's identity).
    JASPERENGINE_API void accumulateTitlePose(const CompiledTitleAnimation& animation, const TitleAnimPose& pose, bool forceRelative,
                                              const double* liveRest, TitlePoseSummary& summary);

    // the still's frame as the companion shows it (S81's sidecar): canvas pixels (image space),
    // the document origin in frame pixels, the rest ink bounds (canvas pixels, image space)
    struct JASPERENGINE_API TitleFrameGeometry{
        double frameX = 0.0, frameY = 0.0, frameWidth = 0.0, frameHeight = 0.0;
        double originX = 0.0, originY = 0.0;
        double inkX = 0.0, inkY = 0.0, inkWidth = 0.0, inkHeight = 0.0;
    };
    // the pivot of the title tracks: the first transform track's `pivot` that is not origin
    JASPERENGINE_API TrackPivot titlePivotOf(const CompiledTitleAnimation& animation);

    // A 2x3 affine in frame-normalised space (0..1, y down, the companion's uv): rows (a, b, c)
    // and (d, e, f) so that n' = (a n.x + b n.y + c, d n.x + e n.y + f)
    struct JASPERENGINE_API FrameAffine{
        double a = 1.0, b = 0.0, c = 0.0, d = 0.0, e = 1.0, f = 0.0;
        FrameAffine inverse() const;
        FrameAffine then(const FrameAffine& next) const;    // next(this(n))
        void apply(double x, double y, double& ox, double& oy) const;
        bool identity() const;
    };
    // The summary's transform (position, rotation, scale, stretch about the pivot) as a frame
    // affine that moves the still's frame: where each point of the frame goes
    JASPERENGINE_API FrameAffine titleFrameAffine(const TitlePoseSummary& summary, const TitleFrameGeometry& frame, TrackPivot pivot);
    // The affine that places a flipbook frame (canvas pixels, image space) over the still's frame
    // (normalised coordinates of the still's frame to those of the flipbook frame's corners)
    JASPERENGINE_API FrameAffine flipbookFrameAffine(const TitleFrameGeometry& still, double x, double y, double width, double height);

    // SplitMix64 (stagger and preset generators)
    struct JASPERENGINE_API SplitMix64{
        uint64_t state = 0;
        explicit SplitMix64(uint64_t seed): state(seed){}
        uint64_t next();
        double unit();              // [0, 1)
    };
    JASPERENGINE_API uint64_t mixSeeds(uint64_t a, uint64_t b);

}

#endif //TITLE_ANIM_SAMPLING_H
