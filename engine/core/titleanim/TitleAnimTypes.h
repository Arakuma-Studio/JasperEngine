// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Contract (docs/specs/S85-title-animation.md R1-R10, §6.1, §6.6)
// - Purpose: the time model of a Title Art document: named animations with tracks (keys, drives,
//   reseeds), stagger and markers, in two forms. The authoring form (TitleAnimation) is what the
//   `.titleart`'s `animation` member holds (TitleAnimIO, module) and what the editor's ops edit.
//   The compiled form (CompiledTitleAnimation) is what plays: key arrays laid out for
//   KeyframeSampling, every value resolved to numbers, stagger delays per unit, markers sorted.
//   The module compiles it from the document (TitleAnimPlan.h); a Baked player compiles its
//   title tracks from `<file>.anim.json` (TitleAnimDataIO.h) with no module.
// - Always built (S80 §6.15, S85 Deviation 1): nothing here includes engine/core/titleart.
// - Values: numbers, Vec2s and colours interpolate (colours in sRGB-encoded channels, R3); every
//   other value is stepped: its keys hold an index into CompiledTitleTrack::stepValues (canonical
//   JSON text the module decodes) and a pose holds the index of the key that shows.
// - Thread-safety: plain values.

#ifndef TITLE_ANIM_TYPES_H
#define TITLE_ANIM_TYPES_H

