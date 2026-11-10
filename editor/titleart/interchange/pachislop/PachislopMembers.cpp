// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PachislopMembers.h"

#include "titleart/TitleArtSchema.h"
#include "titleart/library/PachislopPort.h"
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
#include "titleart/three_d/Title3DConvert.h"
#endif
#include <cctype>
#include <cstdlib>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pachislop;
using titleart::SchemaKind;
using titleart::SchemaMember;
using titleart::TitleArtSchema;
using titleart::ValueType;
namespace fs = std::filesystem;

namespace {

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    bool camelWord(const std::string& s) {
        if (s.empty() || !std::isalpha((unsigned char)s[0])) return false;
        for (unsigned char c : s) if (!std::isalnum(c)) return false;
        return true;
    }

    Json vec2Of(const Json& v) {
        if (v.is_array()) return v;
        if (v.is_object()) return Json::array({num(v, "x", 0), num(v, "y", 0)});
        return Json::array({0, 0});
    }

    Json convertValue(const Json& v, const SchemaMember& m, const MemberContext& ctx, const std::string& path);

    Json material3D(const Json& v) {
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
        return v.is_object() ? titleart::material3DFromPachislop(v) : Json();
#else
        (void)v;
        return Json();
#endif
    }

    // a Pachislop key consumed by a kind's own rule: true when handled
    bool special(const std::string& kind, const std::string& key, const Json& value, const Json& object, Json& out, const MemberContext& ctx,
                 const std::string& path) {
        const std::string at = path.empty() ? key : path + "." + key;
        if (kind == "reflection" && key == "map") {
            // the 2D maps are S86's photo environments, as S84's port maps them; an enabled map is an
            // approximation either way (the photo of the same place, not Pachislop's own panorama)
            const std::string name = value.is_string() ? value.get<std::string>() : std::string();
            Json ref = !name.empty() && ctx.environment ? ctx.environment(name, at) : Json();
            if (!ref.is_null()) out["map"] = ref;
            if (object.value("enabled", false)) {
                ctx.note(at, "reflection map '" + name + "' " +
                                 (ref.is_null() ? "has no library map in this project: the reflection shows no map until one is set"
                                                : "reads the library's photo environment of the same place, not Pachislop's own panorama"), "approximated");
            }
            return true;
        }
        if (kind == "char_adjust" && (key == "face3d" || key == "side3d" || key == "bands3d" || key == "dz" || key == "tiltX" || key == "tiltY")) {
            if (!ctx.has3D) {
                ctx.note(at, "3D letter adjustment (needs the 3D module)");
                return true;
            }
            if (key == "bands3d") {
                Json list = Json::array();
                if (value.is_array()) for (const Json& b : value) list.push_back(b.is_object() ? material3D(b) : Json());
                out["bands_3d"] = list;
            } else if (key == "face3d" || key == "side3d") {
                out[key == "face3d" ? "face_3d" : "side_3d"] = material3D(value);
            } else {
                out[key == "dz" ? "dz" : key == "tiltX" ? "tilt_x" : "tilt_y"] = value;
            }
            return true;
        }
        if (kind == "trace" && (key == "smooth" || key == "despeckle")) {
            // Pachislop's fractions as S80's percentages
            out[key] = num(object, key.c_str(), 0) * 100;
            return true;
        }
        if (kind == "style2d" && key == "hideFace") {
            if (value == true) ctx.note(at, "hidden face (the rims alone): the face shows; set its opacity to 0 instead");
            return true;
        }
        if (kind == "rim" && key == "id") {
            if (value.is_string() && titleart::isTitleArtId(value.get<std::string>())) out["id"] = value;
            return true;
        }
        return false;
    }

    Json convertValue(const Json& v, const SchemaMember& m, const MemberContext& ctx, const std::string& path) {
        switch (m.type) {
            case ValueType::Object:
                if (v.is_null()) return Json();
                if (!v.is_object()) return v;
                return convertMembers(v, m.kind, ctx, path);
            case ValueType::ObjectArray: {
                Json list = Json::array();
                if (v.is_array()) {
                    for (size_t i = 0; i < v.size(); i++) {
                        list.push_back(v[i].is_object() ? convertMembers(v[i], m.kind, ctx, path + "[" + std::to_string(i) + "]") : v[i]);
                    }
                }
                return list;
            }
            case ValueType::ObjectMap: {
                Json map = Json::object();
                if (v.is_object()) {
                    for (auto it = v.begin(); it != v.end(); ++it) {
                        map[it.key()] = it->is_object() ? convertMembers(*it, m.kind, ctx, path + "." + it.key()) : *it;
                    }
                }
                return map;
            }
            case ValueType::Enum:
                if (v.is_string() && camelWord(v.get<std::string>())) return snakeCase(v.get<std::string>());
                if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
                return v;
            case ValueType::Color:
                return v.is_string() ? Json(lower(v.get<std::string>())) : v;
            case ValueType::Vec2:
                return vec2Of(v);
            case ValueType::Vec2List: {
                Json list = Json::array();
                if (v.is_array()) for (const Json& p : v) list.push_back(vec2Of(p));
                return list;
            }
            case ValueType::AssetRef:
                if (v.is_string() && ctx.asset) return ctx.asset(v.get<std::string>(), path);
                return v.is_object() ? v : Json();
            case ValueType::LayerRef:
                if (v.is_string() && ctx.layer) return ctx.layer(v.get<std::string>());
                return Json("");
            default:
                return v;
        }
    }

}

std::string pachislop::snakeCase(const std::string& key) {
    std::string out;
    for (size_t i = 0; i < key.size(); i++) {
        const char c = key[i];
        if (std::isupper((unsigned char)c)) {
            if (i > 0) out += '_';
            out += (char)std::tolower((unsigned char)c);
        } else {
            out += c;
        }
    }
    return out;
}

Json pachislop::convertMembers(const Json& p, const std::string& kindName, const MemberContext& ctx, const std::string& path) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    const SchemaKind* kind = schema.kind(kindName);
    Json out = Json::object();
    if (!kind || !p.is_object()) return out;
    for (auto it = p.begin(); it != p.end(); ++it) {
        const std::string& key = it.key();
        if (special(kindName, key, it.value(), p, out, ctx, path)) continue;
        const titlelibrary::PachislopFieldRule* rule = titlelibrary::pachislopFieldRule(kindName, key);
        const std::string ours = rule ? rule->to : snakeCase(key);
        const SchemaMember* m = schema.member(*kind, ours);
        const std::string at = path.empty() ? key : path + "." + key;
        if (!m) {
            ctx.note(at, "'" + key + "' has no Title Art member");
            continue;
        }
        if (rule && rule->lift) continue;       // a member of the holding object (S84's `^`): its caller reads it
        if (rule && rule->component >= 0 && it.value().is_number()) {
            // one coordinate of a Vec2, the map's sign
            Json& v = out[ours];
            if (!v.is_array()) v = Json::array({0, 0});
            const double d = it.value().get<double>();
            v[rule->component] = rule->negate ? -d : d;
            continue;
        }
        if (rule && rule->negate && it.value().is_number()) {
            out[ours] = -it.value().get<double>();
            continue;
        }
        out[ours] = convertValue(it.value(), *m, ctx, at);
    }
    return out;
}
