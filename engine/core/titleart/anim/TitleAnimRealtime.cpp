// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimRealtime.h"

#include "titleanim/TitleAnimDataIO.h"
#include "titleanim/TitleAnimSampling.h"
#include "titleart/TitleArtIO.h"
#include "util/Fnv.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
namespace ta = jasperengine::titleanim;

namespace {

    double rawOf(const ta::CompiledTitleTrack& t, double value, size_t i){
        const double rest = i < t.rest.size() ? (double)t.rest[i] : 0.0;
        switch (t.combine){
            case ta::TrackCombine::Add: return value - rest;
            case ta::TrackCombine::Multiply: return rest != 0.0 ? value / rest : value;
            case ta::TrackCombine::Replace: break;
        }
        return value;
    }

    double combine(ta::TrackCombine how, double current, double raw){
        switch (how){
            case ta::TrackCombine::Add: return current + raw;
            case ta::TrackCombine::Multiply: return current * raw;
            case ta::TrackCombine::Replace: break;
        }
        return raw;
    }

    // a document value as floats of its kind
    std::vector<double> numbersOf(const Json& value){
        std::vector<double> out;
        if (value.is_number()) out.push_back(value.get<double>());
        else if (value.is_boolean()) out.push_back(value.get<bool>() ? 1.0 : 0.0);
        else if (value.is_array()) for (const Json& v : value) out.push_back(v.is_number() ? v.get<double>() : 0.0);
        else if (value.is_string()){
            float rgb[3];
            if (ta::parseHexColor(value.get<std::string>(), rgb)) out = {rgb[0], rgb[1], rgb[2]};
        }
        return out;
    }

    Json jsonOf(const std::vector<double>& v, ta::TrackValue type, bool integer, bool asColor){
        if (asColor || type == ta::TrackValue::Color){
            float rgb[3] = {0, 0, 0};
            for (size_t i = 0; i < 3 && i < v.size(); i++) rgb[i] = (float)v[i];
            return ta::formatHexColor(rgb);
        }
        if (v.size() == 1) return integer ? Json((int64_t)std::llround(v[0])) : Json(ta::roundToFloat(v[0]));
        Json a = Json::array();
        for (double x : v) a.push_back(ta::roundToFloat(x));
        return a;
    }

    double blendValue(double a, double b, double w){
        return a + (b - a) * w;
    }

}

// ==================================================================== PoseValues

const Json* PoseValues::current(const TitleArtDoc& doc, const std::string& layer, const std::string& path, Json& scratch) const{
    auto it = values.find({layer, path});
    if (it != values.end()) return &it->second;
    const Json* object = layer.empty() ? &doc.root : findLayer(doc.root, layer);
    if (!object) return nullptr;
    const Json* v = getTitleArtField(*object, layer.empty() ? "document" : "layer", path);
    if (!v) return nullptr;
    scratch = *v;
    return &scratch;
}

