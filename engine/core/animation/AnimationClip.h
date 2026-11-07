// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 2.1-2.5, 3.1, 9.R8/R20/R35-R38)
// - Purpose: the two forms of a keyframe clip (.anim file or model sub-clip):
//   * ClipData: the editable value form. Parsed and written by AnimationClipIO, built by the
//     model sub-clip builder, edited by AnimationClipDocument, the Animation window, MCP and the
//     converters (value semantics: copy, change, commit).
//   * AnimationClip: the immutable runtime form built once from a ClipData (AnimationClip::build),
//     shared through AnimationClipPool by every player and preview: per-track typed arrays in
//     contiguous storage that KeyframeSampling reads with key cursors held by the caller. Tracks
//     with identical key times share one range of it, and an all-linear track has no easings.
// - Thread-safety: ClipData is a plain value. An AnimationClip never changes after build, so any
//   thread may read it (the pool hands out shared_ptr<const AnimationClip>).
// - Invariants of a ClipData that came out of the loader (parseClip / buildSubClip):
//   * tracks sorted by path (byte order), then channel in ClipChannel enum order, unique per
//     (path, channel) (property tracks: per (path, component, property), after the others);
//     keys sorted by time, no two closer than 1e-6 s (merging only ever happens at load /
//     normalize, never inside an editor drag: see AnimationClipController's drag API);
//   * every key of a track has the value width of its channel (valueWidth): position/scale 3,
//     rotation 4 (x, y, z, w; normalized, hemisphere continuous along the track), morph the
//     track's widest key (padded with 0, at most MAX_MORPHTARGETS), lattice 3 per entry of
//     `points` (the union of the keys' points; a key lacking one holds 0);
//   * tangents are empty or the value's width, on both sides (in and out); tangents win over
//     the ease (which the loader then resets to linear, with a warning), except a step (a
//     Constant segment: it holds the key's value);
//   * wrap is never ClipWrapMode::Default (that value only appears in player clip entries);
//   * events sorted by (time, function); at most MAX_ANIMATION_EVENT_ARGS args each.
//   * audio cues as ClipAudio.h says (sorted by time, valid; S40).
//   Editors that build ClipData by hand keep these (AnimationClipIO::normalize restores them).
// - Paths are relative to the animated root (ClipBinding grammar); asset paths (previewModel,
//   source.path) are asset-relative like ModelComponent::filename.
// - Quaternions: ClipKey values are x y z w, engine Quaternion is (w, x, y, z) and its
//   three-float constructor means Euler angles: convert only with quaternionFromXYZW /
//   quaternionToXYZW.

#ifndef ANIMATION_CLIP_H
#define ANIMATION_CLIP_H

#include "Export.h"
#include "action/Ease.h"
#include "animation/AnimationEvent.h"
#include "animation/ClipAudio.h"
#include "animation/ClipTypes.h"
#include "math/Quaternion.h"
#include "math/Vector3.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace jasperengine::anim{
    struct ClipParseResult;
}

namespace jasperengine{

    // ClipKey rotation layout (x y z w) <-> engine Quaternion (w, x, y, z)
    JASPERENGINE_API Quaternion quaternionFromXYZW(const float* xyzw);
    JASPERENGINE_API void quaternionToXYZW(const Quaternion& q, float* xyzw);

    // Unity's key tangent modes (S21 R27), editor intent kept with the key: the computed ones
    // (ClampedAuto, Auto, Linear, Flat, Constant) are recomputed from the neighbouring keys by
    // every normalize, Free (in = out) and Broken (in, out apart) keep the stored tangents.
    // None: no mode (an eased key, or tangents as imported).
    // ClipTangentMode: animation/ClipTypes.h

