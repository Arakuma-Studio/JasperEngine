// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleAnimDataIO.h"

#include "animation/AnimationClipIO.h"
#include "io/Data.h"
#include "titleanim/TitleAnimSampling.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace jasperengine;
using namespace jasperengine::titleanim;

namespace {

    void warn(std::vector<TitleAnimWarning>* warnings, const std::string& path, const std::string& message){
        if (warnings) warnings->push_back({path, message});
    }

    std::string member(const std::string& path, const char* name){
        return path.empty() ? std::string(name) : path + "." + name;
    }

    std::string index(const std::string& path, size_t i){
        return path + "[" + std::to_string(i) + "]";
    }

    std::string shortNumber(double v){
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.7g", v);
        return buf;
    }

    // the members of `object` other than `known`, as an object's text ("" when none)
    std::string unknownOf(const AnimJson& object, std::initializer_list<const char*> known){
        AnimJson extra = AnimJson::object();
        for (auto it = object.begin(); it != object.end(); ++it){
            bool listed = false;
            for (const char* k : known) if (it.key() == k){ listed = true; break; }
            if (!listed) extra[it.key()] = it.value();
        }
        return extra.empty() ? std::string() : extra.dump();
    }

    void appendUnknown(AnimJson& object, const std::string& unknown){
        if (unknown.empty()) return;
        AnimJson extra = AnimJson::parse(unknown, nullptr, false);
        if (!extra.is_object()) return;
        for (auto it = extra.begin(); it != extra.end(); ++it)
            if (!object.contains(it.key())) object[it.key()] = it.value();
    }

    // typed reads: a wrong type takes the default with a warning
    double readNumber(const AnimJson& object, const char* key, double fallback, double lo, double hi, const std::string& path,
                      std::vector<TitleAnimWarning>* warnings){
        auto it = object.find(key);
        if (it == object.end()) return fallback;
        if (!it->is_number() || !std::isfinite(it->get<double>())){
            warn(warnings, member(path, key), "expected a number, using " + shortNumber(fallback));
            return fallback;
        }
        const double v = it->get<double>();
        if (v < lo || v > hi){
            const double c = std::min(hi, std::max(lo, v));
            warn(warnings, member(path, key), shortNumber(v) + " clamped to " + shortNumber(c));
            return c;
        }
        return v;
    }

    bool readBool(const AnimJson& object, const char* key, bool fallback, const std::string& path, std::vector<TitleAnimWarning>* warnings){
        auto it = object.find(key);
        if (it == object.end()) return fallback;
        if (!it->is_boolean()){
            warn(warnings, member(path, key), std::string("expected true or false, using ") + (fallback ? "true" : "false"));
            return fallback;
        }
        return it->get<bool>();
    }

    std::string readString(const AnimJson& object, const char* key, const std::string& fallback, const std::string& path,
                           std::vector<TitleAnimWarning>* warnings){
        auto it = object.find(key);
        if (it == object.end()) return fallback;
        if (!it->is_string()){
            warn(warnings, member(path, key), "expected a string, using \"" + fallback + "\"");
            return fallback;
        }
        return it->get<std::string>();
    }

    template<typename E>
    E readEnum(const AnimJson& object, const char* key, E fallback, bool (*fromName)(std::string_view, E&), const char* (*name)(E),
               const std::string& path, std::vector<TitleAnimWarning>* warnings){
        auto it = object.find(key);
        if (it == object.end()) return fallback;
        E out = fallback;
        if (!it->is_string() || !fromName(it->get<std::string>(), out)){
            warn(warnings, member(path, key), std::string("unknown value, using \"") + name(fallback) + "\"");
            return fallback;
        }
        return out;
    }

    bool easeFrom(const AnimJson& value, EaseType& out){
        return value.is_string() && anim::easeFromName(value.get<std::string>(), out) && out != EaseType::CUSTOM && out != EaseType::CURVE;
    }

    AnimJson floatArray(const std::vector<float>& values){
        AnimJson a = AnimJson::array();
        for (float v : values) a.push_back(roundToFloat(v));
        return a;
    }

    AnimJson assetRefJson(const AssetRef& ref){
        if (ref.empty()) return nullptr;
        return AnimJson{{"path", ref.path}, {"guid", ref.guid.str()}};
    }

    bool assetRefFrom(const AnimJson& value, AssetRef& out){
        out = AssetRef();
        if (value.is_null()) return true;
        if (value.is_string()){
            out.path = value.get<std::string>();
            return true;
        }
        if (!value.is_object()) return false;
        if (auto p = value.find("path"); p != value.end() && p->is_string()) out.path = p->get<std::string>();
        if (auto g = value.find("guid"); g != value.end() && g->is_string()) out.guid = Guid::parse(g->get<std::string>());
        return true;
    }

    size_t utf8Count(std::string_view text){
        size_t n = 0;
        for (unsigned char c : text) if ((c & 0xC0) != 0x80) n++;
        return n;
    }

    // "Intro" taken -> "Intro 2"
    std::string uniqueName(const std::string& name, const std::vector<TitleAnimation>& taken){
        auto used = [&](const std::string& n){
            for (const TitleAnimation& a : taken) if (a.name == n) return true;
            return false;
        };
        if (!used(name)) return name;
        for (int i = 2;; i++){
            std::string candidate = name + " " + std::to_string(i);
            if (!used(candidate)) return candidate;
        }
    }

}

std::string TitleAnimWarning::toString() const{
    return path.empty() ? message : path + ": " + message;
}

// ==================================================================== values and keys

double titleanim::roundToFloat(double value){
    if (!std::isfinite(value)) return 0.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.7g", (double)(float)value);
    double out = std::strtod(buf, nullptr);
    return out == 0.0 ? 0.0 : out;     // -0 as 0
}

