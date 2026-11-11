// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimTypes.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::titleanim;

namespace {

    template<typename E, size_t N>
    bool fromNames(std::string_view name, const char* const (&names)[N], E& out){
        for (size_t i = 0; i < N; i++){
            if (name == names[i]){
                out = (E)i;
                return true;
            }
        }
        return false;
    }

    const char* const WRAP_NAMES[] = {"once", "loop", "ping_pong"};
    const char* const ROLE_NAMES[] = {"custom", "intro", "idle", "emphasis", "win", "outro"};
    const char* const CLASS_NAMES[] = {"none", "title_live", "layer_live", "glyph", "glyph_atlas_live", "rerender"};
    const char* const ORDER_NAMES[] = {"forward", "reverse", "center_out", "edges_in", "random"};
    const char* const UNIT_NAMES[] = {"glyph", "word", "line", "child"};
    const char* const PIVOT_NAMES[] = {"origin", "center", "bottom", "top", "left", "right", "baseline"};
    const char* const POLICY_NAMES[] = {"live", "prerender"};

    const char* const TITLE_FIELD_PATHS[] = {
        "", "transform.position", "transform.rotation", "transform.scale", "transform.stretch", "transform.pivot", "opacity",
        "flash.color", "flash.amount", "live.sweep_position", "live.sweep_angle", "live.sweep_width", "live.glow_pulse",
        "live.tint_color", "live.tint_amount", "live.glint_amount"};
    const char* const GLYPH_FIELD_PATHS[] = {
        "", "glyph.offset", "glyph.rotation", "glyph.scale", "glyph.stretch", "glyph.opacity", "glyph.tint", "glyph.flash.color",
        "glyph.flash.amount"};

    VirtualFieldInfo info(TrackValue value, uint8_t width, std::initializer_list<float> rest, double min, double max, TrackCombine combine,
                          const char* label){
        VirtualFieldInfo out;
        out.value = value;
        out.width = width;
        size_t i = 0;
        for (float r : rest) if (i < 3) out.rest[i++] = r;
        out.min = min;
        out.max = max;
        out.relativeCombine = combine;
        out.label = label;
        return out;
    }

}

const char* titleanim::animWrapName(AnimWrap wrap){ return WRAP_NAMES[(size_t)wrap]; }
bool titleanim::animWrapFromName(std::string_view name, AnimWrap& out){ return fromNames(name, WRAP_NAMES, out); }
const char* titleanim::animRoleName(AnimRole role){ return ROLE_NAMES[(size_t)role]; }
bool titleanim::animRoleFromName(std::string_view name, AnimRole& out){ return fromNames(name, ROLE_NAMES, out); }
const char* titleanim::trackClassName(TrackClass cls){ return CLASS_NAMES[(size_t)cls]; }
const char* titleanim::staggerOrderName(StaggerOrder order){ return ORDER_NAMES[(size_t)order]; }
bool titleanim::staggerOrderFromName(std::string_view name, StaggerOrder& out){ return fromNames(name, ORDER_NAMES, out); }
const char* titleanim::staggerUnitName(StaggerUnit unit){ return UNIT_NAMES[(size_t)unit]; }
bool titleanim::staggerUnitFromName(std::string_view name, StaggerUnit& out){ return fromNames(name, UNIT_NAMES, out); }
const char* titleanim::trackPivotName(TrackPivot pivot){ return PIVOT_NAMES[(size_t)pivot]; }
bool titleanim::trackPivotFromName(std::string_view name, TrackPivot& out){ return fromNames(name, PIVOT_NAMES, out); }
const char* titleanim::rerenderPolicyName(RerenderPolicy policy){ return POLICY_NAMES[(size_t)policy]; }
bool titleanim::rerenderPolicyFromName(std::string_view name, RerenderPolicy& out){ return fromNames(name, POLICY_NAMES, out); }

TitleField titleanim::titleFieldFromPath(std::string_view path){
    for (size_t i = 1; i < (size_t)TitleField::Count; i++)
        if (path == TITLE_FIELD_PATHS[i]) return (TitleField)i;
    return TitleField::None;
}

const char* titleanim::titleFieldPath(TitleField field){
    return field < TitleField::Count ? TITLE_FIELD_PATHS[(size_t)field] : "";
}

GlyphField titleanim::glyphFieldFromPath(std::string_view path){
    for (size_t i = 1; i < (size_t)GlyphField::Count; i++){
        std::string_view full = GLYPH_FIELD_PATHS[i];
        if (path == full || path == full.substr(6)) return (GlyphField)i;
    }
    return GlyphField::None;
}

const char* titleanim::glyphFieldPath(GlyphField field){
    return field < GlyphField::Count ? GLYPH_FIELD_PATHS[(size_t)field] : "";
}

