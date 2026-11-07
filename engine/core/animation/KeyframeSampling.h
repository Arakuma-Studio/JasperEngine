// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 3.1, 3.3, 9.R16/R21)
// - Purpose: the keyframe samplers as pure functions over const arrays, with the key cursor
//   held by the caller. Two users, one maths:
//   * ActionSystem's entity tracks (KeyframeTracksComponent + Translate/Rotate/Scale/Morph/
//     LatticeTracksComponent): locateKey + the value samplers replace keyframeUpdate and the
//     file-static samplers, with no behaviour change (ActionTracks/ActionTiming tests);
//   * clips (AnimationClip::Track): the Animation Player, ActionSystem::previewPlayer /
//     sampleClip, MCP get_animation `times`, the Animation window's curves view.
// - Thread-safety: pure; no engine state.
// - Invariants:
//   * locateKey reproduces ActionSystem::keyframeUpdate exactly: `index` is the first key whose
//     time is >= `time` (the last key past the end), `interpolation` the 0..1 position in the
//     segment (index - 1 .. index), clamped to 1 and shaped by easings[index - 1] (missing or
//     LINEAR: linear). At or before the first key index is 0 (the samplers return key 0).
//   * stepped keys (Unity's rule, both samplers): a STEP segment holds its start key's value
//     and its end key shows its own value at its time, from 1e-6 s before it (the clip time
//     resolution: an .anim key time is the entity key's float rounded to 1e-6). The slack is
//     1e-6 + |t|·FLT_EPSILON (about 12 µs at t = 100 s): an entity track and its clip agree
//     exactly at key times, and can still differ in a band of at most that width just before
//     a stepped key, where one time is past the rounded key time and the other is not (one
//     frame shows the next key about a microsecond early; not visible).
//   * a cursor is only a hint (a search start): results never depend on it (spec 3.3
//     determinism); a stale cursor costs time, never correctness. The cursor walk needs
//     sorted times (every AnimationClip track); without a cursor locateKey scans from key 0
//     like keyframeUpdate did, which entity tracks (times not guaranteed sorted) rely on.
//   * sampleTrack returns exactly the first key at or before its time (and for a NaN time) and
//     exactly the last key at or after its time (no interpolation round-off at the ends); it
//     binary-searches when no cursor is given, and with one walks a few keys from the hint
//     before falling back to a binary search. locateKey with a NaN time gives index 0, with or
//     without a cursor.
//   * Hermite (cubic spline) is used when the segment's tangents are given (both non-null);
//     rotations are normalized with the zero-norm guard (the nearer key), slerp otherwise: a
//     normalised lerp where that stays within 0.01 mm at 1 m of slerp (keys up to 30 degrees
//     apart, S74 R12-R13), exact keys at t = 0 and 1, always the shortest arc.
//   * samples are the raw track values: relative tracks are combined with their rest by the
//     caller (position: rest + value through getRelativeTrackMatrix; rotation rest * value;
//     scale rest * value).

#ifndef KEYFRAME_SAMPLING_H
#define KEYFRAME_SAMPLING_H

#include "Export.h"
#include "action/Ease.h"
#include "animation/AnimationClip.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstddef>
#include <vector>

namespace jasperengine::anim{

    // where the last lookup on a track ended (a search hint, per track and per user)
    struct KeyCursor{
        size_t index = 0;
    };

    struct KeyLocation{
        size_t index = 0;           // the key at or after the time: the segment is index-1 .. index
        float interpolation = 0.0f; // 0..1 in the segment, eased
        float segmentDuration = 0.0f; // times[index] - times[index-1]; 0 at index 0
    };

    // the eased factor (EaseType::CUSTOM and CURVE: linear)
    JASPERENGINE_API float applyEase(EaseType ease, float t);

    // `easings` may be shorter than the segment count (or null): linear there
    JASPERENGINE_API KeyLocation locateKey(const float* times, size_t keyCount,
                                           const EaseType* easings, size_t easingCount,
                                           float time, KeyCursor* cursor = nullptr);

    // A location is usable for a track with `valueCount` values (entity tracks may have fewer
    // values than times while edited: they freeze instead of reading out of bounds)
    inline bool keyIndexValid(const KeyLocation& at, size_t valueCount){
        return valueCount > 0 && at.index < valueCount;
    }

