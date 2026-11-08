// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimOps.h"

#include "TitleAnimPresets.h"

#include "animation/AnimationClipIO.h"
#include "titleanim/TitleAnimDataIO.h"
#include "titleanim/TitleAnimSampling.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/anim/TitleAnimIO.h"
#include "titleart/anim/TitleAnimPlan.h"
#include "titleart/anim/TitleAnimRealtime.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleanim;
namespace tt = jasperengine::titleart;

namespace {

    constexpr double MERGE_TIME = 1e-4;

    std::vector<std::string> namesOf(const ta::TitleAnimationSet& set){
        std::vector<std::string> out;
        for (const ta::TitleAnimation& a : set.animations) out.push_back(a.name);
        return out;
    }

    TitleJson trackJson(const ta::TitleTrack& t){
        return ta::encodeTrack(t);
    }

    // the animation an op names; fails listing the names
    ta::TitleAnimation* animationOf(TitleOpContext& ctx, ta::TitleAnimationSet& set, const TitleJson& op, const char* member = "animation"){
        std::string ref;
        if (!ctx.requireString(op, member, ref)) return nullptr;
        const int index = findTitleAnimation(set, ref);
        if (index < 0){
            const std::vector<std::string> names = namesOf(set);
            ctx.fail("unknown_animation", "no animation '" + ref + "'" + (names.empty() ? " (the title has none: add_animation makes one)" : ""), member,
                     names, tt::nearestName(ref, names));
            return nullptr;
        }
        return &set.animations[(size_t)index];
    }

    // the layer of a track op: `layer` omitted (or "") is the title
    bool trackLayer(TitleOpContext& ctx, const TitleJson& op, std::string& layerId){
        layerId.clear();
        auto it = op.find("layer");
        if (it == op.end() || it->is_null() || (it->is_string() && it->get<std::string>().empty())) return true;
        TitleJson* layer = ctx.layer(op, "layer", true);
        if (!layer) return false;
        layerId = layer->value("id", std::string());
        return true;
    }

    // the field of a track op resolved (shorthands accepted); fails with the alternatives
    bool trackField(TitleOpContext& ctx, const TitleJson& op, const std::string& layerId, tt::TrackFieldInfo& info){
        std::string field;
        if (!ctx.requireString(op, "field", field)) return false;
        info = tt::resolveTrackField(ctx.doc, layerId, field);
        if (!info.ok){
            ctx.fail(info.code.empty() ? "bad_path" : info.code, info.error + (info.validHere.empty() ? "" : " (animatable here: listed)"), "field",
                     info.validHere, info.nearest);
            return false;
        }
        return true;
    }

    bool timeOf(TitleOpContext& ctx, const TitleJson& op, const char* member, double& out){
        auto it = op.find(member);
        if (it == op.end() || !it->is_number() || !std::isfinite(it->get<double>()) || it->get<double>() < 0.0){
            return ctx.fail("bad_value", std::string("`") + member + "` is a time in seconds >= 0", member);
        }
        out = it->get<double>();
        return true;
    }

    bool easeOf(TitleOpContext& ctx, const TitleJson& op, const char* member, EaseType& out){
        auto it = op.find(member);
        if (it == op.end()) return true;
        EaseType e = EaseType::LINEAR;
        if (!it->is_string() || !anim::easeFromName(it->get<std::string>(), e) || e == EaseType::CUSTOM || e == EaseType::CURVE){
            std::vector<std::string> eases;
            for (int i = 0; i <= (int)EaseType::STEP; i++) eases.push_back(anim::easeName((EaseType)i));
            return ctx.fail("bad_value", "unknown ease", member, eases, it->is_string() ? tt::nearestName(it->get<std::string>(), eases) : std::string());
        }
        out = e;
        return true;
    }

    // the checks every track write makes against its field (R2, R4, R7, R15)
    bool checkTrack(TitleOpContext& ctx, const ta::TitleAnimation& animation, const ta::TitleTrack& track, const tt::TrackFieldInfo& info){
        const int conflict = conflictingTitleTrack(animation, track.layer, track.field);
        if (conflict >= 0){
            const ta::TitleTrack& other = animation.tracks[(size_t)conflict];
            return ctx.fail("component_conflict", "'" + track.field + "' and '" + other.field + "' cannot both have tracks: remove_track the one on '" +
                                                      other.field + "' first", "field", {other.field}, other.field,
                            TitleJson{{"ops", TitleJson::array({TitleJson{{"op", "remove_track"}, {"animation", animation.name},
                                                                          {"layer", other.layer}, {"field", other.field}}})}});
        }
        if (track.relative && info.stepped)
            return ctx.fail("relative_stepped", "'" + track.field + "' is stepped: a relative track needs a number, a pair or a colour", "relative");
        if (track.reseed && !(info.seed && info.integer))
            return ctx.fail("reseed_not_seed", "a reseed track drives an integer seed field; '" + track.field + "' is not one", "reseed");
        if (track.drive && info.stepped) return ctx.fail("bad_value", "'" + track.field + "' is stepped: drives need a number, a pair or a colour", "drive");
        if (info.target == ta::TrackTarget::Glyph && info.layer){
            const bool merged = info.layer->value("glyph_mode", std::string("cells")) == "merged";
            const bool color = info.glyphField == ta::GlyphField::Opacity || info.glyphField == ta::GlyphField::Tint ||
                               info.glyphField == ta::GlyphField::FlashColor || info.glyphField == ta::GlyphField::FlashAmount;
            if (merged && color){
                return ctx.fail("glyph_color_needs_cells", "glyph opacity, tint and flash need glyph_mode cells; '" + info.layer->value("name", track.layer) +
                                                               "' is merged", "field", {}, "",
                                TitleJson{{"ops", TitleJson::array({TitleJson{{"op", "set"}, {"layer", "id:" + track.layer}, {"values", {{"glyph_mode", "cells"}}}}})}});
            }
        }
        if (track.stagger && info.target != ta::TrackTarget::Glyph && info.target != ta::TrackTarget::Child)
            return ctx.fail("bad_value", "stagger staggers glyph.* (text layers) and child.* (groups) tracks", "stagger");
        if (track.stagger && track.stagger->unit == ta::StaggerUnit::Child && info.target != ta::TrackTarget::Child)
            return ctx.fail("bad_value", "unit child staggers the children of a group (child.* fields)", "stagger");
        if (track.pivot != ta::TrackPivot::Origin){
            const bool glyph = info.target == ta::TrackTarget::Glyph;
            const bool glyphPivot = track.pivot == ta::TrackPivot::Center || track.pivot == ta::TrackPivot::Baseline || track.pivot == ta::TrackPivot::Bottom ||
                                    track.pivot == ta::TrackPivot::Top;
            if (glyph && !glyphPivot) return ctx.fail("bad_value", "glyph tracks pivot at center, baseline, bottom or top", "pivot", {"center", "baseline", "bottom", "top"});
            if (!glyph && track.pivot == ta::TrackPivot::Baseline)
                return ctx.fail("bad_value", "baseline is a glyph pivot; layer and title tracks pivot at origin, center, bottom, top, left or right", "pivot");
        }
        if (animation.overlay && info.target != ta::TrackTarget::Title)
            ctx.note("overlay_needs_title_live", "an overlay plays only its title-live tracks in Baked mode: '" + track.field + "' needs Realtime or Glyph Atlas there");
        return true;
    }