void PoseValues::add(const TitleArtDoc& doc, const ta::CompiledTitleAnimation& animation, const ta::TitleAnimPose& pose, bool overlay,
                     const TrackFilter& include){
    if (pose.values.size() < animation.poseSize) return;
    for (const ta::CompiledTitleTrack& t : animation.tracks){
        if (t.mute || t.orphan || t.cls == ta::TrackClass::None || t.target == ta::TrackTarget::Title) continue;
        if (include && !include(t)) continue;
        const size_t width = std::max<size_t>(1, t.width);
        const double* v = pose.values.data() + t.poseOffset;
        Json scratch;
        if (t.target == ta::TrackTarget::Field){
            // a component writes into its whole value (the rest of it from what is there)
            std::string path = t.field;
            std::string base = path;
            int component = t.component;
            if (component >= 0){
                int c = -1;
                if (ta::splitComponent(path, base, c)) path = base;
            }
            const Json* now = current(doc, t.layer, path, scratch);
            if (t.value == ta::TrackValue::Stepped){
                const size_t key = (size_t)std::max(0.0, v[0]);
                if (key >= t.stepValues.size()) continue;
                Json value = Json::parse(t.stepValues[key], nullptr, false);
                if (value.is_discarded()) continue;
                values[{t.layer, path}] = std::move(value);
                continue;
            }
            std::vector<double> next = now ? numbersOf(*now) : std::vector<double>();
            const bool color = now && now->is_string();
            if (component >= 0){
                if (next.size() <= (size_t)component) next.resize((size_t)component + 1, 0.0);
                next[(size_t)component] = overlay ? combine(t.relativeCombine, next[(size_t)component], rawOf(t, v[0], 0)) : v[0];
            }else{
                next.resize(width, 0.0);
                for (size_t i = 0; i < width; i++) next[i] = overlay ? combine(t.relativeCombine, next[i], rawOf(t, v[i], i)) : v[i];
            }
            for (double& x : next) x = std::min(t.max, std::max(t.min, x));
            values[{t.layer, path}] = jsonOf(next, t.value, t.integer, color);
        }else if (t.target == ta::TrackTarget::Child){
            const Json* group = findLayer(doc.root, t.layer);
            if (!group) continue;
            const Json& children = layerChildren(*group);
            const std::string path = t.field.substr(6);
            for (size_t k = 0; k < children.size(); k++){
                const int32_t unit = t.elementUnit.empty() ? 0 : (k < t.elementUnit.size() ? t.elementUnit[k] : -1);
                if (unit < 0 || (uint32_t)unit >= t.units) continue;
                const std::string id = children[k].value("id", std::string());
                const double* u = v + (size_t)unit * width;
                const Json* now = current(doc, id, path, scratch);
                std::vector<double> next = now ? numbersOf(*now) : std::vector<double>(width, 0.0);
                const bool color = now && now->is_string();
                next.resize(width, 0.0);
                // the pose holds raw values: each child combines with its own value
                const ta::TrackCombine how = overlay ? t.relativeCombine : t.combine;
                for (size_t i = 0; i < width; i++) next[i] = std::min(t.max, std::max(t.min, combine(how, next[i], u[i])));
                values[{id, path}] = jsonOf(next, t.value, t.integer, color);
            }
        }else if (t.target == ta::TrackTarget::Glyph){
            // a merged text layer's glyph transforms are its characters' adjustments (re-render, R15)
            const Json* layer = findLayer(doc.root, t.layer);
            if (!layer || layer->value("glyph_mode", std::string("cells")) != "merged") continue;
            const ta::GlyphField f = t.glyphField;
            if (f != ta::GlyphField::Offset && f != ta::GlyphField::Rotation && f != ta::GlyphField::Scale && f != ta::GlyphField::Stretch) continue;
            for (size_t e = 0; e < t.elementIndex.size(); e++){
                const int32_t unit = t.elementUnit.empty() ? 0 : t.elementUnit[e];
                if (unit < 0 || (uint32_t)unit >= t.units) continue;
                const double* u = v + (size_t)unit * width;
                const std::string chars = "chars." + std::to_string(t.elementIndex[e]) + ".";
                const ta::TrackCombine how = overlay || t.combine != ta::TrackCombine::Replace ? t.relativeCombine : ta::TrackCombine::Replace;
                auto write = [&](const std::string& member, size_t from, size_t count, double identity){
                    const Json* now = current(doc, t.layer, chars + member, scratch);
                    std::vector<double> next = now ? numbersOf(*now) : std::vector<double>(count, identity);
                    next.resize(count, identity);
                    for (size_t i = 0; i < count; i++){
                        const double value = t.component >= 0 ? (t.component == (int)(from + i) ? u[0] : (how == ta::TrackCombine::Multiply ? 1.0 : 0.0))
                                                              : u[std::min(from + i, width - 1)];
                        next[i] = how == ta::TrackCombine::Replace ? value : combine(how, next[i], value);
                    }
                    values[{t.layer, chars + member}] = count == 1 ? Json(ta::roundToFloat(next[0])) : jsonOf(next, ta::TrackValue::Vec2, false, false);
                };
                if (f == ta::GlyphField::Offset) write("offset", 0, 2, 0.0);
                else if (f == ta::GlyphField::Rotation) write("rotation", 0, 1, 0.0);
                else{
                    // scale x stretch through the character's horizontal and vertical scales
                    auto axis = [&](const char* member, size_t index){
                        const Json* now = current(doc, t.layer, chars + member, scratch);
                        double value = now && now->is_number() ? now->get<double>() : 1.0;
                        const double factor = f == ta::GlyphField::Scale ? u[0] : (t.component >= 0 ? (t.component == (int)index ? u[0] : 1.0) : u[index]);
                        value = how == ta::TrackCombine::Replace && f == ta::GlyphField::Stretch ? factor : value * factor;
                        values[{t.layer, chars + member}] = ta::roundToFloat(std::min(10.0, std::max(0.1, value)));
                    };
                    axis("h_scale", 0);
                    axis("v_scale", 1);
                }
            }
        }
    }
}

