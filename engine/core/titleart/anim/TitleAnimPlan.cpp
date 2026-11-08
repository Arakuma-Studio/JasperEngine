// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimPlan.h"

#include "titleanim/TitleAnimDataIO.h"
#include "titleanim/TitleAnimSampling.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/runtime/TitleGlyphAtlasPlan.h"
#include "titleart/text/TitleTextLayout.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>

using namespace jasperengine;
using namespace jasperengine::titleart;
namespace ta = jasperengine::titleanim;

namespace {

    bool isSpace(uint32_t c){
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x00A0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200B) ||
               c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
    }

    std::vector<uint32_t> codepointsOf(const std::string& text){
        std::vector<uint32_t> out;
        for (size_t i = 0; i < text.size();){
            const unsigned char c = (unsigned char)text[i];
            uint32_t cp = c;
            size_t n = 1;
            if (c >= 0xF0 && i + 3 < text.size() + 0){ cp = c & 0x07; n = 4; }
            else if (c >= 0xE0){ cp = c & 0x0F; n = 3; }
            else if (c >= 0xC0){ cp = c & 0x1F; n = 2; }
            for (size_t k = 1; k < n && i + k < text.size(); k++) cp = (cp << 6) | ((unsigned char)text[i + k] & 0x3F);
            out.push_back(cp);
            i += n;
        }
        return out;
    }

    TitleProblem problem(TitleSeverity severity, const std::string& code, const std::string& layer, const std::string& path,
                         const std::string& message){
        TitleProblem p;
        p.severity = severity;
        p.code = code;
        p.layer = layer;
        p.path = path;
        p.message = message;
        return p;
    }

    // a key's value as `width` floats (a number for a pair: both components)
    bool keyFloats(const ta::TitleKey& key, size_t width, std::vector<float>& out){
        out = key.key.value;
        if (out.size() == 1 && width > 1) out.assign(width, out[0]);
        return out.size() == width;
    }

    // the document value as floats (Vec2, colour, number)
    std::vector<float> floatsOf(const Json& value, ta::TrackValue type, int width){
        std::vector<float> out((size_t)width, 0.0f);
        if (type == ta::TrackValue::Color){
            TitleColor c;
            if (parseTitleColor(value, c)){
                out = {c.r, c.g, c.b};
            }
            return out;
        }
        if (value.is_number()){
            out.assign((size_t)width, (float)value.get<double>());
        }else if (value.is_array()){
            for (size_t i = 0; i < value.size() && i < (size_t)width; i++) if (value[i].is_number()) out[i] = (float)value[i].get<double>();
        }else if (value.is_boolean()){
            out[0] = value.get<bool>() ? 1.0f : 0.0f;
        }
        return out;
    }

    bool multipliesWhenRelative(const TrackFieldInfo& info, const std::string& field){
        if (info.combine == ParamCombine::Multiply) return true;
        const std::string leaf = field.substr(field.find_last_of('.') == std::string::npos ? 0 : field.find_last_of('.') + 1);
        return leaf == "scale" || leaf == "stretch" || leaf == "opacity" || leaf == "tint";
    }

    ta::TrackClass classOf(const TitleArtDoc& doc, const ta::TitleTrack& track, const TrackFieldInfo& info, std::string* reason){
        switch (info.target){
            case ta::TrackTarget::Title: return ta::TrackClass::TitleLive;
            case ta::TrackTarget::Glyph: return ta::TrackClass::Glyph;
            case ta::TrackTarget::Child: return info.anim == FxAnim::Live ? ta::TrackClass::Glyph : ta::TrackClass::Rerender;
            case ta::TrackTarget::Field: break;
        }
        if (info.anim == FxAnim::None) return ta::TrackClass::None;
        if (info.layer && info.longPath == "text") return ta::TrackClass::GlyphAtlasLive;
        if (info.anim != FxAnim::Live) return ta::TrackClass::Rerender;
        if (!info.layer) return ta::TrackClass::Rerender;
        // an S83 L param: live only through its effect's live variant (D10)
        if (info.longPath.find(".params.") != std::string::npos){
            const std::string blocker = liveParamBlocker(*info.layer, info.longPath);
            if (!blocker.empty()){
                if (reason) *reason = blocker;
                return ta::TrackClass::Rerender;
            }
        }
        (void)doc;
        (void)track;
        return ta::TrackClass::LayerLive;
    }

}