    // keys of an op (`keys`: the .anim grammar); values checked against the field
    bool keysOf(TitleOpContext& ctx, const TitleJson& op, const tt::TrackFieldInfo& info, std::vector<ta::TitleKey>& out){
        const TitleJson& keys = op["keys"];
        if (!keys.is_array() || keys.empty()) return ctx.fail("bad_value", "`keys` is an array of [time, value] / [time, value, ease] / {time, value, ...}", "keys");
        if (keys.size() > ta::TITLE_ANIM_MAX_KEYS) return ctx.fail("bad_value", "at most " + std::to_string(ta::TITLE_ANIM_MAX_KEYS) + " keys per track", "keys");
        bool eased = false;
        for (size_t i = 0; i < keys.size(); i++){
            ta::TitleKey k;
            std::string error;
            if (!ta::decodeKey(keys[i], k, error)) return ctx.fail("bad_value", "keys[" + std::to_string(i) + "]: " + error, "keys");
            if (info.stepped){
                if (k.stepped.empty()) k.stepped = keys[i].is_array() ? keys[i][1].dump() : keys[i]["value"].dump();
                if (k.key.ease != EaseType::LINEAR && k.key.ease != EaseType::STEP) eased = true;
            }else{
                if (k.key.value.size() == 1 && info.width > 1) k.key.value.assign((size_t)info.width, k.key.value[0]);
                if ((int)k.key.value.size() != info.width){
                    return ctx.fail("bad_value", "keys[" + std::to_string(i) + "]: '" + info.longPath + "' takes " +
                                                     (info.value == ta::TrackValue::Color ? std::string("\"#rrggbb\"") :
                                                      info.width == 2 ? std::string("[x, y]") : std::string("a number")), "keys");
                }
                if (info.value == ta::TrackValue::Color && k.stepped.empty()) k.stepped = TitleJson(ta::formatHexColor(k.key.value.data())).dump();
            }
            out.push_back(std::move(k));
        }
        std::stable_sort(out.begin(), out.end(), [](const ta::TitleKey& a, const ta::TitleKey& b){ return a.key.time < b.key.time; });
        if (eased) ctx.note("ease_dropped", "'" + info.longPath + "' is stepped: its keys' eases are dropped (as .anim does)", "keys");
        return true;
    }

    void commit(TitleOpContext& ctx, const ta::TitleAnimationSet& set){
        tt::writeAnimationSet(ctx.doc, set);
    }

    void changed(TitleOpContext& ctx, const std::string& animation, const std::string& layer, const std::string& field, TitleJson from, TitleJson to){
        TitleOpChange c;
        c.layer = layer;
        c.path = "animation:" + animation + (field.empty() ? std::string() : "/" + field);
        c.from = std::move(from);
        c.to = std::move(to);
        ctx.change(std::move(c));
    }

    std::string trackLabel(const tt::TitleArtDoc& doc, const std::string& layer, const std::string& field){
        const tt::TrackFieldInfo info = tt::resolveTrackField(doc, layer, field);
        std::string label = info.ok && !info.label.empty() ? info.label : field;
        if (!layer.empty())
            if (const TitleJson* l = tt::findLayer(doc.root, layer)) label = l->value("name", layer) + "/" + label;
        return label;
    }

    // ---- the ops ----