    struct JASPERENGINE_API ClipKey{
        float time = 0.0f;
        // flattened value, width ClipData::valueWidth(channel, ...): see the invariants above
        std::vector<float> value;
        // the ease of the segment that starts at this key (EaseType names; never CUSTOM)
        EaseType ease = EaseType::LINEAR;
        // Hermite tangents (glTF CUBICSPLINE semantics, value units per second): both empty or
        // both the value's width
        std::vector<float> inTangent;
        std::vector<float> outTangent;
        // the editor's tangent mode (written as "tangent_mode"; a key with one has tangents
        // after normalize, Constant also the step ease)
        ClipTangentMode tangentMode = ClipTangentMode::None;

        bool hasTangents() const { return !inTangent.empty() && inTangent.size() == outTangent.size(); }
    };

    struct JASPERENGINE_API ClipTrackData{
        std::string path;                       // "" = the root itself
        ClipChannel channel = ClipChannel::Position;
        // position/rotation/scale: values add to the target's rest (spec 9.R19)
        bool relative = false;
        // the track's own keys repeat within a longer clip (KeyframeTracks semantics)
        bool loop = false;
        // lattice: the flat point indices each key's value lists, in this order
        std::vector<int> points;
        // humanoid / humanoid_position (anim-retarget 7.1): the bone, -1 for other channels.
        // humanoid: 0..21 a slot (pose::HumanBone), 22..54 an extra bone (HUMAN_BONE_COUNT +
        // pose::HumanExtraBone); humanoid_position: HUMANOID_POSITION_HIPS / _LEFT_FOOT / _RIGHT_FOOT
        int humanBone = -1;
        // property tracks (anim-properties spec 6): the addressed component and property (Catalog
        // names, PropertyBindings.h). normalize resolves the rest from them: `kind` (a script
        // field's or shader member's comes from its keys: set by the parser or the editor),
        // `width` (floats per key: 1 for a swizzle), `swizzle` and `discrete`. A property that is
        // not animatable (`known` false) keeps the track as read (compact JSON, `preserved`),
        // written back verbatim and never played.
        std::string component;
        std::string property;
        std::string preserved;
        PropertyKind kind = PropertyKind::None;
        size_t width = 0;
        int swizzle = -1;
        bool discrete = false;
        bool known = false;
        std::vector<ClipKey> keys;

        float lastKeyTime() const { return keys.empty() ? 0.0f : keys.back().time; }
    };

    struct JASPERENGINE_API ClipEventData{
        float time = 0.0f;
        std::string function;                   // a Lua identifier
        std::vector<AnimationEventArg> args;
    };

    // A humanoid clip's source (file key "humanoid", anim-retarget 6.1): informational, the
    // reports show the scale ratio with it
    struct JASPERENGINE_API ClipHumanoidInfo{
        std::string sourceModel;                // asset-relative
        float humanScale = 0.0f;                // the source rig's humanScale (0: unknown)

        bool empty() const { return sourceModel.empty() && humanScale == 0.0f; }
    };

    // Written by Extract / Convert / Bake only; ignored by the runtime
    struct JASPERENGINE_API ClipSource{
        std::string kind;                       // "model" | "entity" | "pose"; empty: no source
        std::string path;
        std::string name;

        bool empty() const { return kind.empty(); }
    };

    struct JASPERENGINE_API ClipData{
        // asset GUID (32 lowercase hex), written right after `version`; empty = none yet
        std::string guid;
        // samples per second (1..240): the snapping grid and frames display only
        float frameRate = 30.0f;
        ClipWrapMode wrap = ClipWrapMode::Once;
        // explicit length in seconds; < 0 = automatic (last key or event time)
        float length = -1.0f;
        // the time an additive layer measures this clip's delta from (anim layers spec 6.1, file
        // key "additive_reference", written only when not 0); normalize clamps it to
        // [0, effectiveLength()]
        float additiveReference = 0.0f;
        std::vector<ClipTrackData> tracks;
        std::vector<ClipEventData> events;
        // audio cues (S40): a clip with any is written as version 3
        std::vector<ClipAudioCue> audio;
        // editor-only hint: a model or prefab to preview the clip on (asset-relative)
        std::string previewModel;
        ClipSource source;
        ClipHumanoidInfo humanoid;
        // unknown top-level keys and their compact JSON text, in file order: written back
        std::vector<std::pair<std::string, std::string>> unknownKeys;