void PoseValues::blendToward(const PoseValues& other, double weight){
    std::map<std::pair<std::string, std::string>, Json> out = other.values;
    for (const auto& [key, from] : values){
        auto it = out.find(key);
        if (it == out.end()) continue;
        const std::vector<double> a = numbersOf(from), b = numbersOf(it->second);
        if (a.empty() || a.size() != b.size() || from.type() != it->second.type()) continue;
        std::vector<double> mixed(a.size());
        for (size_t i = 0; i < a.size(); i++) mixed[i] = blendValue(a[i], b[i], weight);
        it->second = jsonOf(mixed, from.is_string() ? ta::TrackValue::Color : ta::TrackValue::Number, from.is_number_integer(), from.is_string());
    }
    values = std::move(out);
}

std::vector<FieldOverride> PoseValues::overrides() const{
    std::vector<FieldOverride> out;
    out.reserve(values.size());
    for (const auto& [key, value] : values) out.push_back({key.first, key.second, value});
    return out;
}

uint64_t PoseValues::hash() const{
    uint64_t h = 0x85;
    for (const auto& [key, value] : values) h = fnv1a64(key.first + "\x1f" + key.second + "\x1f" + value.dump(), h);
    return h;
}

// ==================================================================== glyphs

bool GlyphPose::identity() const{
    return offset[0] == 0.0 && offset[1] == 0.0 && rotation == 0.0 && scale[0] == 1.0 && scale[1] == 1.0 && opacity == 1.0 &&
           tint[0] == 1.0 && tint[1] == 1.0 && tint[2] == 1.0 && flashAmount == 0.0;
}

void jasperengine::titleart::addGlyphPoses(const ta::CompiledTitleAnimation& animation, const ta::TitleAnimPose& pose, const std::string& layerId,
                                           bool overlay, std::map<uint32_t, GlyphPose>& poses){
    if (pose.values.size() < animation.poseSize) return;
    for (const ta::CompiledTitleTrack& t : animation.tracks){
        if (t.mute || t.orphan || t.target != ta::TrackTarget::Glyph || t.layer != layerId) continue;
        const size_t width = std::max<size_t>(1, t.width);
        const ta::TrackCombine how = overlay ? t.relativeCombine : t.combine;
        for (size_t e = 0; e < t.elementIndex.size(); e++){
            const int32_t unit = t.elementUnit.empty() ? 0 : t.elementUnit[e];
            if (unit < 0 || (uint32_t)unit >= t.units) continue;
            const double* u = pose.values.data() + t.poseOffset + (size_t)unit * width;
            GlyphPose& g = poses[t.elementIndex[e]];
            g.cluster = t.elementIndex[e];
            if (t.pivot == ta::TrackPivot::Baseline || t.pivot == ta::TrackPivot::Bottom || t.pivot == ta::TrackPivot::Top || t.pivot == ta::TrackPivot::Center)
                g.pivot = t.pivot;
            auto apply = [&](double* dst, size_t count){
                for (size_t i = 0; i < count; i++){
                    if (t.component >= 0 && (int)i != t.component) continue;
                    const double value = t.component >= 0 ? u[0] : u[std::min(i, width - 1)];
                    dst[i] = combine(how, dst[i], value);
                }
            };
            switch (t.glyphField){
                case ta::GlyphField::Offset: apply(g.offset, 2); break;
                case ta::GlyphField::Rotation: g.rotation = combine(how, g.rotation, u[0]); break;
                case ta::GlyphField::Scale:
                    g.scale[0] = how == ta::TrackCombine::Replace ? u[0] : g.scale[0] * u[0];
                    g.scale[1] = how == ta::TrackCombine::Replace ? u[0] : g.scale[1] * u[0];
                    break;
                case ta::GlyphField::Stretch:
                    for (size_t i = 0; i < 2; i++){
                        if (t.component >= 0 && (int)i != t.component) continue;
                        const double value = t.component >= 0 ? u[0] : u[i];
                        g.scale[i] = how == ta::TrackCombine::Replace ? value : g.scale[i] * value;
                    }
                    break;
                case ta::GlyphField::Opacity: g.opacity = std::min(1.0, std::max(0.0, combine(how, g.opacity, u[0]))); break;
                case ta::GlyphField::Tint: apply(g.tint, 3); break;
                case ta::GlyphField::FlashColor:
                    for (size_t i = 0; i < 3; i++) if (t.component < 0 || (int)i == t.component) g.flash[i] = t.component >= 0 ? u[0] : u[i];
                    break;
                case ta::GlyphField::FlashAmount: g.flashAmount = combine(how, g.flashAmount, u[0]); break;
                default: break;
            }
        }
    }
}