    bool applySettings(TitleOpContext& ctx, ta::TitleAnimationSet& set, ta::TitleAnimation& a, const TitleJson& values){
        for (auto it = values.begin(); it != values.end(); ++it){
            const std::string& key = it.key();
            const TitleJson& v = it.value();
            auto number = [&](double lo, double hi, double& out) -> bool{
                if (!v.is_number() || !std::isfinite(v.get<double>())) return ctx.fail("bad_value", "`" + key + "` is a number", key);
                const double x = v.get<double>();
                out = std::min(hi, std::max(lo, x));
                if (out != x) ctx.note("clamped", key + " " + std::to_string(x) + " clamped to " + std::to_string(out), key);
                return true;
            };
            double d = 0.0;
            if (key == "name"){
                if (!v.is_string() || !ta::isAnimationName(v.get<std::string>()))
                    return ctx.fail("bad_value", "a name is 1-32 characters without '/'", key);
                const std::string name = v.get<std::string>();
                if (name != a.name && set.find(name)) return ctx.fail("name_taken", "an animation is named '" + name + "' already", key, namesOf(set));
                // its references follow the rename
                for (ta::TitleAnimation& other : set.animations) if (other.next == a.name) other.next = name;
                if (set.defaultName == a.name) set.defaultName = name;
                a.name = name;
            }else if (key == "role"){
                ta::AnimRole role;
                if (!v.is_string() || !ta::animRoleFromName(v.get<std::string>(), role))
                    return ctx.fail("bad_value", "role is intro, idle, emphasis, win, outro or custom", key, {"intro", "idle", "emphasis", "win", "outro", "custom"});
                a.role = role;
            }else if (key == "duration"){
                if (v.is_number() && v.get<double>() < 0.0){
                    a.duration = (float)ta::TITLE_ANIM_AUTO_DURATION;
                }else{
                    if (!number(ta::TITLE_ANIM_MIN_DURATION, ta::TITLE_ANIM_MAX_DURATION, d)) return false;
                    a.duration = (float)d;
                }
            }else if (key == "wrap"){
                ta::AnimWrap wrap;
                if (!v.is_string() || !ta::animWrapFromName(v.get<std::string>(), wrap)) return ctx.fail("bad_value", "wrap is once, loop or ping_pong", key, {"once", "loop", "ping_pong"});
                a.wrap = wrap;
            }else if (key == "loop_start"){
                if (!number(0.0, ta::TITLE_ANIM_MAX_DURATION, d)) return false;
                a.loopStart = (float)d;
            }else if (key == "next"){
                if (!v.is_string()) return ctx.fail("bad_value", "next is an animation's name, \"\" none", key);
                const std::string next = v.get<std::string>();
                if (!next.empty() && !set.find(next)) return ctx.fail("unknown_animation", "no animation '" + next + "' to play next", key, namesOf(set), tt::nearestName(next, namesOf(set)));
                a.next = next;
            }else if (key == "overlay"){
                if (!v.is_boolean()) return ctx.fail("bad_value", "overlay is true or false", key);
                a.overlay = v.get<bool>();
            }else if (key == "speed"){
                if (!number(0.01, 10.0, d)) return false;
                a.speed = (float)d;
            }else if (key == "rerender"){
                ta::RerenderPolicy p;
                if (!v.is_string() || !ta::rerenderPolicyFromName(v.get<std::string>(), p)) return ctx.fail("bad_value", "rerender is live or prerender", key, {"live", "prerender"});
                a.rerender = p;
            }else if (key == "rerender_fps"){
                if (!number(1.0, 60.0, d)) return false;
                a.rerenderFps = (float)d;
            }else if (key == "flipbook.fps"){
                if (!number(1.0, (double)ta::TITLE_ANIM_MAX_FLIPBOOK_FPS, d)) return false;
                a.flipbook.fps = (float)d;
            }else if (key == "flipbook.scale"){
                if (!number(0.25, 1.0, d)) return false;
                a.flipbook.scale = (float)d;
            }else if (key == "flipbook.max_page"){
                if (!v.is_number_integer() || (v.get<int>() != 1024 && v.get<int>() != 2048 && v.get<int>() != 4096))
                    return ctx.fail("bad_value", "flipbook.max_page is 1024, 2048 or 4096", key, {"1024", "2048", "4096"});
                a.flipbook.maxPage = v.get<int>();
            }else if (key == "flipbook.frame_blend"){
                if (!v.is_boolean()) return ctx.fail("bad_value", "flipbook.frame_blend is true or false", key);
                a.flipbook.frameBlend = v.get<bool>();
            }else{
                const std::vector<std::string> keys = {"name", "role", "duration", "wrap", "loop_start", "next", "overlay", "speed", "rerender",
                                                       "rerender_fps", "flipbook.fps", "flipbook.scale", "flipbook.max_page", "flipbook.frame_blend"};
                return ctx.fail("bad_path", "set_animation has no value '" + key + "'", "values", keys, tt::nearestName(key, keys));
            }
        }
        if (a.duration > 0.0f && a.loopStart >= a.duration)
            return ctx.fail("bad_value", "loop_start must be before the duration", "values",
                            {}, "", TitleJson{{"ops", TitleJson::array({TitleJson{{"op", "set_animation"}, {"animation", a.name}, {"values", {{"loop_start", 0}}}}})}});
        return true;
    }

    bool addAnimation(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        std::string name;
        if (!ctx.requireString(op, "name", name)) return false;
        if (!ta::isAnimationName(name)) return ctx.fail("bad_value", "a name is 1-32 characters without '/'", "name");
        if (set.find(name)) return ctx.fail("name_taken", "an animation is named '" + name + "' already", "name", namesOf(set));
        if (set.animations.size() >= ta::TITLE_ANIM_MAX_ANIMATIONS)
            return ctx.fail("limit", "a title holds at most " + std::to_string(ta::TITLE_ANIM_MAX_ANIMATIONS) + " animations", "name");
        ta::TitleAnimation a;
        a.id = ctx.newId();
        a.name = name;
        set.animations.push_back(a);
        TitleJson values = TitleJson::object();
        for (const char* key : {"role", "duration", "wrap", "loop_start", "next", "overlay"}) if (op.contains(key)) values[key] = op[key];
        if (!applySettings(ctx, set, set.animations.back(), values)) return false;
        if (set.defaultName.empty() && !set.animations.back().overlay) set.defaultName = name;
        commit(ctx, set);
        ctx.recordNewId("animation", a.id);
        changed(ctx, name, "", "", nullptr, ta::encodeAnimation(set.animations.back()));
        // with a preset, the animation is made from it (R22) in the same step
        if (op.contains("preset")){
            TitleJson apply = {{"op", "apply_animation_preset"}, {"animation", name}, {"preset", op["preset"]}};
            for (const char* key : {"params", "layers"}) if (op.contains(key)) apply[key] = op[key];
            if (!applyTitleAnimationPresetOp(ctx, apply)) return false;
        }
        ctx.undoName = "Add Animation " + name;
        return true;
    }

    bool setAnimation(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        ta::TitleAnimation* a = animationOf(ctx, set, op);
        if (!a) return false;
        if (!op.contains("values") || !op["values"].is_object()) return ctx.fail("bad_value", "`values` is an object of the settings to change", "values");
        const TitleJson before = ta::encodeAnimation(*a);
        const std::string name = a->name;
        if (!applySettings(ctx, set, *a, op["values"])) return false;
        commit(ctx, set);
        changed(ctx, name, "", "", before, ta::encodeAnimation(*a));
        ctx.undoName = "Set Animation " + a->name;
        return true;
    }