    // One value per key (position, scale, rotation). `values` has at least at.index + 1 entries;
    // tangents null for linear tracks, else parallel to values.
    JASPERENGINE_API Vector3 sampleVector3(const KeyLocation& at, const Vector3* values,
                                           const Vector3* inTangents, const Vector3* outTangents);
    JASPERENGINE_API Quaternion sampleQuaternion(const KeyLocation& at, const Quaternion* values,
                                                 const Quaternion* inTangents, const Quaternion* outTangents);

    // A row of `width` values per key (morph weights, lattice offsets): `previous` / `next` are
    // the rows of keys at.index - 1 (at.index at index 0) and at.index; tangent rows null when
    // linear. Writes `width` values to `out`.
    JASPERENGINE_API void sampleFloatRow(const KeyLocation& at, const float* previous, const float* next, size_t width,
                                         const float* previousOut, const float* nextIn, float* out);
    JASPERENGINE_API void sampleVector3Row(const KeyLocation& at, const Vector3* previous, const Vector3* next, size_t width,
                                           const Vector3* previousOut, const Vector3* nextIn, Vector3* out);

    // ---- clips ----

    // A state's time (seconds, any value, negative when played backwards past 0) as a time in
    // the clip of `length` under `wrap`: Once / ClampForever clamp to [0, length], Loop wraps,
    // PingPong goes back and forth. `wrap` Default counts as Once.
    JASPERENGINE_API float wrapClipTime(float time, float length, ClipWrapMode wrap);
    // A clip time as a time in the track (a looping track repeats its own keys)
    JASPERENGINE_API float trackTime(const AnimationClip::Track& track, float clipTime);

    // One track's raw value at a clip time (no relative or rest applied)
    struct JASPERENGINE_API TrackSample{
        ClipChannel channel = ClipChannel::Position;
        bool valid = false;                 // false: an unknown property track or no keys
        Vector3 vector = Vector3(0, 0, 0);  // position, scale
        Quaternion rotation;                // rotation
        std::vector<float> weights;         // morph, property (track width; property in key space)
        std::vector<Vector3> offsets;       // lattice, parallel to the track's points
    };

    // `cursor` (optional) is the track's search hint
    JASPERENGINE_API bool sampleTrack(const AnimationClip::Track& track, float clipTime, TrackSample& out,
                                      KeyCursor* cursor = nullptr);
    // A property track's row stepped (the key at or before the time; at a key time, that key, with
    // the STEP rule's slack): what a discrete binding of a track whose keys interpolate reads (a
    // script's Int field keyed with numbers)
    JASPERENGINE_API bool sampleTrackStepped(const AnimationClip::Track& track, float clipTime, TrackSample& out);
    // every track of the clip at `clipTime` (out[i] is getTracks()[i])
    JASPERENGINE_API void sampleClipTracks(const AnimationClip& clip, float clipTime, std::vector<TrackSample>& out);

    // Events a state crossed when its time went from `fromTime` to `toTime` this update (from =
    // the stored state time, to = from + delta: no wrap applied by the caller), in firing order
    // (spec 9.R16): forward fires fromTime < t <= toTime, backward toTime <= t < fromTime;
    // `includeStart` also fires events exactly at fromTime (a state that just started: at 0, or
    // at its length when played backwards, R18); a loop or ping-pong boundary splits the
    // interval; each event at most once per update. Appends each event's index into
    // clip.getEvents() with where it was crossed: its position on the state's unwrapped time
    // line (between fromTime and toTime), for ordering callbacks of several states by time.
    struct CrossedEvent{
        size_t index = 0;       // into clip.getEvents()
        double position = 0.0;
    };
    JASPERENGINE_API void collectCrossedEvents(const AnimationClip& clip, ClipWrapMode wrap, float fromTime, float toTime,
                                               bool includeStart, std::vector<CrossedEvent>& out);
    // The audio cues crossed, by the same rules (S40 R4): `index` is into clip.getAudioCues(). A clip
    // without cues returns at once.
    JASPERENGINE_API void collectCrossedCues(const AnimationClip& clip, ClipWrapMode wrap, float fromTime, float toTime,
                                             bool includeStart, std::vector<CrossedEvent>& out);

}

#endif //KEYFRAME_SAMPLING_H