// ==================================================================== units

GlyphUnits jasperengine::titleart::glyphUnitsOf(const Json& textLayer, ta::StaggerUnit unit, TitleTextLayout* layout){
    GlyphUnits out;
    struct Element{ uint32_t index; uint32_t line; bool space; };
    std::vector<Element> elements;
    if (layout){
        const LaidOutText& text = layout->layout(textLayer);
        for (const LaidOutCluster& c : text.clusters){
            bool space = !c.codepoints.empty();
            for (uint32_t cp : c.codepoints) space = space && isSpace(cp);
            if (c.inkBox.empty()) space = true;
            elements.push_back({c.index, c.line, space});
        }
    }else{
        const std::vector<uint32_t> cps = codepointsOf(textLayer.value("text", std::string()));
        uint32_t line = 0;
        for (uint32_t i = 0; i < cps.size(); i++){
            elements.push_back({i, line, isSpace(cps[i])});
            if (cps[i] == '\n') line++;
        }
    }
    // logical order (RTL and vertical text included): by the first code point's index
    std::vector<size_t> order(elements.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b){ return elements[a].index < elements[b].index; });
    out.clusterUnit.assign(elements.size(), -1);
    int32_t current = -1;
    uint32_t lastLine = ~0u;
    bool inWord = false;
    for (size_t k : order){
        const Element& e = elements[k];
        switch (unit){
            case ta::StaggerUnit::Glyph:
            case ta::StaggerUnit::Child:
                if (!e.space) out.clusterUnit[k] = (int32_t)out.count++;
                break;
            case ta::StaggerUnit::Word:
                if (e.space || e.line != lastLine) inWord = false;
                if (!e.space){
                    if (!inWord){
                        current = (int32_t)out.count++;
                        inWord = true;
                    }
                    out.clusterUnit[k] = current;
                }
                lastLine = e.line;
                break;
            case ta::StaggerUnit::Line:
                if (!e.space){
                    if (e.line != lastLine){
                        current = (int32_t)out.count++;
                        lastLine = e.line;
                    }
                    out.clusterUnit[k] = current;
                }
                break;
        }
    }
    return out;
}

// ==================================================================== compile