    bool removeAnimation(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        ta::TitleAnimation* a = animationOf(ctx, set, op);
        if (!a) return false;
        const std::string name = a->name;
        const TitleJson before = ta::encodeAnimation(*a);
        set.animations.erase(set.animations.begin() + (a - set.animations.data()));
        for (ta::TitleAnimation& other : set.animations){
            if (other.next == name){
                other.next.clear();
                ctx.note("next_cleared", "'" + other.name + "' no longer plays '" + name + "' next");
            }
        }
        if (set.defaultName == name) set.defaultName.clear();
        commit(ctx, set);
        changed(ctx, name, "", "", before, nullptr);
        ctx.undoName = "Remove Animation " + name;
        return true;
    }

    bool duplicateAnimation(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        ta::TitleAnimation* a = animationOf(ctx, set, op);
        if (!a) return false;
        if (set.animations.size() >= ta::TITLE_ANIM_MAX_ANIMATIONS)
            return ctx.fail("limit", "a title holds at most " + std::to_string(ta::TITLE_ANIM_MAX_ANIMATIONS) + " animations", "animation");
        ta::TitleAnimation copy = *a;
        std::string name = op.value("name", std::string());
        if (name.empty()) name = uniqueTitleAnimationName(set, a->name + " Copy");
        if (!ta::isAnimationName(name)) return ctx.fail("bad_value", "a name is 1-32 characters without '/'", "name");
        if (set.find(name)) return ctx.fail("name_taken", "an animation is named '" + name + "' already", "name", namesOf(set));
        copy.name = name;
        copy.id = ctx.newId();
        set.animations.push_back(copy);
        commit(ctx, set);
        ctx.recordNewId("animation", copy.id);
        changed(ctx, name, "", "", nullptr, ta::encodeAnimation(copy));
        ctx.undoName = "Duplicate Animation " + a->name;
        return true;
    }

    bool setDefaultAnimation(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        std::string ref;
        if (!ctx.requireString(op, "animation", ref)) return false;
        const std::string before = set.defaultName;
        if (ref.empty()){
            set.defaultName.clear();
        }else{
            const int index = findTitleAnimation(set, ref);
            if (index < 0) return ctx.fail("unknown_animation", "no animation '" + ref + "'", "animation", namesOf(set), tt::nearestName(ref, namesOf(set)));
            set.defaultName = set.animations[(size_t)index].name;
        }
        commit(ctx, set);
        TitleOpChange c;
        c.path = "animation.default";
        c.from = before;
        c.to = set.defaultName;
        ctx.change(std::move(c));
        ctx.undoName = set.defaultName.empty() ? "Clear Default Animation" : "Set Default Animation " + set.defaultName;
        return true;
    }

    bool staggerOf(TitleOpContext& ctx, const TitleJson& op, std::optional<ta::TitleStagger>& out){
        auto it = op.find("stagger");
        if (it == op.end()) return true;
        if (it->is_null()){
            out.reset();
            return true;
        }
        std::vector<ta::TitleAnimWarning> w;
        ta::TitleStagger s;
        if (!ta::decodeStagger(*it, "stagger", s, &w)) return ctx.fail("bad_value", "stagger is {each | over, order, unit, seed, ease}", "stagger");
        if (!w.empty()) return ctx.fail("bad_value", w[0].toString(), "stagger");
        out = s;
        return true;
    }

    bool setTrack(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        ta::TitleAnimation* a = animationOf(ctx, set, op);
        if (!a) return false;
        std::string layerId;
        if (!trackLayer(ctx, op, layerId)) return false;
        tt::TrackFieldInfo info;
        if (!trackField(ctx, op, layerId, info)) return false;
        const int sources = (int)op.contains("keys") + (int)op.contains("drive") + (int)op.contains("reseed");
        if (sources > 1) return ctx.fail("bad_value", "a track holds exactly one of keys, drive and reseed", "keys");
        const int existing = findTitleTrack(*a, layerId, info.longPath);
        if (existing < 0 && sources == 0) return ctx.fail("bad_value", "a new track needs keys, a drive or a reseed", "keys");
        if (existing < 0 && a->tracks.size() >= ta::TITLE_ANIM_MAX_TRACKS)
            return ctx.fail("limit", "an animation holds at most " + std::to_string(ta::TITLE_ANIM_MAX_TRACKS) + " tracks", "field");
        ta::TitleTrack track = existing >= 0 ? a->tracks[(size_t)existing] : ta::TitleTrack();
        const TitleJson before = existing >= 0 ? trackJson(track) : TitleJson();
        track.layer = layerId;
        track.field = info.longPath;
        track.orphan = false;
        if (op.contains("relative")){
            if (!op["relative"].is_boolean()) return ctx.fail("bad_value", "relative is true or false", "relative");
            track.relative = op["relative"].get<bool>();
        }
        if (op.contains("mute")){
            if (!op["mute"].is_boolean()) return ctx.fail("bad_value", "mute is true or false", "mute");
            track.mute = op["mute"].get<bool>();
        }
        if (op.contains("pivot")){
            ta::TrackPivot p;
            if (!op["pivot"].is_string() || !ta::trackPivotFromName(op["pivot"].get<std::string>(), p))
                return ctx.fail("bad_value", "pivot is origin, center, bottom, top, left, right or baseline", "pivot",
                                {"origin", "center", "bottom", "top", "left", "right", "baseline"});
            track.pivot = p;
        }else if (existing < 0 && info.target == ta::TrackTarget::Glyph){
            track.pivot = ta::TrackPivot::Center;
        }
        if (!staggerOf(ctx, op, track.stagger)) return false;
        if (op.contains("from_preset") && op["from_preset"].is_string()) track.fromPreset = op["from_preset"].get<std::string>();
        else if (sources > 0) track.fromPreset.clear();    // an edit makes it the user's
        if (op.contains("keys")){
            std::vector<ta::TitleKey> keys;
            if (!keysOf(ctx, op, info, keys)) return false;
            const std::string mode = op.value("mode", std::string("replace"));
            if (mode != "replace" && mode != "merge") return ctx.fail("bad_value", "mode is replace or merge", "mode", {"replace", "merge"});
            if (mode == "merge" && !track.keys.empty()){
                for (ta::TitleKey& k : keys){
                    auto same = std::find_if(track.keys.begin(), track.keys.end(), [&](const ta::TitleKey& x){ return std::fabs(x.key.time - k.key.time) <= MERGE_TIME; });
                    if (same != track.keys.end()) *same = k;
                    else track.keys.push_back(k);
                }
                std::stable_sort(track.keys.begin(), track.keys.end(), [](const ta::TitleKey& x, const ta::TitleKey& y){ return x.key.time < y.key.time; });
            }else{
                track.keys = std::move(keys);
            }
            track.drive.reset();
            track.reseed.reset();
        }else if (op.contains("drive")){
            std::vector<ta::TitleAnimWarning> w;
            ta::TitleDrive d;
            if (!ta::decodeDrive(op["drive"], "drive", d, &w)) return ctx.fail("bad_value", w.empty() ? "drive is {from, to, cycles | rate, ease, wrap}" : w[0].toString(), "drive");
            if ((int)d.from.size() != info.width){
                if (d.from.size() == 1){
                    d.from.assign((size_t)info.width, d.from[0]);
                    d.to.assign((size_t)info.width, d.to.empty() ? d.from[0] : d.to[0]);
                }else{
                    return ctx.fail("bad_value", "'" + info.longPath + "' drives " + std::to_string(info.width) + " value(s)", "drive");
                }
            }
            track.drive = d;
            track.keys.clear();
            track.reseed.reset();
        }else if (op.contains("reseed")){
            const TitleJson& r = op["reseed"];
            if (!r.is_object()) return ctx.fail("bad_value", "reseed is {rate, offset}", "reseed");
            ta::TitleReseed reseed;
            reseed.rate = (float)std::min(240.0, std::max(0.0, r.value("rate", 12.0)));
            reseed.offset = (float)r.value("offset", 0.0);
            track.reseed = reseed;
            track.keys.clear();
            track.drive.reset();
        }
        if (existing >= 0) a->tracks.erase(a->tracks.begin() + existing);
        if (!checkTrack(ctx, *a, track, info)) return false;
        a->tracks.push_back(track);
        commit(ctx, set);
        changed(ctx, a->name, layerId, info.longPath, before, trackJson(track));
        ctx.undoName = std::string(existing >= 0 ? "Set Track " : "Add Track ") + trackLabel(ctx.doc, layerId, info.longPath);
        return true;
    }

