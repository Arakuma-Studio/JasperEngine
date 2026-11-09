// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// Ported from Pachislop src/presets/colourways.ts by SafEight, used with the author's permission.

#include "PachislopColourways.h"

#include "titleart/ColorScheme.h"
#include "titleart/three_d/Title3DConvert.h"

#include <cctype>
#include <set>

using namespace jasperengine;
using titleart::Json;

namespace {

    const char* const PACHISLOP_COMMIT = "1c32502";
    const char* const COLOURWAYS_FILE = "src/presets/colourways.ts";

    const Json NULL_JSON;

    const Json& at(const Json& o, const char* key) {
        if (!o.is_object()) return NULL_JSON;
        auto it = o.find(key);
        return it != o.end() ? *it : NULL_JSON;
    }

    std::string snake(const std::string& camel) {
        std::string out;
        for (char c : camel) {
            if (std::isupper((unsigned char)c)) {
                out += '_';
                out += (char)std::tolower((unsigned char)c);
            } else {
                out += c;
            }
        }
        return out;
    }

    // the members of a colourway that are the identity, not paint (they become name.ja / name.en)
    struct Meta {
        const char* id;
        const char* descriptionEn;
        const char* descriptionJa;
        std::vector<const char*> familyHint;
        std::vector<const char*> tags;
        std::vector<const char*> synonymsEn;
        std::vector<const char*> synonymsJa;
    };

    const std::vector<Meta>& metas() {
        static const std::vector<Meta> table = {
            {"white", "Pearl white metal with a white line and a slate frame.", "真珠のような白の金属、白の線とスレートの枠。",
             {"casino", "minimal"}, {"white", "pearl", "cool", "bright"}, {"snow", "pearl white", "ivory"}, {"しろ", "shiro", "ホワイト", "白色"}},
            {"blue", "Blue metal with a white line and a silver rim.", "青の金属、白の線と銀の縁。",
             {"casino", "gems", "metal"}, {"blue", "metal", "silver", "cool"}, {"sapphire", "azure", "navy"}, {"あお", "ao", "ブルー", "青色"}},
            {"green", "Green metal with a white line and a silver rim.", "緑の金属、白の線と銀の縁。",
             {"casino", "gems"}, {"green", "metal", "silver", "cool"}, {"emerald", "jade", "lime"}, {"みどり", "midori", "グリーン", "緑色"}},
            {"purple", "Purple metal with a white line and a gold rim.", "紫の金属、白の線と金の縁。",
             {"casino", "gems"}, {"purple", "metal", "gold", "violet"}, {"violet", "amethyst", "magenta"}, {"むらさき", "murasaki", "パープル", "紫色"}},
            {"red", "Pure red with lightness steps only, a white line and a gold rim.", "明度だけで階調をつけた純粋な赤、白の線と金の縁。",
             {"casino", "wa", "anime"}, {"red", "metal", "gold", "hot"}, {"crimson", "scarlet", "ruby"}, {"あか", "aka", "レッド", "赤色"}},
            {"gold", "White to yellow to orange to red-brown gold, a black keyline, a gold line and a red frame.",
             "白から黄、橙、赤茶への金、黒の縁、金の線と赤の枠。",
             {"casino", "metal", "wa"}, {"gold", "warm", "metal", "yellow"}, {"yellow", "gilded", "golden"}, {"きん", "kin", "ゴールド", "黄金"}},
            {"silver", "Chrome silver with a white line and a blue metal frame.", "クロームの銀、白の線と青の金属の枠。",
             {"metal", "scifi", "casino"}, {"silver", "chrome", "metal", "cool"}, {"chrome", "steel", "platinum"}, {"ぎん", "gin", "シルバー", "銀色"}},
            {"zebra", "Black zebra stripes on pearl silver, a white line and a gold rim.", "真珠の銀に黒のゼブラ柄、白の線と金の縁。",
             {"casino"}, {"zebra", "pattern", "animal", "stripes", "silver"}, {"stripes", "animal print", "zebra print"},
             {"しまうま", "shimauma", "ゼブラ柄", "縞"}},
            {"giraffe", "Brown giraffe patches on gold, a cream line and a copper rim.", "金にキリンの茶の斑、クリームの線と銅の縁。",
             {"casino"}, {"giraffe", "pattern", "animal", "gold"}, {"animal print", "patches", "giraffe print"}, {"きりん", "kirin", "キリン柄"}},
            {"leopard", "Leopard rosettes on gold, a white line and a red rim.", "金にヒョウ柄の斑、白の線と赤の縁。",
             {"casino"}, {"leopard", "pattern", "animal", "gold", "spots"}, {"animal print", "spots", "cheetah"}, {"ひょう", "hyou", "ヒョウ柄", "豹柄"}},
            {"rainbow", "The full spectrum on every face and every light, a white line and a gold rim.", "すべての面と光に虹のスペクトル、白の線と金の縁。",
             {"casino"}, {"rainbow", "spectrum", "colorful", "multicolor"}, {"spectrum", "prism", "multicolour"}, {"にじ", "niji", "レインボー", "七色"}},
        };
        return table;
    }