ta::CompiledTitleAnimation jasperengine::titleart::compileAnimation(const TitleArtDoc& doc, const ta::TitleAnimation& animation,
                                                                    TitleTextLayout* layout, std::vector<TitleProblem>* problems,
                                                                    const std::string& path){
    ta::CompiledTitleAnimation out;
    out.id = animation.id;
    out.name = animation.name;
    out.next = animation.next;
    out.role = animation.role;
    out.wrap = animation.wrap;
    out.speed = animation.speed;
    out.overlay = animation.overlay;
    out.rerender = animation.rerender;
    out.rerenderFps = animation.rerenderFps;
    out.flipbookSettings = animation.flipbook;
    out.autoDuration = animation.duration <= 0.0f;
    const uint64_t documentSeed = (uint64_t)doc.root.value("seed", 1);
    auto note = [&](TitleSeverity s, const std::string& code, const std::string& layer, const std::string& at, const std::string& message){
        if (problems) problems->push_back(problem(s, code, layer, at, message));
    };
    for (size_t i = 0; i < animation.tracks.size(); i++){
        const ta::TitleTrack& track = animation.tracks[i];
        const std::string at = path + "tracks[" + std::to_string(i) + "]";
        ta::CompiledTitleTrack c;
        c.layer = track.layer;
        c.field = track.field;
        c.pivot = track.pivot;
        c.mute = track.mute;
        const TrackFieldInfo info = resolveTrackField(doc, track.layer, track.field);
        if (!info.ok){
            c.orphan = true;
            c.cls = ta::TrackClass::None;
            const bool orphan = info.code == "layer_missing" || info.code == "bad_path";
            note(TitleSeverity::Warning, orphan ? "anim_orphan" : "not_animatable", track.layer, at,
                 animation.name + ": " + track.field + ": " + info.error + (orphan ? " (the track is kept, not played)" : ""));
            out.tracks.push_back(std::move(c));
            continue;
        }
        c.target = info.target;
        c.titleField = info.titleField;
        c.glyphField = info.glyphField;
        c.component = info.component;
        c.value = info.value;
        c.width = (uint8_t)std::max(1, info.width);
        c.min = info.min;
        c.max = info.max;
        std::string reason;
        c.cls = classOf(doc, track, info, &reason);
        const size_t width = c.width;
        const bool virtualField = info.target == ta::TrackTarget::Title || info.target == ta::TrackTarget::Glyph;
        // R4: relative tracks add or multiply; stepped ones are refused (absolute)
        if (track.relative && info.stepped)
            note(TitleSeverity::Warning, "relative_stepped", track.layer, at, animation.name + ": " + track.field + " is stepped: it plays absolute");
        if (track.relative && !info.stepped){
            if (virtualField){
                const ta::VirtualFieldInfo v = info.target == ta::TrackTarget::Title ? ta::titleFieldInfo(info.titleField) : ta::glyphFieldInfo(info.glyphField);
                c.combine = v.relativeCombine;
            }else{
                c.combine = multipliesWhenRelative(info, info.longPath) ? ta::TrackCombine::Multiply : ta::TrackCombine::Add;
            }
        }
        // rests: virtual and child fields hold raw values (their consumers combine per element);
        // a document field combines with the document's value
        if (virtualField || info.target == ta::TrackTarget::Child){
            const float identity = c.combine == ta::TrackCombine::Multiply ? 1.0f : 0.0f;
            c.rest.assign(width, identity);
        }else if (!info.stepped){
            std::vector<float> rest = floatsOf(info.rest, info.value, info.width);
            if (info.component >= 0) rest = {rest.size() > (size_t)info.component ? rest[(size_t)info.component] : 0.0f};
            c.rest = rest;
        }
        if (track.reseed){
            if (!info.seed || !info.integer){
                c.orphan = true;
                note(TitleSeverity::Warning, "reseed_not_seed", track.layer, at, animation.name + ": " + track.field + " is not a seed: the reseed is not played");
                out.tracks.push_back(std::move(c));
                continue;
            }
            c.source = ta::TrackSource::Reseed;
            c.value = ta::TrackValue::Number;
            c.width = 1;
            c.reseedRate = track.reseed->rate;
            c.reseedOffset = track.reseed->offset;
            c.combine = ta::TrackCombine::Add;
            c.rest = floatsOf(info.rest, ta::TrackValue::Number, 1);
            c.cls = ta::TrackClass::Rerender;
        }else if (track.drive){
            c.source = ta::TrackSource::Drive;
            c.driveFrom = track.drive->from;
            c.driveTo = track.drive->to;
            c.driveFrom.resize(width, c.driveFrom.empty() ? 0.0f : c.driveFrom.back());
            c.driveTo.resize(width, c.driveTo.empty() ? 0.0f : c.driveTo.back());
            c.cycles = track.drive->cycles;
            c.rate = track.drive->rate;
            c.driveWrap = track.drive->wrap;
            c.driveEase = track.drive->ease;
            if (info.stepped){
                c.orphan = true;
                note(TitleSeverity::Warning, "drive_stepped", track.layer, at, animation.name + ": " + track.field + " is stepped: drives need numbers");
            }
        }else if (info.stepped){
            std::vector<ta::TitleKey> keys = track.keys;
            bool eased = false;
            for (ta::TitleKey& k : keys){
                if (k.stepped.empty()){
                    // a number keyed on an integer, bool or enum field: its JSON text
                    if (k.key.value.size() == 1){
                        const double v = k.key.value[0];
                        k.stepped = info.integer || info.value == ta::TrackValue::Stepped ? Json(std::llround(v)).dump() : Json(v).dump();
                    }else{
                        Json a = Json::array();
                        for (float f : k.key.value) a.push_back(f);
                        k.stepped = a.dump();
                    }
                }
                eased = eased || k.key.ease != EaseType::LINEAR;
            }
            if (eased) note(TitleSeverity::Info, "ease_dropped", track.layer, at, animation.name + ": " + track.field + " is stepped: its eases are dropped");
            ta::compileSteppedKeys(keys, c);
            c.combine = ta::TrackCombine::Replace;
        }else{
            std::vector<ClipKey> keys;
            int clamped = -1;
            for (size_t k = 0; k < track.keys.size(); k++){
                ClipKey key = track.keys[k].key;
                std::vector<float> v;
                if (!keyFloats(track.keys[k], width, v)){
                    note(TitleSeverity::Warning, "bad_key", track.layer, at + ".keys[" + std::to_string(k) + "]",
                         animation.name + ": " + track.field + " needs " + std::to_string(width) + " values per key: key dropped");
                    continue;
                }
                key.value = v;
                // R3: a key outside the field's range is reported once per track (samples clamp)
                if (c.combine == ta::TrackCombine::Replace && clamped < 0)
                    for (float x : v) if (x < info.min || x > info.max){ clamped = (int)k; break; }
                keys.push_back(std::move(key));
            }
            if (clamped >= 0)
                note(TitleSeverity::Info, "key_clamped", track.layer, at + ".keys[" + std::to_string(clamped) + "]",
                     animation.name + ": " + track.field + " key " + std::to_string(clamped) + " is outside the field's range and plays clamped");
            ta::compileNumericKeys(keys, width, c);
        }
        // stagger: units of the text (glyph tracks) or the group's children (child tracks)
        if (track.stagger && (info.target == ta::TrackTarget::Glyph || info.target == ta::TrackTarget::Child)){
            size_t units = 0;
            if (info.target == ta::TrackTarget::Glyph && info.layer){
                const GlyphUnits g = glyphUnitsOf(*info.layer, track.stagger->unit, layout);
                c.elementUnit = g.clusterUnit;
                units = g.count;
            }else if (info.layer){
                const size_t children = layerChildren(*info.layer).size();
                c.elementUnit.resize(children);
                for (size_t k = 0; k < children; k++) c.elementUnit[k] = (int32_t)k;
                units = children;
            }
            if (units > 0) ta::staggerDelays(*track.stagger, units, documentSeed, c.unitDelays);
        }
        out.tracks.push_back(std::move(c));
    }
    ta::compileMarkers(animation, out);
    out.duration = ta::resolveDuration(animation, out);
    out.loopStart = std::min((double)animation.loopStart, out.duration * 0.999);
    out.layoutPose();
    return out;
}