std::vector<std::string> jasperengine::titleart::cellLayersOf(const TitleArtDoc& doc, const ta::CompiledTitleAnimation& animation){
    std::vector<std::string> out;
    for (const ta::CompiledTitleTrack& t : animation.tracks){
        if (t.mute || t.orphan || t.target != ta::TrackTarget::Glyph) continue;
        if (std::find(out.begin(), out.end(), t.layer) != out.end()) continue;
        const Json* layer = findLayer(doc.root, t.layer);
        if (layer && layer->value("glyph_mode", std::string("cells")) == "cells") out.push_back(t.layer);
    }
    return out;
}

// ==================================================================== the player

void jasperengine::titleart::playerPose(const TitleArtDoc& doc, const ta::TitleAnimPlayer& player, bool quantize, const TrackFilter& include,
                                        PlayerPose& out){
    out = PlayerPose();
    auto sampled = [&](const ta::PlayingAnimation& p, ta::TitleAnimPose& scratch, double* local) -> const ta::TitleAnimPose&{
        if (!quantize){
            if (local) *local = p.localTime();
            return p.pose;
        }
        // R18: a re-render value changes at k / rerender_fps of the animation's own time
        const double fps = std::max(1.0f, p.animation->rerenderFps);
        const double t = std::floor(p.time * fps + 1e-6) / fps;
        const double l = ta::wrapAnimationTime(t, p.animation->duration, p.animation->loopStart, p.wrap);
        if (local) *local = l;
        scratch.size(*p.animation);
        ta::sampleAnimation(*p.animation, l, scratch);
        return scratch;
    };
    auto glyphs = [&](const ta::PlayingAnimation& p, const ta::TitleAnimPose& pose, bool overlay,
                      std::map<std::string, std::map<uint32_t, GlyphPose>>& into){
        for (const std::string& layer : cellLayersOf(doc, *p.animation)) addGlyphPoses(*p.animation, pose, layer, overlay, into[layer]);
    };
    ta::TitleAnimPose scratch;
    if (const ta::PlayingAnimation* base = player.baseAnimation()){
        const ta::TitleAnimPose& pose = sampled(*base, scratch, &out.sampleTime);
        out.values.add(doc, *base->animation, pose, false, include);
        // glyph cells play at the display rate (R15): their pose is the animation's own
        glyphs(*base, base->pose, false, out.glyphs);
    }
    float weight = 1.0f;
    if (const ta::PlayingAnimation* from = player.fadingFrom(&weight)){
        PoseValues old;
        old.add(doc, *from->animation, from->pose, false, include);
        old.blendToward(out.values, weight);
        out.values = std::move(old);
        std::map<std::string, std::map<uint32_t, GlyphPose>> oldGlyphs;
        glyphs(*from, from->pose, false, oldGlyphs);
        for (auto& [layer, poses] : oldGlyphs){
            std::map<uint32_t, GlyphPose>& now = out.glyphs[layer];
            for (auto& [cluster, a] : poses){
                GlyphPose& b = now[cluster];
                b.cluster = cluster;
                for (int i = 0; i < 2; i++){
                    b.offset[i] = blendValue(a.offset[i], b.offset[i], weight);
                    b.scale[i] = blendValue(a.scale[i], b.scale[i], weight);
                }
                b.rotation = blendValue(a.rotation, b.rotation, weight);
                b.opacity = blendValue(a.opacity, b.opacity, weight);
                for (int i = 0; i < 3; i++) b.tint[i] = blendValue(a.tint[i], b.tint[i], weight);
                b.flashAmount = blendValue(a.flashAmount, b.flashAmount, weight);
            }
        }
    }
    for (const ta::PlayingAnimation& o : player.overlays()){
        ta::TitleAnimPose overlayScratch;
        const ta::TitleAnimPose& pose = sampled(o, overlayScratch, nullptr);
        out.values.add(doc, *o.animation, pose, true, include);
        glyphs(o, o.pose, true, out.glyphs);
    }
    out.any = !out.values.empty() || !out.glyphs.empty();
}
