// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R12, R14, R15, R18, R19, R25, §6.6)
// - Purpose: a sampled pose as what the renderer draws. (1) Document values: the field
//   overrides a render at that pose takes (S80 R18a: applied to the snapshot copy, never to the
//   document): layer and document fields, each child of a group, and glyph tracks of `merged`
//   text layers as their characters' adjustments. (2) Per-glyph values (offset, rotation, scale,
//   stretch, opacity, tint, flash) for the glyph-cell compose of `cells` text layers. The editor's
//   exact renders, the flipbook baker and the runtime's Realtime and Glyph Atlas requests all
//   take their values from here, so one pose renders one way.
// - Overlays (R25) combine relative over what the base (and earlier overlays) set; a stepped
//   field takes the later one.
// - Re-render requests (R18) sample at k / rerender_fps of each animation's own time, so a
//   re-render value changes at most rerender_fps times a second and every request time is a
//   multiple of 1 / rerender_fps.
// - Thread-safety: pure functions.

#pragma once

#include "Export.h"
#include "titleanim/TitleAnimPlayer.h"
#include "titleanim/TitleAnimTypes.h"
#include "titleart/TitleArtDoc.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace jasperengine::titleart {

    using TrackFilter = std::function<bool(const titleanim::CompiledTitleTrack& track)>;

    // the values of one pose, by (layer id, long path): what a FieldOverride list is built from
    class JASPERENGINE_API PoseValues {
    public:
        // a sampled animation's Field and Child tracks (and merged glyph tracks) that `include`
        // accepts (null: every one); `overlay` combines every track relative over the values so far
        void add(const TitleArtDoc& doc, const titleanim::CompiledTitleAnimation& animation, const titleanim::TitleAnimPose& pose,
                 bool overlay, const TrackFilter& include = nullptr);
        // blends the values toward `other` by `weight` (a fade, R26): numbers and colours linearly,
        // stepped values switch at once to `other`'s
        void blendToward(const PoseValues& other, double weight);
        std::vector<FieldOverride> overrides() const;
        bool empty() const{ return values.empty(); }
        uint64_t hash() const;

    private:
        std::map<std::pair<std::string, std::string>, Json> values;
        const Json* current(const TitleArtDoc& doc, const std::string& layer, const std::string& path, Json& scratch) const;
    };

    // R15: one glyph's animated values (identity when nothing animates it); its pivot in layer
    // units from the cluster's ink box
    struct JASPERENGINE_API GlyphPose {
        uint32_t cluster = 0;           // the cluster's first code point index (LaidOutCluster::index)
        double offset[2] = {0.0, 0.0};  // % of the layer's size, +y up
        double rotation = 0.0;          // deg clockwise
        double scale[2] = {1.0, 1.0};   // scale x stretch
        double opacity = 1.0;
        double tint[3] = {1.0, 1.0, 1.0};       // sRGB-encoded
        double flash[3] = {1.0, 1.0, 1.0};
        double flashAmount = 0.0;
        titleanim::TrackPivot pivot = titleanim::TrackPivot::Center;
        bool identity() const;
    };
    // R14, R15: what a Realtime title's frame composes while it plays (TitleAnimCompose): each
    // split layer's sampled field values (absolute, long paths) and each cells text layer's glyphs
    struct JASPERENGINE_API TitleComposePose {
        std::map<std::string, std::map<std::string, Json>> layers;
        std::map<std::string, std::map<uint32_t, GlyphPose>> glyphs;
        bool empty() const { return layers.empty() && glyphs.empty(); }
    };

    // what the job's cached compose depends on (equal poses: equal hashes)
    JASPERENGINE_API uint64_t glyphPoseHash(const GlyphPose& pose);
    // the glyph poses of text layer `layerId` from a sampled animation, combined into `poses`
    // (one per cluster with ink, created on first use); `overlay` as PoseValues::add
    JASPERENGINE_API void addGlyphPoses(const titleanim::CompiledTitleAnimation& animation, const titleanim::TitleAnimPose& pose,
                                        const std::string& layerId, bool overlay, std::map<uint32_t, GlyphPose>& poses);
    // the text layers whose glyph tracks play as cells (glyph_mode cells) in an animation
    JASPERENGINE_API std::vector<std::string> cellLayersOf(const TitleArtDoc& doc, const titleanim::CompiledTitleAnimation& animation);

    // ---- the runtime's view of a player ----

    struct JASPERENGINE_API PlayerPose {
        PoseValues values;              // re-render values (field overrides)
        std::map<std::string, std::map<uint32_t, GlyphPose>> glyphs;  // by cells text layer
        double sampleTime = 0.0;        // the base's quantized local time (tests: request times)
        bool any = false;
    };
    // what the player's animations ask of a render now: every track that is neither title live
    // nor drawn some other way (`include`), sampled at each animation's rerender_fps when
    // `quantize` (Realtime's rate limit, R18), else at its own time (an exact render)
    JASPERENGINE_API void playerPose(const TitleArtDoc& doc, const titleanim::TitleAnimPlayer& player, bool quantize,
                                     const TrackFilter& include, PlayerPose& out);

}