std::shared_ptr<ta::TitleAnimLibrary> jasperengine::titleart::compileAnimations(const TitleArtDoc& doc, TitleTextLayout* layout,
                                                                                std::vector<TitleProblem>* problems){
    auto lib = std::make_shared<ta::TitleAnimLibrary>();
    lib->fromDocument = true;
    const ta::TitleAnimationSet set = readAnimationSet(doc);
    lib->defaultName = set.defaultName;
    for (size_t i = 0; i < set.animations.size(); i++)
        lib->animations.push_back(compileAnimation(doc, set.animations[i], layout, problems, "animation.animations[" + std::to_string(i) + "]."));
    return lib;
}

// ==================================================================== plan

const char* jasperengine::titleart::trackPlayName(TrackPlay play){
    switch (play){
        case TrackPlay::Plays: return "plays";
        case TrackPlay::FallsBack: return "falls_back";
        case TrackPlay::NotPlayed: return "not_played";
    }
    return "plays";
}

const char* jasperengine::titleart::renderModeName(TitleArtRenderMode mode){
    switch (mode){
        case TitleArtRenderMode::BAKED: return "baked";
        case TitleArtRenderMode::REALTIME: return "realtime";
        case TitleArtRenderMode::GLYPH_ATLAS: return "glyph_atlas";
    }
    return "baked";
}

bool jasperengine::titleart::renderModeFromName(const std::string& name, TitleArtRenderMode& out){
    if (name == "baked") out = TitleArtRenderMode::BAKED;
    else if (name == "realtime") out = TitleArtRenderMode::REALTIME;
    else if (name == "glyph_atlas") out = TitleArtRenderMode::GLYPH_ATLAS;
    else return false;
    return true;
}

const AnimationPlan* TitleAnimPlan::find(const std::string& name) const{
    for (const AnimationPlan& a : animations) if (a.name == name) return &a;
    return nullptr;
}

TitleAnimPlan::TestFlags& TitleAnimPlan::testFlags(){
    static TestFlags flags;
    return flags;
}