    bool setKey(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        ta::TitleAnimation* a = animationOf(ctx, set, op);
        if (!a) return false;
        std::string layerId;
        if (!trackLayer(ctx, op, layerId)) return false;
        tt::TrackFieldInfo info;
        if (!trackField(ctx, op, layerId, info)) return false;
        double time = 0.0;
        if (!timeOf(ctx, op, "time", time)) return false;
        const int existing = findTitleTrack(*a, layerId, info.longPath);
        if (existing >= 0 && (a->tracks[(size_t)existing].drive || a->tracks[(size_t)existing].reseed))
            return ctx.fail("bad_value", "the track on '" + info.longPath + "' is a drive or a reseed: set_track replaces it with keys", "field");
        ta::TitleTrack track = existing >= 0 ? a->tracks[(size_t)existing] : ta::TitleTrack();
        const TitleJson before = existing >= 0 ? trackJson(track) : TitleJson();
        track.layer = layerId;
        track.field = info.longPath;
        if (existing < 0 && info.target == ta::TrackTarget::Glyph) track.pivot = ta::TrackPivot::Center;
        // the value: given, else the sampled one (R42's double-click)
        // (a relative track samples its raw value: what its keys hold at that time)
        const TitleJson value = op.contains("value") ? op["value"] : sampleTitleTrackValue(ctx.doc, *a, layerId, info.longPath, time);
        ta::TitleKey key;
        std::string error;
        if (!titleKeyFromValue(value, info.width, info.stepped, info.value == ta::TrackValue::Color, key, error)) return ctx.fail("bad_value", error, "value");
        key.key.time = (float)time;
        if (!easeOf(ctx, op, "ease", key.key.ease)) return false;
        auto tangent = [&](const char* member, std::vector<float>& out) -> bool{
            auto it = op.find(member);
            if (it == op.end()) return true;
            float v[3];
            size_t w = 0;
            if (!ta::numericFromJson(*it, v, w) || it->is_string()) return ctx.fail("bad_value", std::string("`") + member + "` is a number or an array of numbers", member);
            out.assign(v, v + w);
            if (out.size() == 1 && info.width > 1) out.assign((size_t)info.width, out[0]);
            return true;
        };
        if (!tangent("in", key.key.inTangent) || !tangent("out", key.key.outTangent)) return false;
        if (key.key.inTangent.size() != key.key.outTangent.size()){
            if (key.key.inTangent.empty()) key.key.inTangent = key.key.outTangent;
            else key.key.outTangent = key.key.inTangent;
        }
        if (op.contains("tangent_mode")){
            ClipTangentMode m;
            if (!op["tangent_mode"].is_string() || !anim::tangentModeFromName(op["tangent_mode"].get<std::string>(), m))
                return ctx.fail("bad_value", "tangent_mode is clamped_auto, auto, free, linear, constant, flat or broken", "tangent_mode",
                                {"clamped_auto", "auto", "free", "linear", "constant", "flat", "broken"});
            key.key.tangentMode = m;
        }
        auto same = std::find_if(track.keys.begin(), track.keys.end(), [&](const ta::TitleKey& x){ return std::fabs(x.key.time - time) <= MERGE_TIME; });
        if (same != track.keys.end()) *same = key;
        else track.keys.push_back(key);
        std::stable_sort(track.keys.begin(), track.keys.end(), [](const ta::TitleKey& x, const ta::TitleKey& y){ return x.key.time < y.key.time; });
        track.fromPreset.clear();
        if (existing >= 0) a->tracks.erase(a->tracks.begin() + existing);
        if (!checkTrack(ctx, *a, track, info)) return false;
        a->tracks.push_back(track);
        commit(ctx, set);
        changed(ctx, a->name, layerId, info.longPath, before, trackJson(track));
        ctx.undoName = "Set Key " + trackLabel(ctx.doc, layerId, info.longPath);
        return true;
    }