    const Meta* metaOf(const std::string& id) {
        for (const Meta& m : metas()) {
            if (id == m.id) return &m;
        }
        return nullptr;
    }

    Json strings(const std::vector<const char*>& list) {
        Json out = Json::array();
        for (const char* s : list) out.push_back(s);
        return out;
    }

    Json gradientOf(const Json& g, const std::string& path, std::vector<std::string>& notes) {
        if (!g.is_object()) return nullptr;
        Json out = Json::object();
        Json stops = Json::array();
        for (const Json& s : at(g, "stops")) {
            Json stop = Json::object();
            for (const char* key : {"pos", "color", "alpha", "mid"}) {
                if (s.contains(key)) stop[key] = s[key];
            }
            stops.push_back(stop);
        }
        out["stops"] = stops;
        if (g.contains("interp")) out["interp"] = g["interp"];
        for (auto it = g.begin(); it != g.end(); ++it) {
            if (it.key() != "stops" && it.key() != "interp") notes.push_back(path + "." + it.key() + ": not carried");
        }
        return out;
    }

    Json fillOf(const Json& f, const std::string& path, std::vector<std::string>& notes) {
        Json out = Json::object();
        if (!f.is_object()) return out;
        for (auto it = f.begin(); it != f.end(); ++it) {
            const std::string& key = it.key();
            if (key == "kind") out["kind"] = snake(it.value().get<std::string>());
            else if (key == "gradient") out["gradient"] = gradientOf(it.value(), path + ".gradient", notes);
            else if (key == "color" || key == "angle" || key == "scale" || key == "offset" || key == "span") out[key] = it.value();
            else notes.push_back(path + "." + key + ": not carried");
        }
        return out;
    }

    Json patternOf(const Json& p, const std::string& path, std::vector<std::string>& notes) {
        if (!p.is_object()) return nullptr;
        Json out = Json::object();
        for (auto it = p.begin(); it != p.end(); ++it) {
            const std::string& key = it.key();
            if (key == "kind") out["kind"] = snake(it.value().get<std::string>());
            else if (key == "color" || key == "color2" || key == "opacity" || key == "scale" || key == "angle" || key == "seed" || key == "shade" ||
                     key == "relief") out[key] = it.value();
            else notes.push_back(path + "." + key + ": not carried");
        }
        return out;
    }

    // Title3DConvert maps every member of Pachislop's Material3D; a key it does not know is named
    Json materialOf(const Json& m, const std::string& path, std::vector<std::string>& notes) {
        static const std::set<std::string> known = {
            "preset", "color", "gradient", "gradientKind", "gradientAngle", "gradientSpan", "gradientScale", "gradientOffset", "gradientAxis",
            "gradientBend", "metalness", "roughness", "reflectivity", "specBoost", "clearcoat", "clearcoatRoughness", "iridescence",
            "iridescenceIor", "transmission", "ior", "dispersion", "glassDepth", "flakes", "flakeScale", "pattern", "surface", "emissive",
            "emissiveIntensity", "opacity", "environment"};
        if (m.is_object()) {
            for (auto it = m.begin(); it != m.end(); ++it) {
                if (!known.count(it.key())) notes.push_back(path + "." + it.key() + ": not carried");
            }
        }
        return titleart::material3DFromPachislop(m);
    }

    Json paintOf(const Json& p, const std::string& path, std::vector<std::string>& notes) {
        Json out = Json::object();
        out["fill"] = fillOf(at(p, "fill"), path + ".fill", notes);
        out["ramp"] = gradientOf(at(p, "ramp"), path + ".ramp", notes);
        out["material_3d"] = materialOf(at(p, "m3d"), path + ".m3d", notes);
        if (at(p, "shade").is_string()) out["shade"] = p["shade"];
        if (at(p, "pattern").is_object()) out["pattern"] = patternOf(p["pattern"], path + ".pattern", notes);
        for (auto it = p.begin(); it != p.end(); ++it) {
            const std::string& key = it.key();
            if (key != "fill" && key != "ramp" && key != "m3d" && key != "shade" && key != "pattern") notes.push_back(path + "." + key + ": not carried");
        }
        return out;
    }

}

