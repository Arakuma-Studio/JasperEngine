// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The builtin font table is Pachislop's src/assets/registry.ts (BUILTIN_FONTS) by SafEight, used
// with the author's permission (ids, families and Google Fonts file names only; no font is taken).

#include "PachislopMembers.h"

#include "titleart/TitleArtSchema.h"
#if defined(JASPERENGINE_TITLE_ART_3D) && JASPERENGINE_TITLE_ART_3D
#include "titleart/three_d/Title3DConvert.h"
#endif
#include "util/FileUtils.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::pachislop;
using titleart::SchemaKind;
using titleart::SchemaMember;
using titleart::TitleArtSchema;
using titleart::ValueType;
namespace fs = std::filesystem;

namespace {

    struct BuiltinFont {
        const char* id;
        const char* family;
        const char* file;
    };

    const BuiltinFont BUILTIN_FONTS[] = {
        {"dela-gothic-one", "Dela Gothic One", "DelaGothicOne-Regular.ttf"},
        {"mplus1p-black", "M PLUS 1p Black", "MPLUS1p-Black.ttf"},
        {"zen-kaku-gothic-black", "Zen Kaku Gothic New Black", "ZenKakuGothicNew-Black.ttf"},
        {"zen-old-mincho-black", "Zen Old Mincho Black", "ZenOldMincho-Black.ttf"},
        {"yuji-syuku", "Yuji Syuku", "YujiSyuku-Regular.ttf"},
        {"reggae-one", "Reggae One", "ReggaeOne-Regular.ttf"},
        {"rocknroll-one", "RocknRoll One", "RocknRollOne-Regular.ttf"},
        {"rampart-one", "Rampart One", "RampartOne-Regular.ttf"},
        {"train-one", "Train One", "TrainOne-Regular.ttf"},
        {"playfair-black-italic", "Playfair Black Italic", "Playfair-BlackItalic.ttf"},
        {"potta-one", "Potta One", "PottaOne-Regular.ttf"},
        {"mochiy-pop-one", "Mochiy Pop One", "MochiyPopOne-Regular.ttf"},
    };

    // S84's pachislop-map.json, when the library has it
    struct MapFile {
        std::once_flag once;
        std::map<std::string, std::string> fields;
        std::map<std::string, std::string> fonts;
    };

    MapFile& mapFile() {
        static MapFile* m = new MapFile();
        std::call_once(m->once, [] {
            std::ifstream in(titleArtLibraryDir() / "pachislop-map.json", std::ios::binary);
            if (!in) return;
            std::stringstream text;
            text << in.rdbuf();
            const Json json = Json::parse(text.str(), nullptr, false);
            if (!json.is_object()) return;
            if (auto f = json.find("fields"); f != json.end() && f->is_object()) {
                for (auto it = f->begin(); it != f->end(); ++it) if (it->is_string()) m->fields[it.key()] = it->get<std::string>();
            }
            if (auto f = json.find("fonts"); f != json.end() && f->is_object()) {
                for (auto it = f->begin(); it != f->end(); ++it) {
                    if (it->is_string()) m->fonts[it.key()] = it->get<std::string>();
                    else if (it->is_object() && it->contains("id") && (*it)["id"].is_string()) m->fonts[it.key()] = (*it)["id"].get<std::string>();
                }
            }
        });
        return *m;
    }

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
        if ((key == "offsetX" || key == "offsetY") && (kind == "rim" || kind == "fill_image")) {
            if (key == "offsetX") out["offset"] = Json::array({num(object, "offsetX", 0), num(object, "offsetY", 0)});
            return true;
        }
        if ((key == "dx" || key == "dy") && (kind == "char_adjust" || kind == "line_adjust")) {
            if (key == "dx" || !object.contains("dx")) out["offset"] = Json::array({num(object, "dx", 0), num(object, "dy", 0)});
            return true;
        }
        if (key == "rotate" && (kind == "char_adjust" || kind == "fill_image")) {
            out["rotation"] = value;
            return true;
        }
        if (key == "assetId" && (kind == "fill_image" || kind == "pattern")) {
            out["image"] = value.is_string() && ctx.asset ? ctx.asset(value.get<std::string>(), at) : Json();
            return true;
        }
        if (kind == "reflection" && key == "map") {
            // the 2D maps are S84's library maps (photo environments of S86): not in this build
            if (object.value("enabled", false)) {
                ctx.note(at, "reflection map '" + (value.is_string() ? value.get<std::string>() : std::string()) +
                                 "' has no library map in this project: the reflection shows no map until one is set", "approximated");
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
    const auto& fields = mapFile().fields;
    for (auto it = p.begin(); it != p.end(); ++it) {
        const std::string& key = it.key();
        if (special(kindName, key, it.value(), p, out, ctx, path)) continue;
        auto renamed = fields.find(key);
        const std::string ours = renamed != fields.end() ? renamed->second : snakeCase(key);
        const SchemaMember* m = schema.member(*kind, ours);
        const std::string at = path.empty() ? key : path + "." + key;
        if (!m) {
            ctx.note(at, "'" + key + "' has no Title Art member");
            continue;
        }
        out[ours] = convertValue(it.value(), *m, ctx, at);
    }
    return out;
}

fs::path pachislop::titleArtLibraryDir() {
    if (const char* dir = std::getenv("JASPERENGINE_TITLEART_LIBRARY_DIR"); dir && *dir) return fs::path(dir);
    const fs::path exe = FileUtils::getExecutableDir();
#if defined(__APPLE__)
    std::error_code ec;
    if (fs::exists(exe.parent_path() / "Resources" / "titleart", ec)) return exe.parent_path() / "Resources" / "titleart";
#endif
    return exe / "titleart";
}

std::string pachislop::packFontId(const std::string& id) {
    const auto& fonts = mapFile().fonts;
    if (auto it = fonts.find(id); it != fonts.end()) return it->second;
    for (const BuiltinFont& f : BUILTIN_FONTS) if (id == f.id) return f.id;
    return "";
}

std::string pachislop::builtinFontFamily(const std::string& id) {
    for (const BuiltinFont& f : BUILTIN_FONTS) if (id == f.id) return f.family;
    return "";
}

std::string pachislop::builtinFontFile(const std::string& id) {
    for (const BuiltinFont& f : BUILTIN_FONTS) if (id == f.id) return f.file;
    return "";
}