VirtualFieldInfo titleanim::titleFieldInfo(TitleField field){
    switch (field){
        case TitleField::Position: return info(TrackValue::Vec2, 2, {0, 0}, -1e6, 1e6, TrackCombine::Add, "Position");
        case TitleField::Rotation: return info(TrackValue::Number, 1, {0}, -36000, 36000, TrackCombine::Add, "Rotation");
        case TitleField::Scale: return info(TrackValue::Number, 1, {1}, 0, 100, TrackCombine::Multiply, "Scale");
        case TitleField::Stretch: return info(TrackValue::Vec2, 2, {1, 1}, -100, 100, TrackCombine::Multiply, "Stretch");
        case TitleField::Pivot: return info(TrackValue::Vec2, 2, {0, 0}, -1e6, 1e6, TrackCombine::Add, "Pivot");
        case TitleField::Opacity: return info(TrackValue::Number, 1, {1}, 0, 1, TrackCombine::Multiply, "Opacity");
        case TitleField::FlashColor: return info(TrackValue::Color, 3, {1, 1, 1}, 0, 1, TrackCombine::Multiply, "Flash Color");
        case TitleField::FlashAmount: return info(TrackValue::Number, 1, {0}, -4, 4, TrackCombine::Add, "Flash Amount");
        case TitleField::SweepPosition: return info(TrackValue::Number, 1, {0}, -1, 2, TrackCombine::Add, "Sweep Position");
        case TitleField::SweepAngle: return info(TrackValue::Number, 1, {25}, -360, 360, TrackCombine::Add, "Sweep Angle");
        case TitleField::SweepWidth: return info(TrackValue::Number, 1, {0.15f}, 0, 1, TrackCombine::Multiply, "Sweep Width");
        case TitleField::GlowPulse: return info(TrackValue::Number, 1, {0}, 0, 10, TrackCombine::Add, "Glow Pulse");
        case TitleField::TintColor: return info(TrackValue::Color, 3, {1, 1, 1}, 0, 1, TrackCombine::Multiply, "Tint Color");
        case TitleField::TintAmount: return info(TrackValue::Number, 1, {0}, 0, 1, TrackCombine::Add, "Tint Amount");
        case TitleField::GlintAmount: return info(TrackValue::Number, 1, {1}, 0, 10, TrackCombine::Multiply, "Glint Amount");
        default: break;
    }
    return VirtualFieldInfo();
}

VirtualFieldInfo titleanim::glyphFieldInfo(GlyphField field){
    switch (field){
        case GlyphField::Offset: return info(TrackValue::Vec2, 2, {0, 0}, -10000, 10000, TrackCombine::Add, "Glyph Offset");
        case GlyphField::Rotation: return info(TrackValue::Number, 1, {0}, -36000, 36000, TrackCombine::Add, "Glyph Rotation");
        case GlyphField::Scale: return info(TrackValue::Number, 1, {1}, 0, 100, TrackCombine::Multiply, "Glyph Scale");
        case GlyphField::Stretch: return info(TrackValue::Vec2, 2, {1, 1}, -100, 100, TrackCombine::Multiply, "Glyph Stretch");
        case GlyphField::Opacity: return info(TrackValue::Number, 1, {1}, 0, 1, TrackCombine::Multiply, "Glyph Opacity");
        case GlyphField::Tint: return info(TrackValue::Color, 3, {1, 1, 1}, 0, 1, TrackCombine::Multiply, "Glyph Tint");
        case GlyphField::FlashColor: return info(TrackValue::Color, 3, {1, 1, 1}, 0, 1, TrackCombine::Multiply, "Glyph Flash Color");
        case GlyphField::FlashAmount: return info(TrackValue::Number, 1, {0}, -4, 4, TrackCombine::Add, "Glyph Flash Amount");
        default: break;
    }
    return VirtualFieldInfo();
}

const TitleAnimation* TitleAnimationSet::find(std::string_view name) const{
    for (const TitleAnimation& a : animations) if (a.name == name) return &a;
    return nullptr;
}

TitleAnimation* TitleAnimationSet::find(std::string_view name){
    for (TitleAnimation& a : animations) if (a.name == name) return &a;
    return nullptr;
}

double CompiledTitleTrack::endTime() const{
    double end = 0.0;
    if (source == TrackSource::Keys && !times.empty()) end = times.back();
    double delay = 0.0;
    for (float d : unitDelays) delay = std::max(delay, (double)d);
    return end + delay;
}

bool CompiledTitleAnimation::hasTitleTracks() const{
    for (const CompiledTitleTrack& t : tracks)
        if (t.target == TrackTarget::Title && !t.mute && !t.orphan) return true;
    return false;
}

void CompiledTitleAnimation::layoutPose(){
    uint32_t offset = 0;
    for (CompiledTitleTrack& t : tracks){
        t.units = t.unitDelays.empty() ? 1u : (uint32_t)t.unitDelays.size();
        t.poseOffset = offset;
        offset += t.units * std::max<uint32_t>(1u, t.width);
    }
    poseSize = offset;
}

bool TitlePoseSummary::any() const{
    if (transformed) return true;
    for (bool set : liveSet) if (set) return true;
    return false;
}