        bool hasExplicitLength() const { return length >= 0.0f; }
        // a clip with any humanoid track is a humanoid clip (written as version 2)
        bool isHumanoid() const;
        // index into tracks of the humanoid track (channel, humanBone), -1 when none
        int findHumanoidTrack(ClipChannel channel, int humanBone) const;
        // the explicit length, else the last key, event or cue time over the whole clip
        float effectiveLength() const;
        // index into tracks, -1 when none (property tracks are not found by this; for a humanoid
        // channel `path` is the bone name: "left_upper_arm", "hips")
        int findTrack(const std::string& path, ClipChannel channel) const;
        // a property track by its identity (path, component, property), -1 when none
        int findPropertyTrack(const std::string& path, const std::string& component, const std::string& property) const;
        // the track with the identity of `like`: (path, channel), and (component, property) for
        // a property track; -1 when none
        int findTrack(const ClipTrackData& like) const;

        // floats per key value: position/scale/humanoid_position 3, rotation/humanoid 4, morph
        // `morphCount`, lattice 3 * `pointCount`, property `propertyWidth` (ClipTrackData::width)
        static size_t valueWidth(ClipChannel channel, size_t morphCount = 0, size_t pointCount = 0, size_t propertyWidth = 0);
    };

    // The immutable runtime clip (built from a ClipData; neither copyable nor movable, so the
    // track arrays below stay valid for the clip's lifetime)
    class JASPERENGINE_API AnimationClip{
    public:
        struct Track{
            std::string path;
            ClipChannel channel = ClipChannel::Position;
            bool relative = false;
            bool loop = false;
            int humanBone = -1;                 // humanoid tracks (ClipTrackData::humanBone)
            float duration = 0.0f;              // its last key's time
            // the last key's ease: no segment starts there, so sampling never reads it, but a file
            // can hold it and toData gives it back
            EaseType trailingEase = EaseType::LINEAR;

            size_t keyCount = 0;
            // keyCount; null without keys. Tracks with identical times (bit for bit) point at one
            // range, so equal pointers mean equal times; different pointers say nothing
            const float* times = nullptr;
            // keyCount: the segment starting at each key; null when every segment is linear
            const EaseType* easings = nullptr;

            // values per key: 1 for position/rotation/scale, the morph target count for morph,
            // the point count for lattice, the key width for property; key k's row starts at
            // k * width
            size_t width = 1;
            const Vector3* vectors = nullptr;       // position, scale, lattice, humanoid_position
            const Quaternion* rotations = nullptr;  // rotation, humanoid
            const float* weights = nullptr;         // morph, property (key space floats)
            // tangents of the same channel, null without (linear / eased track)
            const Vector3* inVectors = nullptr;
            const Vector3* outVectors = nullptr;
            const Quaternion* inRotations = nullptr;
            const Quaternion* outRotations = nullptr;
            const float* inWeights = nullptr;
            const float* outWeights = nullptr;
            const int* points = nullptr;            // lattice: `width` flat indices

            // property tracks (ClipTrackData's fields; anim-properties spec 8): a discrete track
            // steps (its easings are STEP); `overridden`: components of this whole-property track
            // that a swizzle track of the same clip (same path and property) keys instead
            std::string component;
            std::string property;
            std::string preserved;              // the track as read (ClipTrackData::preserved)
            PropertyKind kind = PropertyKind::None;
            int swizzle = -1;
            bool discrete = false;
            bool known = false;                 // false: not animatable, never sampled
            uint8_t overridden = 0;             // bit k: component k

            bool hasTangents() const { return inVectors || inRotations || inWeights; }
            bool isSampled() const { return (channel != ClipChannel::Property || known) && keyCount > 0; }
        };

