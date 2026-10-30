// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (animation clips spec 2.1-2.5, 3.1, 9.R8/R20/R35-R38)
// - Purpose: the two forms of a keyframe clip (.anim file or model sub-clip):
//   * ClipData: the editable value form. Parsed and written by AnimationClipIO, built by the
//     model sub-clip builder, edited by AnimationClipDocument, the Animation window, MCP and the
//     converters (value semantics: copy, change, commit).
//   * AnimationClip: the immutable runtime form built once from a ClipData (AnimationClip::build),
//     shared through AnimationClipPool by every player and preview: per-track typed arrays in
//     contiguous storage that KeyframeSampling reads with key cursors held by the caller.
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
//     the ease (which the loader then resets to linear, with a warning);
//   * wrap is never ClipWrapMode::Default (that value only appears in player clip entries);
//   * events sorted by (time, function); at most MAX_ANIMATION_EVENT_ARGS args each.
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
    enum class ClipTangentMode : uint8_t { None, ClampedAuto, Auto, Free, Linear, Constant, Flat, Broken };

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
        // property (reserved): the addressed component/property and the track as read (compact
        // JSON text), written back verbatim
        std::string component;
        std::string property;
        std::string preserved;
        std::vector<ClipKey> keys;

        float lastKeyTime() const { return keys.empty() ? 0.0f : keys.back().time; }
    };

    struct JASPERENGINE_API ClipEventData{
        float time = 0.0f;
        std::string function;                   // a Lua identifier
        std::vector<AnimationEventArg> args;
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
        std::vector<ClipTrackData> tracks;
        std::vector<ClipEventData> events;
        // editor-only hint: a model or prefab to preview the clip on (asset-relative)
        std::string previewModel;
        ClipSource source;
        // unknown top-level keys and their compact JSON text, in file order: written back
        std::vector<std::pair<std::string, std::string>> unknownKeys;

        bool hasExplicitLength() const { return length >= 0.0f; }
        // the explicit length, else the last key or event time over the whole clip
        float effectiveLength() const;
        // index into tracks, -1 when none (property tracks are not found by this)
        int findTrack(const std::string& path, ClipChannel channel) const;

        // floats per key value: position/scale 3, rotation 4, morph `morphCount`, lattice
        // 3 * `pointCount`, property 0
        static size_t valueWidth(ClipChannel channel, size_t morphCount = 0, size_t pointCount = 0);
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
            float duration = 0.0f;              // its last key's time

            size_t keyCount = 0;
            const float* times = nullptr;       // keyCount
            const EaseType* easings = nullptr;  // keyCount: the segment starting at each key

            // values per key: 1 for position/rotation/scale, the morph target count for morph,
            // the point count for lattice; key k's row starts at k * width
            size_t width = 1;
            const Vector3* vectors = nullptr;       // position, scale, lattice
            const Quaternion* rotations = nullptr;  // rotation
            const float* weights = nullptr;         // morph
            // tangents of the same channel, null without (linear / eased track)
            const Vector3* inVectors = nullptr;
            const Vector3* outVectors = nullptr;
            const Quaternion* inRotations = nullptr;
            const Quaternion* outRotations = nullptr;
            const float* inWeights = nullptr;
            const float* outWeights = nullptr;
            const int* points = nullptr;            // lattice: `width` flat indices

            // property tracks (never sampled)
            std::string component;
            std::string property;
            std::string preserved;              // the track as read (ClipTrackData::preserved)

            bool hasTangents() const { return inVectors || inRotations || inWeights; }
            bool isSampled() const { return channel != ClipChannel::Property && keyCount > 0; }
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

        const std::string& getGuid() const { return guid; }
        const std::string& getPreviewModel() const { return previewModel; }
        const ClipSource& getSource() const { return source; }
        const std::vector<std::pair<std::string, std::string>>& getUnknownKeys() const { return unknownKeys; }
        // the loader's and build's warnings (what the editor and tools report)
        const std::vector<ClipWarning>& getWarnings() const { return warnings; }

        const std::vector<Track>& getTracks() const { return tracks; }
        // sorted by (time, function)
        const std::vector<ClipEventData>& getEvents() const { return events; }

        // index into getTracks(), -1 when none
        int findTrack(const std::string& path, ClipChannel channel) const;

    private:
        AnimationClip() = default;

        // `data` must satisfy the invariants above (normalized)
        static std::shared_ptr<const AnimationClip> buildNormalized(const ClipData& data, std::vector<ClipWarning> clipWarnings);

        float frameRate = 30.0f;
        ClipWrapMode wrap = ClipWrapMode::Once;
        float length = 0.0f;
        bool explicitLength = false;
        std::string guid;
        std::string previewModel;
        ClipSource source;
        std::vector<std::pair<std::string, std::string>> unknownKeys;
        std::vector<ClipWarning> warnings;
        std::vector<Track> tracks;
        std::vector<ClipEventData> events;

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