bool titleanim::parseHexColor(std::string_view text, float* rgb){
    if (text.size() != 4 && text.size() != 7) return false;
    if (text[0] != '#') return false;
    auto hex = [](char c) -> int{
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    if (text.size() == 4){
        for (int i = 0; i < 3; i++){
            v[2 * i] = v[2 * i + 1] = hex(text[1 + i]);
        }
    }else{
        for (int i = 0; i < 6; i++) v[i] = hex(text[1 + i]);
    }
    for (int i = 0; i < 6; i++) if (v[i] < 0) return false;
    for (int i = 0; i < 3; i++) rgb[i] = (float)(v[2 * i] * 16 + v[2 * i + 1]) / 255.0f;
    return true;
}

std::string titleanim::formatHexColor(const float* rgb){
    char buf[8];
    auto channel = [](float c){ return (int)std::lround(std::min(1.0f, std::max(0.0f, c)) * 255.0f); };
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", channel(rgb[0]), channel(rgb[1]), channel(rgb[2]));
    return buf;
}

bool titleanim::numericFromJson(const AnimJson& value, float* out, size_t& width){
    if (value.is_number()){
        const double v = value.get<double>();
        if (!std::isfinite(v)) return false;
        out[0] = (float)v;
        width = 1;
        return true;
    }
    if (value.is_array()){
        if (value.empty() || value.size() > 3) return false;
        for (size_t i = 0; i < value.size(); i++){
            if (!value[i].is_number() || !std::isfinite(value[i].get<double>())) return false;
            out[i] = (float)value[i].get<double>();
        }
        width = value.size();
        return true;
    }
    if (value.is_string() && parseHexColor(value.get<std::string>(), out)){
        width = 3;
        return true;
    }
    return false;
}

AnimJson titleanim::keyValueJson(const TitleKey& key){
    if (!key.stepped.empty()){
        AnimJson v = AnimJson::parse(key.stepped, nullptr, false);
        if (!v.is_discarded()) return v;
    }
    if (key.key.value.size() == 1) return roundToFloat(key.key.value[0]);
    return floatArray(key.key.value);
}

bool titleanim::decodeKey(const AnimJson& value, TitleKey& out, std::string& error){
    out = TitleKey();
    const AnimJson* time = nullptr;
    const AnimJson* v = nullptr;
    const AnimJson* ease = nullptr;
    const AnimJson* in = nullptr;
    const AnimJson* outT = nullptr;
    const AnimJson* mode = nullptr;
    if (value.is_array()){
        if (value.size() < 2 || value.size() > 3){
            error = "expected [time, value] or [time, value, ease]";
            return false;
        }
        time = &value[0];
        v = &value[1];
        if (value.size() == 3) ease = &value[2];
    }else if (value.is_object()){
        if (auto it = value.find("time"); it != value.end()) time = &*it;
        if (auto it = value.find("value"); it != value.end()) v = &*it;
        if (auto it = value.find("ease"); it != value.end()) ease = &*it;
        if (auto it = value.find("in"); it != value.end()) in = &*it;
        if (auto it = value.find("out"); it != value.end()) outT = &*it;
        if (auto it = value.find("tangent_mode"); it != value.end()) mode = &*it;
        if (!time || !v){
            error = "expected {time, value, ease, in, out, tangent_mode}";
            return false;
        }
    }else{
        error = "expected [time, value]";
        return false;
    }
    if (!time->is_number() || !std::isfinite(time->get<double>()) || time->get<double>() < 0.0){
        error = "expected a time >= 0 in seconds";
        return false;
    }
    out.key.time = (float)time->get<double>();
    float numbers[3];
    size_t width = 0;
    if (numericFromJson(*v, numbers, width)){
        out.key.value.assign(numbers, numbers + width);
        if (v->is_string()) out.stepped = v->dump();     // a colour keeps its text
    }else if (v->is_null() || v->is_object() || v->is_string() || v->is_boolean() || v->is_array()){
        out.stepped = v->dump();
    }else{
        error = "expected a value";
        return false;
    }
    if (ease){
        EaseType e = EaseType::LINEAR;
        if (!easeFrom(*ease, e)){
            error = "unknown ease (linear, step, quad_in ... bounce_in_out)";
            return false;
        }
        out.key.ease = e;
    }
    auto tangent = [&](const AnimJson* t, std::vector<float>& dst) -> bool{
        if (!t) return true;
        float tv[3];
        size_t tw = 0;
        if (!numericFromJson(*t, tv, tw) || t->is_string()) return false;
        dst.assign(tv, tv + tw);
        return true;
    };
    if (!tangent(in, out.key.inTangent) || !tangent(outT, out.key.outTangent)){
        error = "tangents are numbers or arrays of numbers";
        return false;
    }
    if (out.key.inTangent.size() != out.key.outTangent.size()){
        // one side given: the other mirrors it (a Free key)
        if (out.key.inTangent.empty()) out.key.inTangent = out.key.outTangent;
        else out.key.outTangent = out.key.inTangent;
    }
    if (mode){
        ClipTangentMode m = ClipTangentMode::None;
        if (!mode->is_string() || !anim::tangentModeFromName(mode->get<std::string>(), m)){
            error = "unknown tangent_mode (clamped_auto, auto, free, linear, constant, flat, broken)";
            return false;
        }
        out.key.tangentMode = m;
    }
    return true;
}

AnimJson titleanim::encodeKey(const TitleKey& key){
    const bool object = key.key.hasTangents() || key.key.tangentMode != ClipTangentMode::None;
    if (!object){
        AnimJson a = AnimJson::array({roundToFloat(key.key.time), keyValueJson(key)});
        if (key.key.ease != EaseType::LINEAR) a.push_back(anim::easeName(key.key.ease));
        return a;
    }
    AnimJson o = AnimJson::object();
    o["time"] = roundToFloat(key.key.time);
    o["value"] = keyValueJson(key);
    o["ease"] = anim::easeName(key.key.ease);
    if (key.key.hasTangents()){
        o["in"] = key.key.inTangent.size() == 1 ? AnimJson(roundToFloat(key.key.inTangent[0])) : floatArray(key.key.inTangent);
        o["out"] = key.key.outTangent.size() == 1 ? AnimJson(roundToFloat(key.key.outTangent[0])) : floatArray(key.key.outTangent);
    }
    if (key.key.tangentMode != ClipTangentMode::None) o["tangent_mode"] = anim::tangentModeName(key.key.tangentMode);
    return o;
}

// ==================================================================== the authoring form

bool titleanim::isAnimationName(std::string_view name){
    const size_t n = utf8Count(name);
    if (n == 0 || n > TITLE_ANIM_MAX_NAME) return false;
    for (unsigned char c : name) if (c == '/' || c < 0x20 || c == 0x7f) return false;
    return true;
}

void titleanim::sortMarkers(std::vector<TitleMarker>& markers){
    std::stable_sort(markers.begin(), markers.end(), [](const TitleMarker& a, const TitleMarker& b){
        if (a.time != b.time) return a.time < b.time;
        return a.name < b.name;
    });
}

bool titleanim::decodeStagger(const AnimJson& value, const std::string& path, TitleStagger& out, std::vector<TitleAnimWarning>* w){
    out = TitleStagger();
    if (!value.is_object()){
        warn(w, path, "expected {each | over, order, unit, seed, ease}, stagger dropped");
        return false;
    }
    out.each = (float)readNumber(value, "each", 0.05, 0.0, TITLE_ANIM_MAX_STAGGER_EACH, path, w);
    if (value.contains("over")){
        const double over = readNumber(value, "over", -1.0, -1.0, TITLE_ANIM_MAX_STAGGER_OVER, path, w);
        out.over = over < 0.0 ? -1.0f : (float)over;
    }
    out.order = readEnum(value, "order", StaggerOrder::Forward, &staggerOrderFromName, &staggerOrderName, path, w);
    out.unit = readEnum(value, "unit", StaggerUnit::Glyph, &staggerUnitFromName, &staggerUnitName, path, w);
    out.seed = (uint32_t)readNumber(value, "seed", 1, 0, 2147483647.0, path, w);
    if (auto it = value.find("ease"); it != value.end() && !easeFrom(*it, out.ease)){
        warn(w, member(path, "ease"), "unknown ease, using \"linear\"");
        out.ease = EaseType::LINEAR;
    }
    return true;
}

AnimJson titleanim::encodeStagger(const TitleStagger& s){
    AnimJson o = AnimJson::object();
    if (s.over >= 0.0f) o["over"] = roundToFloat(s.over);
    else o["each"] = roundToFloat(s.each);
    o["order"] = staggerOrderName(s.order);
    o["unit"] = staggerUnitName(s.unit);
    o["seed"] = s.seed;
    o["ease"] = anim::easeName(s.ease);
    return o;
}

bool titleanim::decodeDrive(const AnimJson& value, const std::string& path, TitleDrive& out, std::vector<TitleAnimWarning>* w){
    out = TitleDrive();
    if (!value.is_object()){
        warn(w, path, "expected {from, to, cycles | rate, ease, wrap}");
        return false;
    }
    float v[3];
    size_t width = 0;
    if (auto it = value.find("from"); it != value.end()){
        if (!numericFromJson(*it, v, width)){
            warn(w, member(path, "from"), "expected a number, [x, y] or \"#rrggbb\"");
            return false;
        }
        out.from.assign(v, v + width);
    }else{
        out.from = {0.0f};
    }
    if (auto it = value.find("to"); it != value.end()){
        if (!numericFromJson(*it, v, width)){
            warn(w, member(path, "to"), "expected a number, [x, y] or \"#rrggbb\"");
            return false;
        }
        out.to.assign(v, v + width);
    }else{
        out.to = out.from;
    }
    if (out.to.size() != out.from.size()){
        // a number against a pair: the number for each component
        const size_t n = std::max(out.from.size(), out.to.size());
        if (out.from.size() == 1) out.from.assign(n, out.from[0]);
        if (out.to.size() == 1) out.to.assign(n, out.to[0]);
        if (out.to.size() != out.from.size()){
            warn(w, path, "from and to have different widths");
            return false;
        }
    }
    out.cycles = (float)readNumber(value, "cycles", 0.0, 0.0, 1000.0, path, w);
    out.rate = (float)readNumber(value, "rate", 0.0, -1e6, 1e6, path, w);
    out.wrap = readBool(value, "wrap", false, path, w);
    if (auto it = value.find("ease"); it != value.end() && !easeFrom(*it, out.ease)){
        warn(w, member(path, "ease"), "unknown ease, using \"linear\"");
        out.ease = EaseType::LINEAR;
    }
    if (out.cycles <= 0.0f && out.rate == 0.0f) warn(w, path, "a drive with neither cycles nor rate holds its `from` value");
    return true;
}

AnimJson titleanim::encodeDrive(const TitleDrive& d){
    auto value = [](const std::vector<float>& v) -> AnimJson{
        if (v.size() == 1) return roundToFloat(v[0]);
        return floatArray(v);
    };
    AnimJson o = AnimJson::object();
    o["from"] = value(d.from);
    o["to"] = value(d.to);
    o["cycles"] = roundToFloat(d.cycles);
    o["rate"] = roundToFloat(d.rate);
    o["wrap"] = d.wrap;
    o["ease"] = anim::easeName(d.ease);
    return o;
}

bool titleanim::decodeTrack(const AnimJson& value, const std::string& path, TitleTrack& out, std::vector<TitleAnimWarning>* w){
    out = TitleTrack();
    if (!value.is_object()){
        warn(w, path, "expected a track object, track dropped");
        return false;
    }
    out.layer = readString(value, "layer", "", path, w);
    auto field = value.find("field");
    if (field == value.end() || !field->is_string() || field->get<std::string>().empty()){
        warn(w, member(path, "field"), "expected a field path, track dropped");
        return false;
    }
    out.field = field->get<std::string>();
    out.relative = readBool(value, "relative", false, path, w);
    out.pivot = readEnum(value, "pivot", TrackPivot::Origin, &trackPivotFromName, &trackPivotName, path, w);
    out.mute = readBool(value, "mute", false, path, w);
    out.fromPreset = readString(value, "from_preset", "", path, w);
    if (auto it = value.find("stagger"); it != value.end() && !it->is_null()){
        TitleStagger s;
        if (decodeStagger(*it, member(path, "stagger"), s, w)) out.stagger = s;
    }
    const bool hasKeys = value.contains("keys"), hasDrive = value.contains("drive"), hasReseed = value.contains("reseed");
    if ((int)hasKeys + (int)hasDrive + (int)hasReseed != 1){
        warn(w, path, "a track holds exactly one of keys, drive and reseed, track dropped");
        return false;
    }
    if (hasKeys){
        const AnimJson& keys = value["keys"];
        if (!keys.is_array()){
            warn(w, member(path, "keys"), "expected an array of keys, track dropped");
            return false;
        }
        bool sorted = true;
        for (size_t i = 0; i < keys.size(); i++){
            if (out.keys.size() >= TITLE_ANIM_MAX_KEYS){
                warn(w, member(path, "keys"), std::to_string(keys.size()) + " keys: only the first " + std::to_string(TITLE_ANIM_MAX_KEYS) + " are kept");
                break;
            }
            TitleKey key;
            std::string error;
            if (!decodeKey(keys[i], key, error)){
                warn(w, index(member(path, "keys"), i), error + ", key dropped");
                continue;
            }
            if (!out.keys.empty() && key.key.time < out.keys.back().key.time) sorted = false;
            out.keys.push_back(std::move(key));
        }
        if (!sorted){
            warn(w, member(path, "keys"), "keys were not in time order: sorted");
            std::stable_sort(out.keys.begin(), out.keys.end(), [](const TitleKey& a, const TitleKey& b){ return a.key.time < b.key.time; });
        }
        if (out.keys.empty()){
            warn(w, member(path, "keys"), "no keys, track dropped");
            return false;
        }
    }else if (hasDrive){
        TitleDrive d;
        if (!decodeDrive(value["drive"], member(path, "drive"), d, w)){
            warn(w, path, "track dropped");
            return false;
        }
        out.drive = d;
    }else{
        const AnimJson& r = value["reseed"];
        if (!r.is_object()){
            warn(w, member(path, "reseed"), "expected {rate, offset}, track dropped");
            return false;
        }
        TitleReseed reseed;
        reseed.rate = (float)readNumber(r, "rate", 12.0, 0.0, 240.0, member(path, "reseed"), w);
        reseed.offset = (float)readNumber(r, "offset", 0.0, -1e6, 1e6, member(path, "reseed"), w);
        out.reseed = reseed;
    }
    out.unknown = unknownOf(value, {"layer", "field", "relative", "pivot", "mute", "from_preset", "stagger", "keys", "drive", "reseed"});
    return true;
}

AnimJson titleanim::encodeTrack(const TitleTrack& t){
    AnimJson o = AnimJson::object();
    o["layer"] = t.layer;
    o["field"] = t.field;
    o["relative"] = t.relative;
    o["pivot"] = trackPivotName(t.pivot);
    o["mute"] = t.mute;
    if (!t.fromPreset.empty()) o["from_preset"] = t.fromPreset;
    if (t.stagger) o["stagger"] = encodeStagger(*t.stagger);
    if (t.drive){
        o["drive"] = encodeDrive(*t.drive);
    }else if (t.reseed){
        o["reseed"] = AnimJson{{"rate", roundToFloat(t.reseed->rate)}, {"offset", roundToFloat(t.reseed->offset)}};
    }else{
        AnimJson keys = AnimJson::array();
        for (const TitleKey& k : t.keys) keys.push_back(encodeKey(k));
        o["keys"] = std::move(keys);
    }
    appendUnknown(o, t.unknown);
    return o;
}

bool titleanim::decodeMarker(const AnimJson& value, const std::string& path, TitleMarker& out, std::vector<TitleAnimWarning>* w){
    out = TitleMarker();
    if (!value.is_object()){
        warn(w, path, "expected a marker object, marker dropped");
        return false;
    }
    auto time = value.find("time");
    if (time == value.end() || !time->is_number() || !std::isfinite(time->get<double>())){
        warn(w, member(path, "time"), "expected a time in seconds, marker dropped");
        return false;
    }
    out.time = (float)std::max(0.0, time->get<double>());
    out.name = readString(value, "name", "", path, w);
    if (!anim::isLuaIdentifier(out.name)){
        warn(w, member(path, "name"), "\"" + out.name + "\" is not a Lua function name, marker dropped");
        return false;
    }
    if (auto args = value.find("args"); args != value.end()){
        if (!args->is_array()){
            warn(w, member(path, "args"), "expected an array of numbers, strings and booleans");
        }else{
            for (size_t i = 0; i < args->size(); i++){
                if (out.args.size() >= MAX_ANIMATION_EVENT_ARGS){
                    warn(w, member(path, "args"), "more than " + std::to_string(MAX_ANIMATION_EVENT_ARGS) + " args: the rest are dropped");
                    break;
                }
                const AnimJson& a = (*args)[i];
                if (a.is_number() && std::isfinite(a.get<double>())) out.args.push_back(AnimationEvent::numberArg(a.get<double>()));
                else if (a.is_string()) out.args.push_back(AnimationEvent::stringArg(a.get<std::string>()));
                else if (a.is_boolean()) out.args.push_back(AnimationEvent::boolArg(a.get<bool>()));
                else warn(w, index(member(path, "args"), i), "not a number, string or boolean, dropped");
            }
        }
    }
    if (auto sound = value.find("sound"); sound != value.end() && !assetRefFrom(*sound, out.sound))
        warn(w, member(path, "sound"), "expected {path, guid}, no sound");
    out.volumeDb = (float)readNumber(value, "volume_db", 0.0, -60.0, 12.0, path, w);
    out.group = readString(value, "group", "", path, w);
    out.unknown = unknownOf(value, {"time", "name", "args", "sound", "volume_db", "group"});
    return true;
}

AnimJson titleanim::encodeMarker(const TitleMarker& m){
    AnimJson o = AnimJson::object();
    o["time"] = roundToFloat(m.time);
    o["name"] = m.name;
    AnimJson args = AnimJson::array();
    for (const AnimationEventArg& a : m.args){
        if (std::holds_alternative<double>(a)) args.push_back(roundToFloat(std::get<double>(a)) == std::get<double>(a) ? std::get<double>(a) : std::get<double>(a));
        else if (std::holds_alternative<std::string>(a)) args.push_back(std::get<std::string>(a));
        else args.push_back(std::get<bool>(a));
    }
    o["args"] = std::move(args);
    if (!m.sound.empty()) o["sound"] = assetRefJson(m.sound);
    o["volume_db"] = roundToFloat(m.volumeDb);
    o["group"] = m.group;
    appendUnknown(o, m.unknown);
    return o;
}

bool titleanim::decodeAnimation(const AnimJson& value, const std::string& path, TitleAnimation& out, std::vector<TitleAnimWarning>* w){
    out = TitleAnimation();
    if (!value.is_object()){
        warn(w, path, "expected an animation object, animation dropped");
        return false;
    }
    out.id = readString(value, "id", "", path, w);
    out.name = readString(value, "name", "", path, w);
    if (!isAnimationName(out.name)){
        warn(w, member(path, "name"), "\"" + out.name + "\" is not a name (1-32 characters, no '/'), using \"Animation\"");
        out.name = "Animation";
    }
    out.role = readEnum(value, "role", AnimRole::Custom, &animRoleFromName, &animRoleName, path, w);
    {
        auto it = value.find("duration");
        double d = TITLE_ANIM_AUTO_DURATION;
        if (it != value.end()){
            if (!it->is_number() || !std::isfinite(it->get<double>())){
                warn(w, member(path, "duration"), "expected seconds or -1 (automatic), using -1");
            }else{
                d = it->get<double>();
                if (d < 0.0){
                    if (d != TITLE_ANIM_AUTO_DURATION) warn(w, member(path, "duration"), shortNumber(d) + ": negative means automatic (-1)");
                    d = TITLE_ANIM_AUTO_DURATION;
                }else if (d < TITLE_ANIM_MIN_DURATION || d > TITLE_ANIM_MAX_DURATION){
                    const double c = std::min(TITLE_ANIM_MAX_DURATION, std::max(TITLE_ANIM_MIN_DURATION, d));
                    warn(w, member(path, "duration"), shortNumber(d) + " clamped to " + shortNumber(c));
                    d = c;
                }
            }
        }
        out.duration = (float)d;
    }
    out.wrap = readEnum(value, "wrap", AnimWrap::Once, &animWrapFromName, &animWrapName, path, w);
    out.loopStart = (float)readNumber(value, "loop_start", 0.0, 0.0, TITLE_ANIM_MAX_DURATION, path, w);
    if (out.duration > 0.0f && out.loopStart >= out.duration){
        warn(w, member(path, "loop_start"), shortNumber(out.loopStart) + " is not before the duration, using 0");
        out.loopStart = 0.0f;
    }
    out.next = readString(value, "next", "", path, w);
    out.overlay = readBool(value, "overlay", false, path, w);
    out.speed = (float)readNumber(value, "speed", 1.0, 0.01, 10.0, path, w);
    out.rerender = readEnum(value, "rerender", RerenderPolicy::Live, &rerenderPolicyFromName, &rerenderPolicyName, path, w);
    out.rerenderFps = (float)readNumber(value, "rerender_fps", 30.0, 1.0, 60.0, path, w);
    if (auto fb = value.find("flipbook"); fb != value.end()){
        const std::string fp = member(path, "flipbook");
        if (!fb->is_object()){
            warn(w, fp, "expected {fps, scale, max_page, frame_blend}, using the defaults");
        }else{
            out.flipbook.fps = (float)readNumber(*fb, "fps", 30.0, 1.0, (double)TITLE_ANIM_MAX_FLIPBOOK_FPS, fp, w);
            out.flipbook.scale = (float)readNumber(*fb, "scale", 1.0, 0.25, 1.0, fp, w);
            int page = (int)readNumber(*fb, "max_page", 2048, 1024, 4096, fp, w);
            if (page != 1024 && page != 2048 && page != 4096){
                const int snapped = page < 1536 ? 1024 : (page < 3072 ? 2048 : 4096);
                warn(w, member(fp, "max_page"), std::to_string(page) + " is not 1024, 2048 or 4096: using " + std::to_string(snapped));
                page = snapped;
            }
            out.flipbook.maxPage = page;
            out.flipbook.frameBlend = readBool(*fb, "frame_blend", false, fp, w);
        }
    }
    if (auto ps = value.find("preset_source"); ps != value.end() && !ps->is_null()){
        if (ps->is_object() && ps->contains("id") && (*ps)["id"].is_string()){
            out.hasPresetSource = true;
            out.presetId = (*ps)["id"].get<std::string>();
            const AnimJson params = ps->contains("params") && (*ps)["params"].is_object() ? (*ps)["params"] : AnimJson::object();
            out.presetParams = params.dump();
        }else{
            warn(w, member(path, "preset_source"), "expected {id, params}, dropped");
        }
    }
    if (auto tracks = value.find("tracks"); tracks != value.end()){
        const std::string tp = member(path, "tracks");
        if (!tracks->is_array()){
            warn(w, tp, "expected an array of tracks");
        }else{
            for (size_t i = 0; i < tracks->size(); i++){
                if (out.tracks.size() >= TITLE_ANIM_MAX_TRACKS){
                    warn(w, tp, std::to_string(tracks->size()) + " tracks: only the first " + std::to_string(TITLE_ANIM_MAX_TRACKS) + " are kept");
                    break;
                }
                TitleTrack t;
                if (decodeTrack((*tracks)[i], index(tp, i), t, w)) out.tracks.push_back(std::move(t));
            }
        }
    }
    if (auto markers = value.find("markers"); markers != value.end()){
        const std::string mp = member(path, "markers");
        if (!markers->is_array()){
            warn(w, mp, "expected an array of markers");
        }else{
            for (size_t i = 0; i < markers->size(); i++){
                if (out.markers.size() >= TITLE_ANIM_MAX_MARKERS){
                    warn(w, mp, std::to_string(markers->size()) + " markers: only the first " + std::to_string(TITLE_ANIM_MAX_MARKERS) + " are kept");
                    break;
                }
                TitleMarker m;
                if (decodeMarker((*markers)[i], index(mp, i), m, w)) out.markers.push_back(std::move(m));
            }
            sortMarkers(out.markers);
        }
    }
    out.unknown = unknownOf(value, {"id", "name", "role", "duration", "wrap", "loop_start", "next", "overlay", "speed", "rerender",
                                    "rerender_fps", "flipbook", "preset_source", "tracks", "markers"});
    return true;
}

AnimJson titleanim::encodeAnimation(const TitleAnimation& a){
    AnimJson o = AnimJson::object();
    o["id"] = a.id;
    o["name"] = a.name;
    o["role"] = animRoleName(a.role);
    o["duration"] = a.duration < 0.0f ? AnimJson(-1) : AnimJson(roundToFloat(a.duration));
    o["wrap"] = animWrapName(a.wrap);
    o["loop_start"] = roundToFloat(a.loopStart);
    o["next"] = a.next;
    o["overlay"] = a.overlay;
    o["speed"] = roundToFloat(a.speed);
    o["rerender"] = rerenderPolicyName(a.rerender);
    o["rerender_fps"] = roundToFloat(a.rerenderFps);
    o["flipbook"] = AnimJson{{"fps", roundToFloat(a.flipbook.fps)}, {"scale", roundToFloat(a.flipbook.scale)},
                             {"max_page", a.flipbook.maxPage}, {"frame_blend", a.flipbook.frameBlend}};
    if (a.hasPresetSource){
        AnimJson params = AnimJson::parse(a.presetParams.empty() ? std::string("{}") : a.presetParams, nullptr, false);
        o["preset_source"] = AnimJson{{"id", a.presetId}, {"params", params.is_object() ? params : AnimJson::object()}};
    }
    AnimJson tracks = AnimJson::array();
    for (const TitleTrack& t : a.tracks) tracks.push_back(encodeTrack(t));
    o["tracks"] = std::move(tracks);
    AnimJson markers = AnimJson::array();
    for (const TitleMarker& m : a.markers) markers.push_back(encodeMarker(m));
    o["markers"] = std::move(markers);
    appendUnknown(o, a.unknown);
    return o;
}

void titleanim::decodeAnimationSet(const AnimJson& value, const std::string& path, TitleAnimationSet& out, std::vector<TitleAnimWarning>* w){
    out = TitleAnimationSet();
    if (value.is_null()) return;
    if (!value.is_object()){
        warn(w, path, "expected {fps, default, animations}, no animations");
        return;
    }
    out.fps = (int)std::lround(readNumber(value, "fps", TITLE_ANIM_DEFAULT_FPS, 1, 120, path, w));
    out.defaultName = readString(value, "default", "", path, w);
    if (auto list = value.find("animations"); list != value.end()){
        const std::string ap = member(path, "animations");
        if (!list->is_array()){
            warn(w, ap, "expected an array of animations");
        }else{
            for (size_t i = 0; i < list->size(); i++){
                if (out.animations.size() >= TITLE_ANIM_MAX_ANIMATIONS){
                    warn(w, ap, std::to_string(list->size()) + " animations: only the first " + std::to_string(TITLE_ANIM_MAX_ANIMATIONS) + " are kept");
                    break;
                }
                TitleAnimation a;
                if (!decodeAnimation((*list)[i], index(ap, i), a, w)) continue;
                const std::string unique = uniqueName(a.name, out.animations);
                if (unique != a.name){
                    warn(w, member(index(ap, i), "name"), "\"" + a.name + "\" is taken, renamed \"" + unique + "\"");
                    a.name = unique;
                }
                out.animations.push_back(std::move(a));
            }
        }
    }
    if (!out.defaultName.empty() && !out.find(out.defaultName)){
        warn(w, member(path, "default"), "no animation \"" + out.defaultName + "\", none is the default");
        out.defaultName.clear();
    }
    out.unknown = unknownOf(value, {"fps", "default", "animations"});
}

AnimJson titleanim::encodeAnimationSet(const TitleAnimationSet& set){
    AnimJson o = AnimJson::object();
    o["fps"] = set.fps;
    o["default"] = set.defaultName;
    AnimJson list = AnimJson::array();
    for (const TitleAnimation& a : set.animations) list.push_back(encodeAnimation(a));
    o["animations"] = std::move(list);
    appendUnknown(o, set.unknown);
    return o;
}

// ==================================================================== title-live tracks

bool titleanim::splitComponent(std::string_view field, std::string& base, int& component){
    base = std::string(field);
    component = -1;
    const size_t dot = field.rfind('.');
    if (dot == std::string_view::npos || dot + 2 != field.size()) return false;
    const char c = field.back();
    int index = -1;
    if (c == 'x' || c == 'r') index = 0;
    else if (c == 'y' || c == 'g') index = 1;
    else if (c == 'b') index = 2;
    if (index < 0) return false;
    base = std::string(field.substr(0, dot));
    component = index;
    return true;
}

bool titleanim::compileTitleTrack(const TitleTrack& track, CompiledTitleTrack& out, std::string* error){
    out = CompiledTitleTrack();
    out.layer = track.layer;
    out.field = track.field;
    std::string base = track.field;
    int component = -1;
    TitleField field = titleFieldFromPath(track.field);
    if (field == TitleField::None && splitComponent(track.field, base, component)) field = titleFieldFromPath(base);
    if (field == TitleField::None){
        if (error) *error = "'" + track.field + "' is not a title field";
        return false;
    }
    const VirtualFieldInfo info = titleFieldInfo(field);
    if (component >= 0){
        const bool fits = (info.value == TrackValue::Vec2 && component < 2) || (info.value == TrackValue::Color && component < 3);
        if (!fits){
            if (error) *error = "'" + track.field + "' names no component of " + base;
            return false;
        }
    }
    const size_t width = component >= 0 ? 1 : info.width;
    out.target = TrackTarget::Title;
    out.titleField = field;
    out.component = component;
    out.value = component >= 0 ? TrackValue::Number : info.value;
    out.width = (uint8_t)width;
    out.cls = TrackClass::TitleLive;
    out.pivot = track.pivot;
    out.mute = track.mute;
    out.orphan = track.orphan;
    out.min = info.min;
    out.max = info.max;
    out.combine = track.relative ? info.relativeCombine : TrackCombine::Replace;
    // a relative title track's pose holds its raw value: the rest is its combination's identity
    const float identity = out.combine == TrackCombine::Multiply ? 1.0f : 0.0f;
    out.rest.assign(width, identity);
    if (track.reseed){
        if (error) *error = "'" + track.field + "' is not a seed";
        return false;
    }
    if (track.drive){
        out.source = TrackSource::Drive;
        out.driveFrom = track.drive->from;
        out.driveTo = track.drive->to;
        out.driveFrom.resize(width, out.driveFrom.empty() ? 0.0f : out.driveFrom.back());
        out.driveTo.resize(width, out.driveTo.empty() ? 0.0f : out.driveTo.back());
        out.cycles = track.drive->cycles;
        out.rate = track.drive->rate;
        out.driveWrap = track.drive->wrap;
        out.driveEase = track.drive->ease;
        return true;
    }
    std::vector<ClipKey> keys;
    keys.reserve(track.keys.size());
    for (const TitleKey& k : track.keys){
        ClipKey key = k.key;
        if (key.value.size() == 1 && width > 1) key.value.assign(width, key.value[0]);
        if (key.value.size() != width){
            if (error) *error = "'" + track.field + "' keys need " + std::to_string(width) + " values";
            return false;
        }
        keys.push_back(std::move(key));
    }
    compileNumericKeys(keys, width, out);
    return true;
}

double titleanim::resolveDuration(const TitleAnimation& animation, const CompiledTitleAnimation& compiled){
    if (animation.duration > 0.0f) return std::max(TITLE_ANIM_MIN_DURATION, (double)animation.duration);
    double end = 0.0;
    for (const CompiledTitleTrack& t : compiled.tracks) if (!t.orphan) end = std::max(end, t.endTime());
    for (const CompiledMarker& m : compiled.markers) end = std::max(end, (double)m.time);
    // nothing sets an end (only drives): one second
    if (end <= 0.0) return 1.0;
    return std::min(TITLE_ANIM_MAX_DURATION, std::max(TITLE_ANIM_MIN_DURATION, end));
}

void titleanim::compileMarkers(const TitleAnimation& animation, CompiledTitleAnimation& out){
    out.markers.clear();
    for (const TitleMarker& m : animation.markers){
        CompiledMarker c;
        c.time = m.time;
        c.name = m.name;
        c.args = m.args;
        c.sound = m.sound;
        c.volumeDb = m.volumeDb;
        c.group = m.group;
        out.markers.push_back(std::move(c));
    }
    std::stable_sort(out.markers.begin(), out.markers.end(), [](const CompiledMarker& a, const CompiledMarker& b){
        if (a.time != b.time) return a.time < b.time;
        return a.name < b.name;
    });
}

void titleanim::compileTitleOnly(const TitleAnimation& animation, CompiledTitleAnimation& out, std::vector<TitleAnimWarning>* warnings){
    CompiledFlipbook flipbook = std::move(out.flipbook);
    BakedCost cost = out.bakedCost;
    out = CompiledTitleAnimation();
    out.flipbook = std::move(flipbook);
    out.bakedCost = cost;
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
    for (size_t i = 0; i < animation.tracks.size(); i++){
        const TitleTrack& t = animation.tracks[i];
        if (!t.layer.empty()) continue;
        CompiledTitleTrack c;
        std::string error;
        if (!compileTitleTrack(t, c, &error)){
            if (warnings) warnings->push_back({"tracks[" + std::to_string(i) + "]", error + ", not played"});
            continue;
        }
        out.tracks.push_back(std::move(c));
    }
    compileMarkers(animation, out);
    out.duration = resolveDuration(animation, out);
    out.loopStart = std::min((double)animation.loopStart, out.duration * 0.999);
    out.layoutPose();
}

// ==================================================================== <file>.anim.json

const TitleAnimDataEntry* TitleAnimData::find(std::string_view name) const{
    for (const TitleAnimDataEntry& e : animations) if (e.authored.name == name) return &e;
    return nullptr;
}

namespace {

    bool readFlipbook(const AnimJson& fb, const std::string& path, CompiledFlipbook& out, std::vector<std::string>* warnings){
        out = CompiledFlipbook();
        if (fb.is_null()) return true;
        auto note = [&](const std::string& m){ if (warnings) warnings->push_back(path + ": " + m); };
        if (!fb.is_object()){
            note("expected a flipbook object or null, no flipbook");
            return false;
        }
        out.fps = fb.value("fps", 30.0f);
        if (!(out.fps >= 1.0f && out.fps <= (float)TITLE_ANIM_MAX_FLIPBOOK_FPS)) out.fps = 30.0f;
        auto rect = fb.find("frame");
        if (rect == fb.end() || !rect->is_array() || rect->size() != 4){
            note("frame: expected [x, y, width, height], no flipbook");
            return false;
        }
        out.frameX = (*rect)[0].get<int>();
        out.frameY = (*rect)[1].get<int>();
        out.frameWidth = (*rect)[2].get<int>();
        out.frameHeight = (*rect)[3].get<int>();
        if (auto size = fb.find("size"); size != fb.end() && size->is_array() && size->size() == 2){
            out.pixelWidth = (*size)[0].get<int>();
            out.pixelHeight = (*size)[1].get<int>();
        }else{
            out.pixelWidth = out.frameWidth;
            out.pixelHeight = out.frameHeight;
        }
        out.blend = fb.value("blend", false);
        if (auto pages = fb.find("pages"); pages != fb.end() && pages->is_array()){
            for (const AnimJson& p : *pages){
                if (!p.is_object() || !p.contains("file") || !p["file"].is_string()) continue;
                FlipbookPage page;
                page.file = p["file"].get<std::string>();
                page.guid = p.value("guid", std::string());
                if (auto s = p.find("size"); s != p.end() && s->is_array() && s->size() == 2){
                    page.width = (*s)[0].get<int>();
                    page.height = (*s)[1].get<int>();
                }
                out.pages.push_back(std::move(page));
            }
        }
        if (auto frames = fb.find("frames"); frames != fb.end() && frames->is_array()){
            for (const AnimJson& f : *frames){
                if (!f.is_array() || f.size() != 3 || !f[0].is_number_integer()) continue;
                FlipbookFrame frame{f[0].get<int>(), f[1].get<int>(), f[2].get<int>()};
                if (frame.page < 0 || frame.page >= (int)out.pages.size()) continue;
                out.frames.push_back(frame);
            }
        }
        if (out.frameWidth <= 0 || out.frameHeight <= 0 || out.pixelWidth <= 0 || out.pixelHeight <= 0){
            note("an empty frame, no flipbook");
            out = CompiledFlipbook();
            return false;
        }
        return true;
    }

    AnimJson flipbookJson(const CompiledFlipbook& fb){
        if (fb.empty()) return nullptr;
        AnimJson o = AnimJson::object();
        o["fps"] = roundToFloat(fb.fps);
        o["frame"] = AnimJson::array({fb.frameX, fb.frameY, fb.frameWidth, fb.frameHeight});
        o["size"] = AnimJson::array({fb.pixelWidth, fb.pixelHeight});
        o["blend"] = fb.blend;
        AnimJson pages = AnimJson::array();
        for (const FlipbookPage& p : fb.pages)
            pages.push_back(AnimJson{{"file", p.file}, {"guid", p.guid}, {"size", AnimJson::array({p.width, p.height})}});
        o["pages"] = std::move(pages);
        AnimJson frames = AnimJson::array();
        for (const FlipbookFrame& f : fb.frames) frames.push_back(AnimJson::array({f.page, f.x, f.y}));
        o["frames"] = std::move(frames);
        return o;
    }

    AnimJson costJson(const BakedCost& c){
        return AnimJson{{"frames", c.frames}, {"unique", c.unique}, {"pages", c.pages},
                        {"bytes", AnimJson{{"desktop", c.bytesDesktop}, {"mobile", c.bytesMobile}, {"android_etc2", c.bytesAndroidEtc2},
                                           {"web", c.bytesWeb}}}};
    }

    void readCost(const AnimJson& value, BakedCost& out){
        out = BakedCost();
        if (!value.is_object()) return;
        out.frames = value.value("frames", 0);
        out.unique = value.value("unique", 0);
        out.pages = value.value("pages", 0);
        if (auto b = value.find("bytes"); b != value.end() && b->is_object()){
            out.bytesDesktop = b->value("desktop", (uint64_t)0);
            out.bytesMobile = b->value("mobile", (uint64_t)0);
            out.bytesAndroidEtc2 = b->value("android_etc2", (uint64_t)0);
            out.bytesWeb = b->value("web", (uint64_t)0);
        }
    }

}

bool titleanim::readAnimData(std::string_view text, TitleAnimData& out, std::vector<std::string>* warnings, std::string* error){
    out = TitleAnimData();
    AnimJson root = AnimJson::parse(text.begin(), text.end(), nullptr, false);
    auto fail = [&](const std::string& m){
        if (error) *error = m;
        return false;
    };
    if (root.is_discarded() || !root.is_object()) return fail("not a title animation data file (not a JSON object)");
    if (root.value("format", std::string()) != TITLE_ANIM_DATA_FORMAT)
        return fail(std::string("not a title animation data file (format is not ") + TITLE_ANIM_DATA_FORMAT + ")");
    const int version = root.value("version", 0);
    if (version > TITLE_ANIM_DATA_VERSION)
        return fail("needs a newer JasperEngine (version " + std::to_string(version) + "; this build reads " + std::to_string(TITLE_ANIM_DATA_VERSION) +
                    "): bake it again with this editor");
    if (version < 1) return fail("has no version");
    out.sourceHash = root.value("source_hash", std::string());
    out.bakeVersion = root.value("bake_version", 0u);
    out.defaultName = root.value("default", std::string());
    std::vector<TitleAnimWarning> w;
    if (auto list = root.find("animations"); list != root.end() && list->is_array()){
        for (size_t i = 0; i < list->size(); i++){
            const AnimJson& a = (*list)[i];
            const std::string path = "animations[" + std::to_string(i) + "]";
            TitleAnimDataEntry entry;
            // the animation's settings and markers, its title tracks under `title_tracks`
            AnimJson settings = a;
            if (settings.is_object()){
                settings.erase("title_tracks");
                settings.erase("flipbook");
                settings.erase("baked_cost");
            }
            if (!decodeAnimation(settings, path, entry.authored, &w)) continue;
            entry.authored.unknown.clear();
            if (auto tracks = a.find("title_tracks"); tracks != a.end() && tracks->is_array()){
                for (size_t t = 0; t < tracks->size(); t++){
                    TitleTrack track;
                    AnimJson tj = (*tracks)[t];
                    if (tj.is_object()) tj["layer"] = "";
                    if (decodeTrack(tj, path + ".title_tracks[" + std::to_string(t) + "]", track, &w)){
                        track.unknown.clear();
                        entry.authored.tracks.push_back(std::move(track));
                    }
                }
            }
            if (auto fb = a.find("flipbook"); fb != a.end()){
                std::vector<std::string> fw;
                readFlipbook(*fb, path + ".flipbook", entry.compiled.flipbook, warnings ? warnings : &fw);
            }
            if (auto cost = a.find("baked_cost"); cost != a.end()) readCost(*cost, entry.compiled.bakedCost);
            compileTitleOnly(entry.authored, entry.compiled, &w);
            out.animations.push_back(std::move(entry));
        }
    }
    if (warnings) for (const TitleAnimWarning& x : w) warnings->push_back(x.toString());
    return true;
}

bool titleanim::loadAnimData(const std::string& path, TitleAnimData& out, std::vector<std::string>* warnings, std::string* error){
    Data data;
    if (data.open(path.c_str()) != FileErrors::FILEDATA_OK){
        if (error) *error = "cannot read " + path;
        return false;
    }
    const std::string text = data.readString();
    return readAnimData(text, out, warnings, error);
}

std::string titleanim::writeAnimData(const TitleAnimData& data){
    AnimJson root = AnimJson::object();
    root["format"] = TITLE_ANIM_DATA_FORMAT;
    root["version"] = TITLE_ANIM_DATA_VERSION;
    root["source_hash"] = data.sourceHash;
    root["bake_version"] = data.bakeVersion;
    root["default"] = data.defaultName;
    AnimJson list = AnimJson::array();
    for (const TitleAnimDataEntry& e : data.animations){
        const TitleAnimation& a = e.authored;
        AnimJson o = AnimJson::object();
        o["id"] = a.id;
        o["name"] = a.name;
        o["role"] = animRoleName(a.role);
        o["duration"] = a.duration < 0.0f ? AnimJson(-1) : AnimJson(roundToFloat(a.duration));
        o["wrap"] = animWrapName(a.wrap);
        o["loop_start"] = roundToFloat(a.loopStart);
        o["next"] = a.next;
        o["overlay"] = a.overlay;
        o["speed"] = roundToFloat(a.speed);
        AnimJson tracks = AnimJson::array();
        for (const TitleTrack& t : a.tracks){
            if (!t.layer.empty()) continue;
            AnimJson tj = encodeTrack(t);
            tj.erase("layer");
            tj.erase("from_preset");
            tracks.push_back(std::move(tj));
        }
        o["title_tracks"] = std::move(tracks);
        AnimJson markers = AnimJson::array();
        for (const TitleMarker& m : a.markers) markers.push_back(encodeMarker(m));
        o["markers"] = std::move(markers);
        o["flipbook"] = flipbookJson(e.compiled.flipbook);
        o["baked_cost"] = costJson(e.compiled.bakedCost);
        list.push_back(std::move(o));
    }
    root["animations"] = std::move(list);
    return root.dump(1) + "\n";
}

std::string titleanim::titleAnimDataPath(const std::string& titlePath){
    return titlePath + TITLE_ANIM_DATA_SUFFIX;
}

std::string titleanim::titleFlipbookPageFile(const std::string& titlePath, const std::string& animationId, int page){
    const size_t slash = titlePath.find_last_of('/');
    const std::string file = slash == std::string::npos ? titlePath : titlePath.substr(slash + 1);
    return file + "." + animationId + ".flip" + std::to_string(page) + ".png";
}