    // a track an op names (layer?, field), with its index; fails listing the animation's tracks
    int namedTrack(TitleOpContext& ctx, const ta::TitleAnimation& a, const TitleJson& op, std::string& layerId){
        if (!trackLayer(ctx, op, layerId)) return -1;
        std::string field;
        if (!ctx.requireString(op, "field", field)) return -1;
        // the long form of a shorthand when it still resolves (a gone layer's track by its stored field)
        const tt::TrackFieldInfo info = tt::resolveTrackField(ctx.doc, layerId, field);
        const int index = findTitleTrack(a, layerId, info.ok ? info.longPath : field);
        if (index < 0){
            std::vector<std::string> fields;
            for (const ta::TitleTrack& t : a.tracks) if (t.layer == layerId) fields.push_back(t.field);
            ctx.fail("unknown_track", "'" + a.name + "' has no track on '" + field + "'" + (layerId.empty() ? " of the title" : ""), "field", fields,
                     tt::nearestName(field, fields));
        }
        return index;
    }

    bool removeTrack(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        ta::TitleAnimation* a = animationOf(ctx, set, op);
        if (!a) return false;
        std::string layerId;
        const int index = namedTrack(ctx, *a, op, layerId);
        if (index < 0) return false;
        const ta::TitleTrack removed = a->tracks[(size_t)index];
        a->tracks.erase(a->tracks.begin() + index);
        commit(ctx, set);
        changed(ctx, a->name, layerId, removed.field, trackJson(removed), nullptr);
        ctx.undoName = "Remove Track " + trackLabel(ctx.doc, layerId, removed.field);
        return true;
    }

    // the tracks an op touches: one (layer? field), a layer's, or all
    std::vector<size_t> touchedTracks(TitleOpContext& ctx, ta::TitleAnimation& a, const TitleJson& op, bool& failed){
        failed = false;
        std::vector<size_t> out;
        if (op.contains("field")){
            std::string layerId;
            const int index = namedTrack(ctx, a, op, layerId);
            if (index < 0){
                failed = true;
                return out;
            }
            out.push_back((size_t)index);
            return out;
        }
        std::string layerId;
        const bool byLayer = op.contains("layer") && !(op["layer"].is_string() && op["layer"].get<std::string>().empty());
        if (byLayer && !trackLayer(ctx, op, layerId)){
            failed = true;
            return out;
        }
        for (size_t i = 0; i < a.tracks.size(); i++) if (!byLayer || a.tracks[i].layer == layerId) out.push_back(i);
        return out;
    }

    bool rangeOf(TitleOpContext& ctx, const TitleJson& op, double& a, double& b, bool& given){
        given = false;
        a = -1e30;
        b = 1e30;
        auto it = op.find("range");
        if (it == op.end()) return true;
        if (!it->is_array() || it->size() != 2 || !(*it)[0].is_number() || !(*it)[1].is_number())
            return ctx.fail("bad_value", "range is [from, to] in seconds", "range");
        a = std::min((*it)[0].get<double>(), (*it)[1].get<double>());
        b = std::max((*it)[0].get<double>(), (*it)[1].get<double>());
        given = true;
        return true;
    }

    bool removeKeys(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        ta::TitleAnimation* a = animationOf(ctx, set, op);
        if (!a) return false;
        bool failed = false;
        const std::vector<size_t> tracks = touchedTracks(ctx, *a, op, failed);
        if (failed) return false;
        double from, to;
        bool ranged = false;
        if (!rangeOf(ctx, op, from, to, ranged)) return false;
        std::vector<double> times;
        if (op.contains("times")){
            if (!op["times"].is_array()) return ctx.fail("bad_value", "times is an array of seconds", "times");
            for (const TitleJson& t : op["times"]) if (t.is_number()) times.push_back(t.get<double>());
        }else if (!ranged){
            return ctx.fail("bad_value", "remove_keys takes `times` or `range`", "times");
        }
        size_t removed = 0;
        std::vector<size_t> emptied;
        for (size_t i : tracks){
            ta::TitleTrack& t = a->tracks[i];
            const size_t before = t.keys.size();
            t.keys.erase(std::remove_if(t.keys.begin(), t.keys.end(), [&](const ta::TitleKey& k){
                if (ranged && k.key.time >= from - MERGE_TIME && k.key.time <= to + MERGE_TIME) return true;
                for (double x : times) if (std::fabs(k.key.time - x) <= MERGE_TIME) return true;
                return false;
            }), t.keys.end());
            removed += before - t.keys.size();
            if (before > 0 && t.keys.empty() && !t.drive && !t.reseed) emptied.push_back(i);
        }
        // a track without keys goes
        for (size_t k = emptied.size(); k-- > 0;) a->tracks.erase(a->tracks.begin() + (std::ptrdiff_t)emptied[k]);
        commit(ctx, set);
        changed(ctx, a->name, "", "", nullptr, TitleJson{{"keys_removed", removed}, {"tracks_removed", emptied.size()}});
        ctx.undoName = "Delete Keys";
        return true;
    }