        AnimationClip(const AnimationClip&) = delete;
        AnimationClip& operator=(const AnimationClip&) = delete;
        ~AnimationClip();

        // The runtime form of `data` (normalized first, problems appended to `warnings`).
        // Never null.
        static std::shared_ptr<const AnimationClip> build(const ClipData& data, std::vector<ClipWarning>* warnings = nullptr);
        // build() of a parsed clip: getWarnings() lists `loadWarnings` (the parser's) first
        static std::shared_ptr<const AnimationClip> build(const ClipData& data, const std::vector<ClipWarning>& loadWarnings,
                                                          std::vector<ClipWarning>* warnings);
        // build() of a parseClip result without normalizing again (it is normalized already);
        // null when the parse failed. getWarnings() = the parser's warnings
        static std::shared_ptr<const AnimationClip> build(const anim::ClipParseResult& parsed);
        // back to the value form (round trip: build(toData()) samples identically)
        ClipData toData() const;

        float getFrameRate() const { return frameRate; }
        ClipWrapMode getWrap() const { return wrap; }
        // the effective length (ClipData::effectiveLength), seconds
        float getLength() const { return length; }
        bool hasExplicitLength() const { return explicitLength; }
        // ClipData::additiveReference (seconds, in [0, getLength()])
        float getAdditiveReference() const { return additiveReference; }

        const std::string& getGuid() const { return guid; }
        const std::string& getPreviewModel() const { return previewModel; }
        const ClipSource& getSource() const { return source; }
        const ClipHumanoidInfo& getHumanoidInfo() const { return humanoid; }
        // any humanoid track (anim-retarget 5.1)
        bool isHumanoid() const { return humanoidTracks > 0; }
        const std::vector<std::pair<std::string, std::string>>& getUnknownKeys() const { return unknownKeys; }
        // the loader's and build's warnings (what the editor and tools report)
        const std::vector<ClipWarning>& getWarnings() const { return warnings; }

        const std::vector<Track>& getTracks() const { return tracks; }
        // some track is a sampled property track
        bool hasPropertyTracks() const { return propertyTracks; }
        // sorted by (time, function)
        const std::vector<ClipEventData>& getEvents() const { return events; }
        // sorted by time (S40)
        const std::vector<ClipAudioCue>& getAudioCues() const { return audio; }
        // the heap this clip owns, from the real capacities of its arrays and strings and with the
        // object itself, measured once at build. The only source of a clip's memory number
        size_t memoryBytes() const { return heapBytes; }

        // index into getTracks(), -1 when none (humanoid channels: `path` is the bone name)
        int findTrack(const std::string& path, ClipChannel channel) const;
        int findHumanoidTrack(ClipChannel channel, int humanBone) const;

    private:
        AnimationClip() = default;

        // `data` must satisfy the invariants above (normalized)
        static std::shared_ptr<const AnimationClip> buildNormalized(const ClipData& data, std::vector<ClipWarning> clipWarnings);
        size_t measureHeap() const;

        float frameRate = 30.0f;
        ClipWrapMode wrap = ClipWrapMode::Once;
        float length = 0.0f;
        bool explicitLength = false;
        float additiveReference = 0.0f;
        std::string guid;
        std::string previewModel;
        ClipSource source;
        ClipHumanoidInfo humanoid;
        size_t humanoidTracks = 0;
        std::vector<std::pair<std::string, std::string>> unknownKeys;
        std::vector<ClipWarning> warnings;
        std::vector<Track> tracks;
        std::vector<ClipEventData> events;
        std::vector<ClipAudioCue> audio;
        bool propertyTracks = false;
        size_t heapBytes = 0;

        // contiguous storage the Track pointers point into
        std::vector<float> timeStorage;
        std::vector<EaseType> easingStorage;
        std::vector<Vector3> vectorStorage;
        std::vector<Quaternion> rotationStorage;
        std::vector<float> weightStorage;
        std::vector<int> pointStorage;
    };

}

#endif //ANIMATION_CLIP_H