const std::vector<std::string>& editor::pachislopColourwayIds() {
    static const std::vector<std::string> ids = {"white", "blue", "green", "purple", "red", "gold", "silver", "zebra", "giraffe", "leopard", "rainbow"};
    return ids;
}

Json editor::colorSchemeFromPachislop(const std::string& colourwayId, const Json& dump, std::vector<std::string>& notes) {
    const Json& entry = at(at(dump, "COLOURWAY_DEFS"), colourwayId.c_str());
    const Json& def = at(entry, "def");
    const Meta* meta = metaOf(colourwayId);
    if (!def.is_object() || !meta) {
        notes.push_back(colourwayId + ": no such colourway in the dump");
        return nullptr;
    }
    const std::string base = "COLOURWAY_DEFS." + colourwayId;

    Json j = Json::object();
    j["format"] = titleart::TITLE_COLORS_FORMAT;
    j["version"] = titleart::TITLE_COLORS_VERSION;
    j["id"] = colourwayId;
    j["revision"] = 1;
    j["name"] = Json{{"en", at(def, "name")}, {"ja", at(def, "label")}};
    j["description"] = Json{{"en", meta->descriptionEn}, {"ja", meta->descriptionJa}};
    j["family_hint"] = strings(meta->familyHint);
    j["tags"] = strings(meta->tags);
    j["synonyms"] = Json{{"en", strings(meta->synonymsEn)}, {"ja", strings(meta->synonymsJa)}};

    Json paints = Json::object();
    paints["face"] = paintOf(at(def, "face"), base + ".face", notes);
    paints["dark"] = paintOf(at(def, "dark"), base + ".dark", notes);
    Json accents = Json::array();
    const Json& list = at(def, "accents");
    for (size_t i = 0; list.is_array() && i < list.size(); i++) accents.push_back(paintOf(list[i], base + ".accents[" + std::to_string(i) + "]", notes));
    paints["accents"] = accents;
    paints["base"] = paintOf(at(def, "base"), base + ".base", notes);
    if (at(def, "bevel3d").is_object()) paints["bevel_rim_3d"] = paintOf(def["bevel3d"], base + ".bevel3d", notes);
    paints["side_3d"] = materialOf(at(def, "side3d"), base + ".side3d", notes);
    j["paints"] = paints;

    j["face_glow"] = at(def, "faceGlow");
    j["glow"] = at(def, "glow");
    j["glow_gradient"] = gradientOf(at(def, "glowGradient"), base + ".glowGradient", notes);
    j["shadow"] = at(def, "shadow");
    j["glint"] = at(def, "glint");
    j["light_ramp"] = at(at(at(dump, "LIGHT"), colourwayId.c_str()), "ramp");
    const Json& orb = at(def, "orb");
    j["orb"] = Json{{"ramp", gradientOf(at(orb, "ramp"), base + ".orb.ramp", notes)}, {"radial", at(orb, "radial").is_boolean() ? orb["radial"].get<bool>() : true}};
    if (at(def, "patternGround").is_string()) j["pattern_ground"] = def["patternGround"];
    // Pachislop's rainbow behaviour is keyed by its id (colourway === 'rainbow')
    j["spectrum"] = colourwayId == "rainbow";
    j["swatch"] = at(at(at(dump, "COLOURWAY_INFO"), colourwayId.c_str()), "swatch");
    Json provenance{{"source", "pachislop"}, {"commit", PACHISLOP_COMMIT}, {"file", COLOURWAYS_FILE}};
    if (at(entry, "line").is_number_integer()) provenance["line"] = entry["line"];
    j["provenance"] = provenance;

    static const std::set<std::string> carried = {"label", "name", "face", "dark", "accents", "base", "bevel3d", "side3d", "faceGlow", "glow",
                                                  "glowGradient", "shadow", "glint", "orb", "patternGround"};
    for (auto it = def.begin(); it != def.end(); ++it) {
        if (it.key() == "swatchPattern") notes.push_back(base + ".swatchPattern: a CSS swatch of the UI, not carried (the swatch colours are)");
        else if (!carried.count(it.key())) notes.push_back(base + "." + it.key() + ": not carried");
    }

    titleart::ColorScheme scheme;
    std::vector<std::string> warnings;
    if (!titleart::parseColorScheme(j, scheme, &warnings)) {
        for (const std::string& w : warnings) notes.push_back(colourwayId + ": " + w);
        return nullptr;
    }
    for (const std::string& w : warnings) notes.push_back(colourwayId + ": " + w);
    return titleart::writeColorScheme(scheme);
}