std::string jasperengine::titleart::liveParamBlocker(const Json& layer, const std::string& longPath){
    std::vector<FieldSegment> segs;
    if (!parseFieldPath(longPath, segs, nullptr) || segs.size() < 4) return "";
    // effects[i].params.<p> (layer effects) or generator.params.<p>
    if (segs[0].key == "generator"){
        const Json& gen = layer.value("generator", Json::object());
        const GeneratorDef* def = findGenerator(gen.value("type", std::string()));
        if (!def) return "the generator type is not registered";
        const std::string param = segs.back().key;
        if (def->live.kind == LiveKind::None) return std::string(def->label) + " has no live variant: " + param + " re-renders";
        return "";
    }
    if (segs[0].key != "effects" || !segs[1].isIndex) return "";
    const Json& effects = layer.value("effects", Json::array());
    const size_t index = segs[1].index;
    if (index >= effects.size()) return "";
    const EffectDef* def = findEffect(effects[index].value("type", std::string()));
    if (!def) return "the effect type is not registered";
    const std::string param = segs.back().key;
    bool listed = false;
    for (const char* p : def->live.params) listed = listed || param == p;
    if (def->live.kind == LiveKind::None || !listed)
        return std::string(def->label) + " has no live variant for " + param + " (S83 D10: live.kind none): it re-renders";
    // D10's suffix rule: every enabled effect after it scales its split
    for (size_t k = index + 1; k < effects.size(); k++){
        if (!effects[k].value("enabled", true)) continue;
        const EffectDef* after = findEffect(effects[k].value("type", std::string()));
        if (!after || after->live.kind != LiveKind::ScaleSplit){
            return std::string(def->label) + " " + param + " is followed by " + (after ? after->label : "an unknown effect") +
                   ", which is not scale_split (S83 D10): it re-renders";
        }
    }
    return "";
}

std::string jasperengine::titleart::layerSplitBlocker(const TitleArtDoc& doc, const std::string& layerId, std::string* code){
    auto blocked = [&](const char* c, const std::string& why){
        if (code) *code = c;
        return why;
    };
    const Json* self = findLayer(doc.root, layerId);
    if (!self) return blocked("layer_missing", "the layer is gone");
    if (self->value("clip", false)) return blocked("clipped", "it is clipped to the layer below");
    std::string found;
    std::string foundCode;
    forEachLayer(doc.root, [&](const Json& layer, const Json*, int){
        if (!found.empty()) return false;
        for (const Json& m : layer.value("masks", Json::array())){
            if (m.value("type", std::string()) == "layer" && m.value("layer", std::string()) == layerId){
                found = "layer '" + layer.value("name", std::string()) + "' masks with it";
                foundCode = "mask_names_it";
                return false;
            }
        }
        if (layerTypeOf(layer) == LayerType::Generator){
            const Json& params = layer.value("generator", Json::object()).value("params", Json::object());
            if (params.value("follow", std::string()) == layerId){
                found = "generator '" + layer.value("name", std::string()) + "' follows it";
                foundCode = "followed";
                return false;
            }
        }
        return true;
    });
    if (!found.empty()) return blocked(foundCode.c_str(), found);
    // the chain of sibling lists from the top: a clip above it, an adjustment above it, an
    // enclosing group's effects
    std::vector<const Json*> chain;
    std::function<bool(const Json&)> walk = [&](const Json& list) -> bool{
        for (const Json& l : list){
            if (l.value("id", std::string()) == layerId){
                chain.push_back(&l);
                return true;
            }
            if (layerTypeOf(l) == LayerType::Group){
                chain.push_back(&l);
                if (walk(layerChildren(l))) return true;
                chain.pop_back();
            }
        }
        return false;
    };
    walk(doc.layers());
    const Json* siblings = &doc.layers();
    for (size_t depth = 0; depth < chain.size(); depth++){
        const Json* at = chain[depth];
        size_t index = 0;
        for (; index < siblings->size(); index++) if (&(*siblings)[index] == at) break;
        if (depth + 1 == chain.size() && index + 1 < siblings->size() && (*siblings)[index + 1].value("clip", false))
            return blocked("clip_base", "layer '" + (*siblings)[index + 1].value("name", std::string()) + "' is clipped to it");
        if (!TitleAnimPlan::testFlags().ignoreAdjustments){
            for (size_t k = index + 1; k < siblings->size(); k++){
                if (layerTypeOf((*siblings)[k]) == LayerType::Adjustment && (*siblings)[k].value("visible", true))
                    return blocked("adjustment_above", "adjustment layer '" + (*siblings)[k].value("name", std::string()) + "' is above it");
            }
        }
        if (depth + 1 < chain.size()){
            for (const Json& e : at->value("effects", Json::array())){
                if (e.value("enabled", true))
                    return blocked("group_effects", "its group '" + at->value("name", std::string()) + "' has an effect (" + e.value("type", std::string()) + ")");
            }
            siblings = &layerChildren(*at);
        }
    }
    return "";
}