    bool retime(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        ta::TitleAnimation* a = animationOf(ctx, set, op);
        if (!a) return false;
        bool failed = false;
        const std::vector<size_t> tracks = touchedTracks(ctx, *a, op, failed);
        if (failed) return false;
        double from, to;
        bool ranged = false;
        if (!rangeOf(ctx, op, from, to, ranged)) return false;
        const double shift = op.value("shift", 0.0);
        const double scale = op.value("scale", 1.0);
        if (!std::isfinite(shift) || !std::isfinite(scale) || scale <= 0.0) return ctx.fail("bad_value", "shift is seconds, scale a factor > 0", "scale");
        const double pivot = op.contains("pivot_time") ? op.value("pivot_time", 0.0) : (ranged ? from : 0.0);
        auto map = [&](double t){ return std::max(0.0, pivot + (t - pivot) * scale + shift); };
        size_t moved = 0;
        for (size_t i : tracks){
            ta::TitleTrack& t = a->tracks[i];
            for (ta::TitleKey& k : t.keys){
                if (ranged && (k.key.time < from - MERGE_TIME || k.key.time > to + MERGE_TIME)) continue;
                k.key.time = (float)map(k.key.time);
                // tangents are per second: a time scale divides them
                for (float& x : k.key.inTangent) x = (float)(x / scale);
                for (float& x : k.key.outTangent) x = (float)(x / scale);
                moved++;
            }
            std::stable_sort(t.keys.begin(), t.keys.end(), [](const ta::TitleKey& x, const ta::TitleKey& y){ return x.key.time < y.key.time; });
        }
        // markers move with the keys unless the op names a track or a layer
        if (!op.contains("field") && !op.contains("layer")){
            for (ta::TitleMarker& m : a->markers){
                if (ranged && (m.time < from - MERGE_TIME || m.time > to + MERGE_TIME)) continue;
                m.time = (float)map(m.time);
                moved++;
            }
            ta::sortMarkers(a->markers);
            if (a->duration > 0.0f && !ranged){
                a->duration = (float)std::min(ta::TITLE_ANIM_MAX_DURATION, std::max(ta::TITLE_ANIM_MIN_DURATION, map(a->duration)));
                a->loopStart = (float)std::min((double)a->duration * 0.999, map(a->loopStart));
            }
        }
        commit(ctx, set);
        changed(ctx, a->name, "", "", nullptr, TitleJson{{"moved", moved}, {"shift", shift}, {"scale", scale}});
        ctx.undoName = scale != 1.0 ? "Scale Keys" : "Move Keys";
        return true;
    }

    bool setMarker(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        ta::TitleAnimation* a = animationOf(ctx, set, op);
        if (!a) return false;
        std::string name;
        if (!ctx.requireString(op, "name", name)) return false;
        if (!anim::isLuaIdentifier(name)) return ctx.fail("bad_value", "a marker's name is a Lua identifier (onImpact, beat_1)", "name");
        double time = 0.0;
        if (!timeOf(ctx, op, "time", time)) return false;
        // `index` addresses the markers of that name (0 the first); one past the last adds one
        std::vector<size_t> same;
        for (size_t i = 0; i < a->markers.size(); i++) if (a->markers[i].name == name) same.push_back(i);
        int target = -1;
        if (op.contains("index")){
            if (!op["index"].is_number_integer() || op["index"].get<int>() < 0 || (size_t)op["index"].get<int>() > same.size())
                return ctx.fail("bad_value", "index is 0.." + std::to_string(same.size()) + " among the markers named '" + name + "'", "index");
            const size_t i = (size_t)op["index"].get<int>();
            if (i < same.size()) target = (int)same[i];
        }else if (!same.empty()){
            target = (int)same[0];
        }
        if (target < 0 && a->markers.size() >= ta::TITLE_ANIM_MAX_MARKERS)
            return ctx.fail("limit", "an animation holds at most " + std::to_string(ta::TITLE_ANIM_MAX_MARKERS) + " markers", "name");
        ta::TitleMarker m = target >= 0 ? a->markers[(size_t)target] : ta::TitleMarker();
        m.name = name;
        m.time = (float)time;
        TitleJson marker = ta::encodeMarker(m);
        for (const char* key : {"args", "sound", "volume_db", "group"}) if (op.contains(key)) marker[key] = op[key];
        std::vector<ta::TitleAnimWarning> w;
        ta::TitleMarker decoded;
        if (!ta::decodeMarker(marker, "marker", decoded, &w) || !w.empty()) return ctx.fail("bad_value", w.empty() ? "not a marker" : w[0].toString(), "args");
        if (!decoded.sound.empty()){
            const std::string& p = decoded.sound.path;
            const size_t dot = p.find_last_of('.');
            const std::string ext = dot == std::string::npos ? std::string() : p.substr(dot);
            if (ext != ".wav" && ext != ".ogg" && ext != ".mp3" && ext != ".flac" && ext != ".soundevent")
                return ctx.fail("bad_value", "a marker's sound is an audio clip (.wav .ogg .mp3 .flac) or a .soundevent", "sound");
        }
        if (target >= 0) a->markers[(size_t)target] = decoded;
        else a->markers.push_back(decoded);
        ta::sortMarkers(a->markers);
        if (a->duration > 0.0f && time > a->duration) ctx.note("marker_after_end", "marker '" + name + "' is after the animation's end and never fires", "time");
        commit(ctx, set);
        changed(ctx, a->name, "", "marker:" + name, nullptr, ta::encodeMarker(decoded));
        ctx.undoName = std::string(target >= 0 ? "Set Marker " : "Add Marker ") + name;
        return true;
    }

    bool removeMarker(TitleOpContext& ctx, const TitleJson& op){
        ta::TitleAnimationSet set = tt::readAnimationSet(ctx.doc);
        ta::TitleAnimation* a = animationOf(ctx, set, op);
        if (!a) return false;
        const bool hasName = op.contains("name") && op["name"].is_string();
        const bool hasIndex = op.contains("index") && op["index"].is_number_integer();
        if (!hasName && !hasIndex) return ctx.fail("bad_value", "remove_marker takes `name` or `index`", "name");
        std::vector<std::string> names;
        for (const ta::TitleMarker& m : a->markers) names.push_back(m.name);
        size_t removed = 0;
        if (hasName){
            const std::string name = op["name"].get<std::string>();
            std::vector<size_t> same;
            for (size_t i = 0; i < a->markers.size(); i++) if (a->markers[i].name == name) same.push_back(i);
            if (same.empty()) return ctx.fail("unknown_marker", "no marker named '" + name + "'", "name", names, tt::nearestName(name, names));
            if (hasIndex){
                const int i = op["index"].get<int>();
                if (i < 0 || (size_t)i >= same.size()) return ctx.fail("bad_value", "index is 0.." + std::to_string(same.size() - 1), "index");
                a->markers.erase(a->markers.begin() + (std::ptrdiff_t)same[(size_t)i]);
                removed = 1;
            }else{
                for (size_t k = same.size(); k-- > 0;) a->markers.erase(a->markers.begin() + (std::ptrdiff_t)same[k]);
                removed = same.size();
            }
        }else{
            const int i = op["index"].get<int>();
            if (i < 0 || (size_t)i >= a->markers.size()) return ctx.fail("bad_value", "index is 0.." + std::to_string((int)a->markers.size() - 1), "index", names);
            a->markers.erase(a->markers.begin() + i);
            removed = 1;
        }
        commit(ctx, set);
        changed(ctx, a->name, "", "markers", nullptr, TitleJson{{"removed", removed}});
        ctx.undoName = removed > 1 ? "Remove Markers" : "Remove Marker";
        return true;
    }

}