#include "Export.h"
#include "action/Ease.h"
#include "animation/AnimationClip.h"
#include "animation/AnimationEvent.h"
#include "asset/AssetRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jasperengine::titleanim{

    // §6.1 limits
    constexpr size_t TITLE_ANIM_MAX_ANIMATIONS = 32;
    constexpr size_t TITLE_ANIM_MAX_TRACKS = 512;
    constexpr size_t TITLE_ANIM_MAX_KEYS = 4096;
    constexpr size_t TITLE_ANIM_MAX_MARKERS = 64;
    constexpr size_t TITLE_ANIM_MAX_NAME = 32;
    constexpr double TITLE_ANIM_MIN_DURATION = 0.01;
    constexpr double TITLE_ANIM_MAX_DURATION = 60.0;
    constexpr double TITLE_ANIM_MAX_STAGGER_EACH = 2.0;
    constexpr double TITLE_ANIM_MAX_STAGGER_OVER = 30.0;
    constexpr int TITLE_ANIM_DEFAULT_FPS = 30;
    constexpr int TITLE_ANIM_MAX_FLIPBOOK_FPS = 60;
    constexpr size_t TITLE_ANIM_MAX_FLIPBOOK_FRAMES = 240;
    // `duration` -1: the last key time with its stagger delays, and the last marker
    constexpr double TITLE_ANIM_AUTO_DURATION = -1.0;

    enum class AnimWrap : uint8_t { Once, Loop, PingPong };
    enum class AnimRole : uint8_t { Custom, Intro, Idle, Emphasis, Win, Outro };
    // R11: the runtime class of a track (what it costs and how each Render Mode plays it)
    enum class TrackClass : uint8_t { None, TitleLive, LayerLive, Glyph, GlyphAtlasLive, Rerender };
    enum class StaggerOrder : uint8_t { Forward, Reverse, CenterOut, EdgesIn, Random };
    enum class StaggerUnit : uint8_t { Glyph, Word, Line, Child };
    enum class TrackPivot : uint8_t { Origin, Center, Bottom, Top, Left, Right, Baseline };
    enum class RerenderPolicy : uint8_t { Live, Prerender };

    JASPERENGINE_API const char* animWrapName(AnimWrap wrap);              // "once", "loop", "ping_pong"
    JASPERENGINE_API bool animWrapFromName(std::string_view name, AnimWrap& out);
    JASPERENGINE_API const char* animRoleName(AnimRole role);              // "custom", "intro", ...
    JASPERENGINE_API bool animRoleFromName(std::string_view name, AnimRole& out);
    JASPERENGINE_API const char* trackClassName(TrackClass cls);           // "none", "title_live", "layer_live", "glyph", "glyph_atlas_live", "rerender"
    JASPERENGINE_API const char* staggerOrderName(StaggerOrder order);     // "forward", "reverse", "center_out", "edges_in", "random"
    JASPERENGINE_API bool staggerOrderFromName(std::string_view name, StaggerOrder& out);
    JASPERENGINE_API const char* staggerUnitName(StaggerUnit unit);        // "glyph", "word", "line", "child"
    JASPERENGINE_API bool staggerUnitFromName(std::string_view name, StaggerUnit& out);
    JASPERENGINE_API const char* trackPivotName(TrackPivot pivot);         // "origin", "center", ... "baseline"
    JASPERENGINE_API bool trackPivotFromName(std::string_view name, TrackPivot& out);
    JASPERENGINE_API const char* rerenderPolicyName(RerenderPolicy policy);    // "live", "prerender"
    JASPERENGINE_API bool rerenderPolicyFromName(std::string_view name, RerenderPolicy& out);

    // ---- the authoring form ----

    struct JASPERENGINE_API TitleStagger{
        float each = 0.05f;             // seconds between units (ignored when over >= 0)
        float over = -1.0f;             // seconds from the first unit's start to the last's; < 0: each
        StaggerOrder order = StaggerOrder::Forward;
        StaggerUnit unit = StaggerUnit::Glyph;
        uint32_t seed = 1;
        EaseType ease = EaseType::LINEAR;
    };

    struct JASPERENGINE_API TitleDrive{
        std::vector<float> from, to;    // the value's width (a colour: sRGB channels)
        float cycles = 0.0f;            // > 0: that many runs over the loop span (seamless)
        float rate = 0.0f;              // units per second when cycles is 0
        bool wrap = false;              // rate: wrap into [from, to)
        EaseType ease = EaseType::LINEAR;
    };

    struct JASPERENGINE_API TitleReseed{
        float rate = 12.0f;             // new seeds per second
        float offset = 0.0f;
    };

    // a key value: numbers (width 1..3) or a stepped value (canonical JSON text)
    struct JASPERENGINE_API TitleKey{
        ClipKey key;                    // time, value[] (numeric tracks), ease, tangents, tangent mode
        std::string stepped;            // stepped tracks: the value as canonical JSON text; numeric: ""
    };

    struct JASPERENGINE_API TitleTrack{
        std::string layer;              // a layer id, "" for the title (R5)
        std::string field;              // S80 field path, long form (or S85's virtual fields)
        std::string fromPreset;         // the preset that wrote it ("" none)
        bool relative = false;
        bool mute = false;
        TrackPivot pivot = TrackPivot::Origin;
        std::vector<TitleKey> keys;
        std::optional<TitleStagger> stagger;
        std::optional<TitleDrive> drive;
        std::optional<TitleReseed> reseed;
        bool orphan = false;            // its layer or field is gone (not played, kept: R59)
        std::string unknown;            // members this build does not know (a JSON object's text, "" none)
    };

    struct JASPERENGINE_API TitleMarker{
        float time = 0.0f;
        std::string name;               // a Lua identifier
        std::vector<AnimationEventArg> args;    // at most MAX_ANIMATION_EVENT_ARGS
        AssetRef sound;                 // an audio clip or a .soundevent (S37), empty none
        float volumeDb = 0.0f;          // -60..12
        std::string group;              // a mixer group, "" the source's (else Master)
        std::string unknown;
    };

    struct JASPERENGINE_API FlipbookSettings{
        float fps = 30.0f;              // frames per second, 1..60
        float scale = 1.0f;             // of the main output's scale, 0.25..1
        int maxPage = 2048;             // 1024, 2048 or 4096
        bool frameBlend = false;
    };

    struct JASPERENGINE_API TitleAnimation{
        std::string id;                 // 8 hex
        std::string name;
        AnimRole role = AnimRole::Custom;
        float duration = (float)TITLE_ANIM_AUTO_DURATION;
        AnimWrap wrap = AnimWrap::Once;
        float loopStart = 0.0f;
        std::string next;
        bool overlay = false;
        float speed = 1.0f;
        RerenderPolicy rerender = RerenderPolicy::Live;
        float rerenderFps = 30.0f;
        FlipbookSettings flipbook;
        // R22: the preset this animation came from, with the parameters it was applied with
        std::string presetId;
        std::string presetParams;       // canonical JSON object text ("" none)
        bool hasPresetSource = false;
        std::vector<TitleTrack> tracks;
        std::vector<TitleMarker> markers;
        std::string unknown;
    };

    // the document member (§6.1)
    struct JASPERENGINE_API TitleAnimationSet{
        int fps = TITLE_ANIM_DEFAULT_FPS;
        std::string defaultName;
        std::vector<TitleAnimation> animations;
        std::string unknown;

        const TitleAnimation* find(std::string_view name) const;
        TitleAnimation* find(std::string_view name);
    };

    // ---- the compiled form ----

    enum class TrackValue : uint8_t { Number, Vec2, Color, Stepped };
    enum class TrackCombine : uint8_t { Replace, Add, Multiply };
    enum class TrackSource : uint8_t { Keys, Drive, Reseed };

    // what a track writes (TitleField/GlyphField for S85's virtual fields, else a layer or
    // document field the module applies by path)
    enum class TrackTarget : uint8_t { Field, Title, Glyph, Child };

    // R5's title-level virtual fields (the companion's draw and S81's Live Effects)
    enum class TitleField : uint8_t {
        None, Position, Rotation, Scale, Stretch, Pivot, Opacity, FlashColor, FlashAmount,
        SweepPosition, SweepAngle, SweepWidth, GlowPulse, TintColor, TintAmount, GlintAmount,
        Count
    };
    // R9's per-glyph virtual fields (also `child.*` of a group's layer-live members)
    enum class GlyphField : uint8_t { None, Offset, Rotation, Scale, Stretch, Opacity, Tint, FlashColor, FlashAmount, Count };

    // "transform.position" -> Position ...; None when not a title field
    JASPERENGINE_API TitleField titleFieldFromPath(std::string_view path);
    JASPERENGINE_API const char* titleFieldPath(TitleField field);
    // "glyph.offset" -> Offset (the path without the "glyph." prefix too)
    JASPERENGINE_API GlyphField glyphFieldFromPath(std::string_view path);
    JASPERENGINE_API const char* glyphFieldPath(GlyphField field);
    // the value type, width, identity rest, range and combination of a virtual field
    struct JASPERENGINE_API VirtualFieldInfo{
        TrackValue value = TrackValue::Number;
        uint8_t width = 1;
        float rest[3] = {0.0f, 0.0f, 0.0f};
        double min = -1e9, max = 1e9;
        TrackCombine relativeCombine = TrackCombine::Add;
        const char* label = "";
    };
    JASPERENGINE_API VirtualFieldInfo titleFieldInfo(TitleField field);
    JASPERENGINE_API VirtualFieldInfo glyphFieldInfo(GlyphField field);

    struct JASPERENGINE_API CompiledTitleTrack{
        std::string layer, field;       // as authored (long form)
        TrackTarget target = TrackTarget::Field;
        TitleField titleField = TitleField::None;
        GlyphField glyphField = GlyphField::None;
        // a component track ("tint.r"): the component written of a wider value, -1 the whole
        int component = -1;
        TrackValue value = TrackValue::Number;
        uint8_t width = 1;              // values per sample (1..3; stepped 1)
        TrackSource source = TrackSource::Keys;
        TrackCombine combine = TrackCombine::Replace;   // relative tracks: Add or Multiply
        // how the field combines when relative (an overlay combines every track this way, R25)
        TrackCombine relativeCombine = TrackCombine::Add;
        bool integer = false;           // a whole-number field (seeds, counts): written rounded
        TrackClass cls = TrackClass::Rerender;
        TrackPivot pivot = TrackPivot::Origin;
        bool mute = false;
        bool orphan = false;
        double min = -1e18, max = 1e18; // the field's range (samples clamp to it)
        std::vector<float> rest;        // the rest value (width): what a relative track combines with

        // keys (KeyframeSampling's layout: times, an ease per segment start, `width` values per
        // key, tangents empty or `width` per key)
        std::vector<float> times;
        std::vector<EaseType> easings;
        std::vector<float> values;
        std::vector<float> inTangents, outTangents;
        std::vector<std::string> stepValues;    // stepped tracks: canonical JSON of each key's value

        // drive and reseed
        std::vector<float> driveFrom, driveTo;
        float cycles = 0.0f, rate = 0.0f;
        bool driveWrap = false;
        EaseType driveEase = EaseType::LINEAR;
        float reseedRate = 0.0f, reseedOffset = 0.0f;

        // stagger: one delay per unit (empty: one value for the whole target); per layout
        // cluster (glyph tracks) or child (child tracks) the unit it belongs to, -1 none
        std::vector<float> unitDelays;
        std::vector<int32_t> elementUnit;
        // glyph tracks: each element's grapheme cluster (its first code point's index in the text,
        // the `chars` key); child tracks: the child's index
        std::vector<uint32_t> elementIndex;
        // a component track ("tint.r"): the whole value's rest (the other components it keeps)
        std::vector<float> wholeRest;

        // where the samples go in a pose: `units` x `width` doubles from `poseOffset`
        uint32_t poseOffset = 0;
        uint32_t units = 1;

        bool hasTangents() const{ return !inTangents.empty(); }
        size_t keyCount() const{ return times.size(); }
        // the last key time plus the largest stagger delay (R1's automatic duration)
        double endTime() const;
    };

    struct JASPERENGINE_API CompiledMarker{
        float time = 0.0f;
        std::string name;
        std::vector<AnimationEventArg> args;
        AssetRef sound;
        float volumeDb = 0.0f;
        std::string group;
    };

    // a Baked flipbook (R16, R60): the frames' rects on pages; every frame has `frameWidth` x
    // `frameHeight` pixels and covers `frame` (canvas pixels, image space)
    struct JASPERENGINE_API FlipbookPage{
        std::string file;               // beside the document
        std::string guid;
        int width = 0, height = 0;
    };
    struct JASPERENGINE_API FlipbookFrame{
        int page = 0, x = 0, y = 0;
    };
    struct JASPERENGINE_API CompiledFlipbook{
        float fps = 30.0f;
        int frameX = 0, frameY = 0, frameWidth = 0, frameHeight = 0;     // canvas px of the frame (image space)
        int pixelWidth = 0, pixelHeight = 0;    // a frame's size in page pixels
        bool blend = false;
        std::vector<FlipbookPage> pages;
        std::vector<FlipbookFrame> frames;
        bool empty() const{ return frames.empty() || pages.empty(); }
    };

    // R20's Baked figures (`baked_cost`), stored with the flipbook
    struct JASPERENGINE_API BakedCost{
        int frames = 0, unique = 0, pages = 0;
        uint64_t bytesDesktop = 0, bytesMobile = 0, bytesAndroidEtc2 = 0, bytesWeb = 0;
    };

    struct JASPERENGINE_API CompiledTitleAnimation{
        std::string id, name, next;
        AnimRole role = AnimRole::Custom;
        double duration = 1.0;          // resolved (automatic made explicit), > 0
        bool autoDuration = false;
        AnimWrap wrap = AnimWrap::Once;
        double loopStart = 0.0;
        float speed = 1.0f;
        bool overlay = false;
        RerenderPolicy rerender = RerenderPolicy::Live;
        float rerenderFps = 30.0f;
        FlipbookSettings flipbookSettings;
        std::vector<CompiledTitleTrack> tracks;
        std::vector<CompiledMarker> markers;    // sorted by (time, name)
        uint32_t poseSize = 0;          // doubles a pose of this animation holds
        // Baked: its flipbook (empty when every track is title live, or not baked)
        CompiledFlipbook flipbook;
        BakedCost bakedCost;

        // the loop span's start (loop and ping-pong: loopStart; once: 0)
        double spanStart() const{ return wrap == AnimWrap::Once ? 0.0 : loopStart; }
        bool hasTitleTracks() const;
        // lays out poseOffset/units of every track and sets poseSize
        void layoutPose();
    };

    // the sampled values of one animation at one time (allocation-free once sized)
    struct JASPERENGINE_API TitleAnimPose{
        double time = 0.0;              // the animation's local (wrapped) time
        std::vector<double> values;     // CompiledTitleAnimation::poseSize
        void size(const CompiledTitleAnimation& animation){ values.assign(animation.poseSize, 0.0); }
    };

    // the whole title's draw values (R13): what the companion's ANIM feature and S81's Live
    // Effects show; `has*` says which a playing track wrote (the rest keep the instance's)
    struct JASPERENGINE_API TitlePoseSummary{
        double position[2] = {0.0, 0.0};    // px, +y up
        double rotation = 0.0;              // deg clockwise
        double scale = 1.0;
        double stretch[2] = {1.0, 1.0};
        double pivot[2] = {0.0, 0.0};       // px from the frame centre, +y up
        double opacity = 1.0;
        double flashColor[3] = {1.0, 1.0, 1.0}; // sRGB-encoded
        double flashAmount = 0.0;
        bool transformed = false;           // a transform, opacity or flash track played
        // S81's Live Effects (R5): values and whether a track wrote them
        double live[(size_t)TitleField::Count] = {};
        double tintColor[3] = {1.0, 1.0, 1.0};
        bool liveSet[(size_t)TitleField::Count] = {};

        bool any() const;
    };

}

#endif //TITLE_ANIM_TYPES_H