TitleAnimPlan jasperengine::titleart::buildAnimPlan(const TitleArtDoc& doc, const std::vector<ta::CompiledTitleAnimation>& animations,
                                                    TitleArtRenderMode mode){
    TitleAnimPlan plan;
    plan.mode = mode;
    const TitleGlyphAtlasPlan glyph = mode == TitleArtRenderMode::GLYPH_ATLAS ? planTitleGlyphAtlas(doc) : TitleGlyphAtlasPlan();
    const bool atlas = mode == TitleArtRenderMode::GLYPH_ATLAS && glyph.fallback.empty() && !glyph.textLayer.empty();
    // a Glyph Atlas title that falls back renders as Realtime (S81 R25)
    const TitleArtRenderMode effective = mode == TitleArtRenderMode::GLYPH_ATLAS && !atlas ? TitleArtRenderMode::REALTIME : mode;
    std::map<std::string, std::string> splitBlockers;
    auto splitBlocker = [&](const std::string& layer, std::string& code) -> std::string{
        auto it = splitBlockers.find(layer);
        if (it == splitBlockers.end()){
            std::string c;
            const std::string why = layerSplitBlocker(doc, layer, &c);
            it = splitBlockers.emplace(layer, why.empty() ? std::string() : c + "|" + why).first;
        }
        const size_t bar = it->second.find('|');
        code = bar == std::string::npos ? std::string() : it->second.substr(0, bar);
        return bar == std::string::npos ? std::string() : it->second.substr(bar + 1);
    };
    for (const ta::CompiledTitleAnimation& a : animations){
        AnimationPlan ap;
        ap.name = a.name;
        ap.mode = mode;
        for (size_t i = 0; i < a.tracks.size(); i++){
            const ta::CompiledTitleTrack& t = a.tracks[i];
            TrackPlan tp;
            tp.track = i;
            tp.layer = t.layer;
            tp.field = t.field;
            tp.cls = t.cls;
            auto set = [&](TrackPlay play, const char* how, const std::string& reason = {}, const char* code = ""){
                tp.play = play;
                tp.how = how;
                tp.reason = reason;
                tp.code = code;
            };
            if (t.orphan || t.cls == ta::TrackClass::None){
                set(TrackPlay::NotPlayed, "none", t.orphan ? "its layer or field is gone" : "not animatable", t.orphan ? "anim_orphan" : "not_animatable");
            }else if (t.mute){
                set(TrackPlay::NotPlayed, "none", "muted", "muted");
            }else if (t.cls == ta::TrackClass::TitleLive){
                set(TrackPlay::Plays, "companion");
            }else if (effective == TitleArtRenderMode::BAKED){
                if (a.overlay){
                    set(TrackPlay::NotPlayed, "none", "an overlay in Baked plays its title-live tracks only", "overlay_needs_title_live");
                }else{
                    set(TrackPlay::Plays, "flipbook");
                    ap.flipbook = true;
                }
            }else if (effective == TitleArtRenderMode::REALTIME){
                const Json* layer = t.layer.empty() ? nullptr : findLayer(doc.root, t.layer);
                const bool merged = layer && layer->value("glyph_mode", std::string("cells")) == "merged";
                const bool prerender = a.rerender == ta::RerenderPolicy::Prerender;
                auto rerender = [&](const std::string& reason, const char* code){
                    set(reason.empty() ? TrackPlay::Plays : TrackPlay::FallsBack, prerender ? "prerender" : "rerender", reason, code);
                    if (prerender) ap.prerender = true;
                    else ap.rerender = true;
                };
                switch (t.cls){
                    case ta::TrackClass::LayerLive:{
                        std::string code;
                        const std::string why = splitBlocker(t.layer, code);
                        if (why.empty()){
                            set(TrackPlay::Plays, "layer_split");
                            if (std::find(ap.layerSplits.begin(), ap.layerSplits.end(), t.layer) == ap.layerSplits.end()) ap.layerSplits.push_back(t.layer);
                        }else{
                            rerender("not a layer split: " + why, code.c_str());
                        }
                        break;
                    }
                    case ta::TrackClass::Glyph:
                        if (t.target == ta::TrackTarget::Glyph && merged){
                            rerender("glyph_mode merged: glyph transforms re-render", "glyph_merged");
                        }else if (t.target == ta::TrackTarget::Child){
                            rerender("child tracks of a group re-render in Realtime", "child_rerender");
                        }else{
                            set(TrackPlay::Plays, "glyph_cells");
                            if (std::find(ap.glyphLayers.begin(), ap.glyphLayers.end(), t.layer) == ap.glyphLayers.end()) ap.glyphLayers.push_back(t.layer);
                        }
                        break;
                    default:
                        rerender("", "");
                        break;
                }
            }else{
                // Glyph Atlas: the glyph layer animates its cells; plates re-render; the glyph
                // layer's style cannot (it would rebuild the atlas every sample)
                const bool glyphLayer = t.layer == glyph.textLayer;
                switch (t.cls){
                    case ta::TrackClass::Glyph:
                        if (glyphLayer && t.target == ta::TrackTarget::Glyph){
                            set(TrackPlay::Plays, "glyph_cells");
                            if (std::find(ap.glyphLayers.begin(), ap.glyphLayers.end(), t.layer) == ap.glyphLayers.end()) ap.glyphLayers.push_back(t.layer);
                        }else{
                            set(TrackPlay::FallsBack, "plate_rerender", "glyph tracks of a plate layer re-render the plate", "plate_rerender");
                            ap.rerender = true;
                        }
                        break;
                    case ta::TrackClass::GlyphAtlasLive:
                        if (glyphLayer) set(TrackPlay::Plays, "cells_swap");
                        else{
                            set(TrackPlay::FallsBack, "plate_rerender", "the text of a plate layer re-renders the plate", "plate_rerender");
                            ap.rerender = true;
                        }
                        break;
                    case ta::TrackClass::LayerLive:
                        if (glyphLayer){
                            set(TrackPlay::Plays, "glyph_cells");
                        }else{
                            // a plate renders per state (S81 R25): its live values re-render it, rate-limited
                            set(TrackPlay::FallsBack, "plate_rerender", "a plate layer re-renders for its live values", "plate_rerender");
                            ap.rerender = true;
                        }
                        break;
                    default:
                        if (glyphLayer){
                            set(TrackPlay::NotPlayed, "none", "re-render tracks on the glyph layer's style are not played in Glyph Atlas",
                                "rerender_in_glyph_atlas");
                        }else{
                            set(TrackPlay::Plays, "plate_rerender");
                            ap.rerender = true;
                        }
                        break;
                }
            }
            ap.tracks.push_back(std::move(tp));
        }
        // R16: Glow Pulse needs a glow output, which frames do not have
        if (ap.flipbook){
            for (TrackPlan& tp : ap.tracks){
                if (a.tracks[tp.track].titleField == ta::TitleField::GlowPulse && tp.play == TrackPlay::Plays){
                    tp.play = TrackPlay::NotPlayed;
                    tp.how = "none";
                    tp.reason = "Glow Pulse needs a glow output, which flipbook frames do not have: it holds 0 while frames play";
                    tp.code = "glow_pulse_over_frames";
                    ap.notes.push_back("glow_pulse_over_frames");
                }
            }
        }
        if (a.overlay && effective == TitleArtRenderMode::BAKED){
            for (const TrackPlan& tp : ap.tracks)
                if (tp.code == "overlay_needs_title_live"){
                    ap.notes.push_back("overlay_needs_title_live");
                    break;
                }
        }
        plan.animations.push_back(std::move(ap));
    }
    return plan;
}