// ==================================================================== helpers

int editor::findTitleAnimation(const ta::TitleAnimationSet& set, const std::string& ref){
    const bool byId = ref.rfind("id:", 0) == 0;
    for (size_t i = 0; i < set.animations.size(); i++){
        if (byId ? set.animations[i].id == ref.substr(3) : set.animations[i].name == ref) return (int)i;
    }
    return -1;
}

int editor::findTitleTrack(const ta::TitleAnimation& animation, const std::string& layer, const std::string& field){
    for (size_t i = 0; i < animation.tracks.size(); i++)
        if (animation.tracks[i].layer == layer && animation.tracks[i].field == field) return (int)i;
    return -1;
}

int editor::conflictingTitleTrack(const ta::TitleAnimation& animation, const std::string& layer, const std::string& field){
    for (size_t i = 0; i < animation.tracks.size(); i++){
        const ta::TitleTrack& t = animation.tracks[i];
        if (t.layer != layer || t.field == field) continue;
        const std::string& a = t.field;
        const bool contains = (field.size() > a.size() && field.compare(0, a.size(), a) == 0 && field[a.size()] == '.') ||
                              (a.size() > field.size() && a.compare(0, field.size(), field) == 0 && a[field.size()] == '.');
        if (contains) return (int)i;
    }
    return -1;
}

std::string editor::uniqueTitleAnimationName(const ta::TitleAnimationSet& set, const std::string& name){
    std::string base = name.substr(0, ta::TITLE_ANIM_MAX_NAME - 3);
    if (!set.find(base)) return base;
    for (int i = 2; i < 1000; i++){
        const std::string candidate = base + " " + std::to_string(i);
        if (!set.find(candidate)) return candidate;
    }
    return base;
}

TitleJson editor::sampleTitleTrackValue(const tt::TitleArtDoc& doc, const ta::TitleAnimation& animation, const std::string& layer,
                                        const std::string& field, double time){
    const tt::TrackFieldInfo info = tt::resolveTrackField(doc, layer, field);
    if (!info.ok) return nullptr;
    const int index = findTitleTrack(animation, layer, info.longPath);
    if (index < 0) return info.rest;
    ta::TitleAnimation one = animation;
    one.tracks = {animation.tracks[(size_t)index]};
    one.tracks[0].relative = false;     // the raw track value: what its keys hold at that time
    const ta::CompiledTitleAnimation c = tt::compileAnimation(doc, one, nullptr, nullptr);
    if (c.tracks.empty() || c.tracks[0].orphan) return info.rest;
    ta::TitleAnimPose pose;
    pose.size(c);
    ta::sampleAnimation(c, std::max(0.0, time), pose);
    const ta::CompiledTitleTrack& t = c.tracks[0];
    const double* v = pose.values.data() + t.poseOffset;
    if (t.value == ta::TrackValue::Stepped){
        const size_t k = (size_t)std::max(0.0, v[0]);
        return k < t.stepValues.size() ? TitleJson::parse(t.stepValues[k], nullptr, false) : info.rest;
    }
    if (t.value == ta::TrackValue::Color){
        float rgb[3] = {(float)v[0], (float)v[1], (float)v[2]};
        return ta::formatHexColor(rgb);
    }
    if (t.width == 1) return ta::roundToFloat(v[0]);
    TitleJson a = TitleJson::array();
    for (size_t i = 0; i < t.width; i++) a.push_back(ta::roundToFloat(v[i]));
    return a;
}

bool editor::titleKeyFromValue(const TitleJson& value, int width, bool stepped, bool color, ta::TitleKey& out, std::string& error){
    out = ta::TitleKey();
    if (stepped){
        if (value.is_discarded()){
            error = "expected a value";
            return false;
        }
        out.stepped = value.dump();
        float v[3];
        size_t w = 0;
        if (ta::numericFromJson(value, v, w) && !value.is_string()) out.key.value.assign(v, v + w);
        return true;
    }
    float v[3];
    size_t w = 0;
    if (!ta::numericFromJson(value, v, w)){
        error = color ? "expected \"#rrggbb\"" : (width == 2 ? "expected [x, y]" : "expected a number");
        return false;
    }
    if (w == 1 && width > 1){
        for (int i = 1; i < width; i++) v[i] = v[0];
        w = (size_t)width;
    }
    if ((int)w != width){
        error = color ? "expected \"#rrggbb\"" : (width == 2 ? "expected [x, y]" : "expected a number");
        return false;
    }
    out.key.value.assign(v, v + w);
    if (color) out.stepped = TitleJson(ta::formatHexColor(v)).dump();
    return true;
}

void editor::registerTitleAnimOps(){
    static bool done = false;
    if (done) return;
    done = true;
    TitleArtOps::registerOp("add_animation", addAnimation);
    TitleArtOps::registerOp("set_animation", setAnimation);
    TitleArtOps::registerOp("remove_animation", removeAnimation);
    TitleArtOps::registerOp("duplicate_animation", duplicateAnimation);
    TitleArtOps::registerOp("set_default_animation", setDefaultAnimation);
    TitleArtOps::registerOp("set_track", setTrack);
    TitleArtOps::registerOp("set_key", setKey);
    TitleArtOps::registerOp("remove_track", removeTrack);
    TitleArtOps::registerOp("remove_keys", removeKeys);
    TitleArtOps::registerOp("retime", retime);
    TitleArtOps::registerOp("set_marker", setMarker);
    TitleArtOps::registerOp("remove_marker", removeMarker);
}
