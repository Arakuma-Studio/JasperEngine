// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLibraryFormats.h"

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtSchema.h"
#include "titleart/TitleEffectRegistry.h"

#include <map>
#include <mutex>
#include <regex>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
using titleart::SchemaKind;
using titleart::SchemaMember;
using titleart::TitleArtSchema;
using titleart::ValueType;

namespace {

    const Json NULL_JSON = nullptr;

    // the default object of a kind (and variant), made once
    const Json& defaultOf(const std::string& kind, const std::string& variant) {
        static std::mutex lock;
        static std::map<std::string, Json> cache;
        std::lock_guard<std::mutex> guard(lock);
        const std::string key = kind + "\n" + variant;
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        return cache.emplace(key, TitleArtSchema::get().defaultObject(kind, variant)).first->second;
    }

    Json paramsDiff(const std::vector<titleart::ParamDef>* defs, const Json& params) {
        if (!params.is_object()) return params;
        Json out = Json::object();
        for (auto it = params.begin(); it != params.end(); ++it) {
            const titleart::ParamDef* def = defs ? titleart::findParam(*defs, it.key()) : nullptr;
            if (def && it.value() == def->defaultValue) continue;
            out[it.key()] = it.value();
        }
        return out;
    }

    Json sparseAgainst(const Json& value, const SchemaKind& kind, const Json& defaults, bool dropIds);

    // the sparse form of one member value (null result + `omit` when it equals the default)
    Json sparseMember(const SchemaMember& m, const Json& v, const Json& d, const Json& siblings, bool dropIds, bool& omit) {
        const TitleArtSchema& schema = TitleArtSchema::get();
        omit = false;
        switch (m.type) {
            case ValueType::Object: {
                if (v.is_null()) {
                    omit = d.is_null();
                    return v;
                }
                if (!v.is_object()) return v;
                const SchemaKind* child = schema.kind(m.kind);
                if (!child) {
                    omit = v == d;
                    return v;
                }
                const SchemaKind* variant = child->discriminator.empty() ? child : schema.variantOf(*child, v);
                if (!variant) return v;
                const std::string variantName = child->discriminator.empty() ? std::string() : v.value(child->discriminator, std::string());
                // what a missing member of a present object reads as is the KIND's default (normalize),
                // not the member's own default (only an absent object takes that: style_3d.face, a
                // band's bevel, a curve's points), so the object is written against the kind's
                Json sub = sparseAgainst(v, *variant, defaultOf(m.kind, variantName), dropIds);
                omit = d.is_object() && v == d;
                return sub;
            }
            case ValueType::ObjectArray: {
                if (!v.is_array()) return v;
                Json out = Json::array();
                for (const Json& item : v) out.push_back(item.is_object() ? sparseObject(item, m.kind, dropIds) : item);
                Json defaultSparse = Json::array();
                if (d.is_array()) {
                    for (const Json& item : d) defaultSparse.push_back(item.is_object() ? sparseObject(item, m.kind, dropIds) : item);
                }
                omit = out == defaultSparse;
                return out;
            }
            case ValueType::ObjectMap: {
                if (!v.is_object()) return v;
                Json out = Json::object();
                for (auto it = v.begin(); it != v.end(); ++it) {
                    out[it.key()] = it.value().is_object() ? sparseObject(it.value(), m.kind, dropIds) : it.value();
                }
                omit = out.empty() && (d.is_null() || d.empty());
                return out;
            }
            case ValueType::ObjectArrayMap: {
                if (!v.is_object()) return v;
                Json out = Json::object();
                for (auto it = v.begin(); it != v.end(); ++it) {
                    Json list = Json::array();
                    if (it.value().is_array()) {
                        for (const Json& item : it.value()) list.push_back(item.is_object() ? sparseObject(item, m.kind, dropIds) : item);
                    }
                    out[it.key()] = std::move(list);
                }
                omit = out.empty() && (d.is_null() || d.empty());
                return out;
            }
            case ValueType::EffectParams:
            case ValueType::GeneratorParams: {
                const std::string type = siblings.value("type", std::string());
                const std::vector<titleart::ParamDef>* defs = nullptr;
                if (m.type == ValueType::EffectParams) {
                    if (const titleart::EffectDef* def = titleart::findEffect(type)) defs = &def->params;
                } else if (const titleart::GeneratorDef* def = titleart::findGenerator(type)) {
                    defs = &def->params;
                }
                if (!defs) return v;        // an unknown type keeps its params verbatim
                Json out = paramsDiff(defs, v);
                omit = out.empty();
                return out;
            }
            default:
                omit = v == d;
                return v;
        }
    }

    Json sparseAgainst(const Json& value, const SchemaKind& kind, const Json& defaults, bool dropIds) {
        const TitleArtSchema& schema = TitleArtSchema::get();
        Json out = Json::object();
        std::set<std::string> known;
        for (const SchemaMember* m : schema.membersOf(kind)) {
            known.insert(m->key);
            auto it = value.find(m->key);
            if (it == value.end()) continue;
            if (m->type == ValueType::Id) {
                if (!dropIds) out[m->key] = *it;
                continue;
            }
            // the discriminator and type names stay: they say what the defaults are
            if (m->key == "type" || m->key == kind.discriminator) {
                out[m->key] = *it;
                continue;
            }
            auto d = defaults.find(m->key);
            bool omit = false;
            Json sparse = sparseMember(*m, *it, d == defaults.end() ? NULL_JSON : *d, value, dropIds, omit);
            if (!omit) out[m->key] = std::move(sparse);
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!known.count(it.key())) out[it.key()] = it.value();
        }
        return out;
    }

    // ---- canonical look members ----

    void warn(std::vector<LibraryWarning>* warnings, const std::string& code, const std::string& path, const std::string& message) {
        if (warnings) warnings->push_back({code, path, message});
    }

    Json localizedJson(const Json& value) {
        const LocalizedText t = decodeLocalized(value);
        return t.toJson();
    }

    Json stringList(const Json& value, const std::string& path, std::vector<LibraryWarning>* warnings) {
        Json out = Json::array();
        if (value.is_null()) return out;
        if (!value.is_array()) {
            warn(warnings, "bad_member", path, "not a list: dropped");
            return out;
        }
        for (size_t i = 0; i < value.size(); i++) {
            if (value[i].is_string()) out.push_back(value[i]);
            else warn(warnings, "bad_member", path + "[" + std::to_string(i) + "]", "not a string: dropped");
        }
        return out;
    }

    Json synonymsJson(const Json& value, const std::string& path, std::vector<LibraryWarning>* warnings) {
        Json out = Json::object();
        out["en"] = Json::array();
        out["ja"] = Json::array();
        if (value.is_object()) {
            out["en"] = stringList(value.contains("en") ? value["en"] : NULL_JSON, path + ".en", warnings);
            out["ja"] = stringList(value.contains("ja") ? value["ja"] : NULL_JSON, path + ".ja", warnings);
        } else if (!value.is_null()) {
            warn(warnings, "bad_member", path, "not {en, ja}: dropped");
        }
        return out;
    }

    Json allowList(const Json& value, const char* key, const std::string& path, std::vector<LibraryWarning>* warnings) {
        Json out = Json::array();
        if (value.is_null()) return out;
        if (!value.is_array()) {
            warn(warnings, "bad_member", path, "not a list: dropped");
            return out;
        }
        for (size_t i = 0; i < value.size(); i++) {
            const Json& e = value[i];
            if (!e.is_object() || !e.contains(key) || !e[key].is_string()) {
                warn(warnings, "bad_member", path + "[" + std::to_string(i) + "]", std::string("needs `") + key + "` and `reason`: dropped");
                continue;
            }
            Json entry = Json::object();
            entry[key] = e[key];
            entry["reason"] = e.value("reason", std::string());
            if (entry["reason"].get<std::string>().empty()) warn(warnings, "allow_without_reason", path + "[" + std::to_string(i) + "]", "an allowlist entry needs a reason");
            out.push_back(std::move(entry));
        }
        return out;
    }

    Json effectList(const Json& value, const std::string& path, std::vector<LibraryWarning>* warnings) {
        Json out = Json::array();
        if (value.is_null()) return out;
        if (!value.is_array()) {
            warn(warnings, "bad_member", path, "not a list: dropped");
            return out;
        }
        for (size_t i = 0; i < value.size(); i++) {
            if (!value[i].is_object() || !value[i].contains("type") || !value[i]["type"].is_string()) {
                warn(warnings, "bad_member", path + "[" + std::to_string(i) + "]", "an effect needs a `type`: dropped");
                continue;
            }
            out.push_back(sparseEffect(value[i], true));
        }
        return out;
    }

    bool isPositionalPartKey(const std::string& key) {
        static const std::regex pattern("^(face|bevel|side|(rim|band)#[0-9]+)$");
        return std::regex_match(key, pattern);
    }

    Json generatorEntry(const Json& g, const std::string& path, std::vector<LibraryWarning>* warnings, bool& ok) {
        ok = g.is_object() && g.contains("type") && g["type"].is_string();
        if (!ok) {
            warn(warnings, "bad_member", path, "a generator needs a `type`: dropped");
            return nullptr;
        }
        const std::string type = g["type"].get<std::string>();
        Json out = Json::object();
        out["type"] = type;
        if (g.contains("preset") && g["preset"].is_string() && !g["preset"].get<std::string>().empty()) out["preset"] = g["preset"];
        const std::string place = g.value("place", std::string("above"));
        out["place"] = place == "below" ? "below" : "above";
        if (place != "above" && place != "below") warn(warnings, "bad_member", path + ".place", "`" + place + "` is not above or below: above");
        const std::string scope = g.value("scope", std::string("layer"));
        out["scope"] = scope == "group" ? "group" : "layer";
        if (scope != "layer" && scope != "group") warn(warnings, "bad_member", path + ".scope", "`" + scope + "` is not layer or group: layer");
        if (g.contains("name") && g["name"].is_string() && !g["name"].get<std::string>().empty()) out["name"] = g["name"];
        out["blend"] = g.value("blend", std::string("normal"));
        out["opacity"] = g.contains("opacity") && g["opacity"].is_number() ? g["opacity"] : Json(1);
        out["params"] = sparseGeneratorParams(type, g.contains("params") ? g["params"] : Json::object());
        out["effects"] = effectList(g.contains("effects") ? g["effects"] : NULL_JSON, path + ".effects", warnings);
        static const std::set<std::string> known = {"type", "preset", "place", "scope", "name", "blend", "opacity", "params", "effects"};
        for (auto it = g.begin(); it != g.end(); ++it) {
            if (!known.count(it.key())) out[it.key()] = it.value();
        }
        return out;
    }

    // the style's part counts: positional keys it has
    void partKeysOfStyle(const Json& look, std::set<std::string>& keys) {
        keys.insert("face");
        if (look.contains("style") && look["style"].is_object()) {
            const Json& style = look["style"];
            keys.insert("bevel");
            if (style.contains("rims") && style["rims"].is_array()) {
                for (size_t i = 0; i < style["rims"].size(); i++) keys.insert("rim#" + std::to_string(i));
            }
        }
        if (look.contains("style_3d") && look["style_3d"].is_object()) {
            const Json& style = look["style_3d"];
            keys.insert("bevel");
            keys.insert("side");
            if (style.contains("bands") && style["bands"].is_array()) {
                for (size_t i = 0; i < style["bands"].size(); i++) keys.insert("band#" + std::to_string(i));
            }
        }
    }

    const char* const LOOK_ORDER[] = {"format", "version", "id", "revision", "name", "description", "family", "tags", "synonyms",
                                      "dimension", "applies", "ref_size", "background", "font", "thumbnail_texts", "style", "style_3d", "effects",
                                      "part_effects", "generators", "group_effects", "lighting", "links", "lint_allow", "check_allow",
                                      "assets", "provenance"};

    Json provenanceJson(const Json& value) {
        if (!value.is_object()) return Json::object();
        Json out = Json::object();
        for (const char* key : {"source", "commit", "file", "line", "item", "author", "license_note"}) {
            if (value.contains(key)) out[key] = value[key];
        }
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (!out.contains(it.key())) out[it.key()] = it.value();
        }
        return out;
    }

    std::string intText(int v) { return std::to_string(v); }

}

// ---- LocalizedText ----

Json LocalizedText::toJson() const {
    Json out = Json::object();
    if (!en.empty()) out["en"] = en;
    if (!ja.empty()) out["ja"] = ja;
    return out;
}

bool titlelibrary::hasJapaneseScript(std::string_view text) {
    // decode UTF-8 and look for kana (3040-30ff), CJK punctuation (3000-303f), kanji (4e00-9fff,
    // 3400-4dbf), half-width katakana (ff66-ff9f)
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = (unsigned char)text[i];
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0xF0 && i + 3 < text.size() + 0) {
            cp = ((c & 0x07u) << 18) | (((unsigned char)text[i + 1] & 0x3Fu) << 12) | (((unsigned char)text[i + 2] & 0x3Fu) << 6) | ((unsigned char)text[i + 3] & 0x3Fu);
            n = 4;
        } else if (c >= 0xE0 && i + 2 < text.size()) {
            cp = ((c & 0x0Fu) << 12) | (((unsigned char)text[i + 1] & 0x3Fu) << 6) | ((unsigned char)text[i + 2] & 0x3Fu);
            n = 3;
        } else if (c >= 0xC0 && i + 1 < text.size()) {
            cp = ((c & 0x1Fu) << 6) | ((unsigned char)text[i + 1] & 0x3Fu);
            n = 2;
        }
        if ((cp >= 0x3000 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xFF66 && cp <= 0xFF9F)) return true;
        i += n;
    }
    return false;
}

LocalizedText titlelibrary::decodeLocalized(const Json& value) {
    LocalizedText out;
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        (hasJapaneseScript(s) ? out.ja : out.en) = s;
    } else if (value.is_object()) {
        if (value.contains("en") && value["en"].is_string()) out.en = value["en"].get<std::string>();
        if (value.contains("ja") && value["ja"].is_string()) out.ja = value["ja"].get<std::string>();
    }
    return out;
}

std::string LibraryWarning::toString() const {
    return (path.empty() ? std::string() : path + ": ") + message;
}

titleart::TitleJsonMigrations& titlelibrary::titleLookMigrations() {
    static titleart::TitleJsonMigrations table(TITLE_LOOK_FORMAT, 1);
    return table;
}

// ---- sparse forms ----

Json titlelibrary::sparseObject(const Json& full, std::string_view kindName, bool dropIds) {
    const TitleArtSchema& schema = TitleArtSchema::get();
    const SchemaKind* kind = schema.kind(kindName);
    if (!kind || !full.is_object()) return full;
    const SchemaKind* variant = kind->discriminator.empty() ? kind : schema.variantOf(*kind, full);
    if (!variant) return full;      // an unknown type: kept verbatim
    const std::string variantName = kind->discriminator.empty() ? std::string() : full.value(kind->discriminator, std::string());
    if (std::string(kindName) == "effect") return sparseEffect(full, dropIds);
    return sparseAgainst(full, *variant, defaultOf(std::string(kindName), variantName), dropIds);
}

Json titlelibrary::sparseEffect(const Json& effect, bool dropIds) {
    if (!effect.is_object()) return effect;
    Json out = Json::object();
    if (!dropIds && effect.contains("id")) out["id"] = effect["id"];
    const std::string type = effect.value("type", std::string());
    out["type"] = type;
    if (effect.contains("enabled") && effect["enabled"].is_boolean() && !effect["enabled"].get<bool>()) out["enabled"] = false;
    const titleart::EffectDef* def = titleart::findEffect(type);
    out["params"] = paramsDiff(def ? &def->params : nullptr, effect.contains("params") ? effect["params"] : Json::object());
    if (effect.value("from_look", false)) out["from_look"] = true;
    static const std::set<std::string> known = {"id", "type", "enabled", "params", "from_look"};
    for (auto it = effect.begin(); it != effect.end(); ++it) {
        if (!known.count(it.key())) out[it.key()] = it.value();
    }
    return out;
}

Json titlelibrary::sparseGeneratorParams(const std::string& type, const Json& params) {
    const titleart::GeneratorDef* def = titleart::findGenerator(type);
    return paramsDiff(def ? &def->params : nullptr, params);
}

Json titlelibrary::expandObject(const Json& sparse, std::string_view kind, titleart::TitleIdGenerator* ids) {
    Json out = sparse.is_object() ? sparse : Json::object();
    titleart::NormalizeContext context;
    context.ids = ids;
    titleart::normalizeTitleArtObject(out, std::string(kind), "", context);
    return out;
}

// ---- Look ----

const Json& TitleLook::member(const char* key) const {
    auto it = root.find(key);
    return it == root.end() ? NULL_JSON : *it;
}
std::string TitleLook::id() const { return root.value("id", std::string()); }
int TitleLook::revision() const { return root.value("revision", 1); }
LocalizedText TitleLook::name() const { return decodeLocalized(member("name")); }
LocalizedText TitleLook::description() const { return decodeLocalized(member("description")); }
std::string TitleLook::family() const { return root.value("family", std::string()); }
std::vector<std::string> TitleLook::tags() const {
    std::vector<std::string> out;
    for (const Json& t : member("tags")) if (t.is_string()) out.push_back(t.get<std::string>());
    return out;
}
std::string TitleLook::dimension() const { return root.value("dimension", std::string("2d")); }
std::string TitleLook::applies() const { return root.value("applies", std::string("style")); }
double TitleLook::refSize() const {
    const Json& r = member("ref_size");
    return r.is_number() && r.get<double>() > 0 ? r.get<double>() : TITLE_LOOK_DEFAULT_REF_SIZE;
}

Json titlelibrary::canonicalLookJson(const Json& root, std::vector<LibraryWarning>* warnings) {
    Json out = Json::object();
    out["format"] = TITLE_LOOK_FORMAT;
    out["version"] = root.contains("version") && root["version"].is_number_integer() ? root["version"] : Json(TITLE_LOOK_VERSION);
    out["id"] = root.value("id", std::string());
    if (!root.contains("id") || !root["id"].is_string()) warn(warnings, "bad_member", "id", "a look needs a string `id`");
    int revision = 1;
    if (root.contains("revision")) {
        if (root["revision"].is_number_integer() && root["revision"].get<int>() >= 1) revision = root["revision"].get<int>();
        else warn(warnings, "bad_member", "revision", "not a whole number >= 1: 1");
    }
    out["revision"] = revision;
    out["name"] = localizedJson(root.contains("name") ? root["name"] : NULL_JSON);
    out["description"] = localizedJson(root.contains("description") ? root["description"] : NULL_JSON);
    out["family"] = root.contains("family") && root["family"].is_string() ? root["family"] : Json("custom");
    out["tags"] = stringList(root.contains("tags") ? root["tags"] : NULL_JSON, "tags", warnings);
    if (out["tags"].size() > TITLE_LIBRARY_MAX_TAGS) warn(warnings, "bad_member", "tags", "more than " + std::to_string(TITLE_LIBRARY_MAX_TAGS) + " tags");
    out["synonyms"] = synonymsJson(root.contains("synonyms") ? root["synonyms"] : NULL_JSON, "synonyms", warnings);
    const std::string dimension = root.value("dimension", std::string("2d"));
    if (dimension != "2d" && dimension != "3d") warn(warnings, "bad_member", "dimension", "`" + dimension + "` is not 2d or 3d: 2d");
    out["dimension"] = dimension == "3d" ? "3d" : "2d";
    const std::string applies = root.value("applies", std::string("style"));
    if (applies != "style" && applies != "effects") warn(warnings, "bad_member", "applies", "`" + applies + "` is not style or effects: style");
    out["applies"] = applies == "effects" ? "effects" : "style";
    double refSize = TITLE_LOOK_DEFAULT_REF_SIZE;
    if (root.contains("ref_size")) {
        if (root["ref_size"].is_number() && root["ref_size"].get<double>() > 0) refSize = root["ref_size"].get<double>();
        else warn(warnings, "bad_member", "ref_size", "not a positive number: 200");
    }
    out["ref_size"] = refSize;
    if (root.contains("font") && !root["font"].is_null()) {
        if (root["font"].is_object()) out["font"] = root["font"];
        else warn(warnings, "bad_member", "font", "not {latin, ja}: dropped");
    }
    if (root.contains("thumbnail_texts") && root["thumbnail_texts"].is_object()) out["thumbnail_texts"] = root["thumbnail_texts"];
    const bool styleLook = out["applies"] == "style";
    if (styleLook) {
        const char* styleKey = out["dimension"] == "3d" ? "style_3d" : "style";
        const char* kind = out["dimension"] == "3d" ? "style_3d" : "style2d";
        const char* otherKey = out["dimension"] == "3d" ? "style" : "style_3d";
        if (root.contains(otherKey) && !root[otherKey].is_null()) warn(warnings, "bad_member", otherKey, std::string("a ") + out["dimension"].get<std::string>() + " look carries `" + styleKey + "`: dropped");
        const Json style = root.contains(styleKey) && root[styleKey].is_object() ? root[styleKey] : Json::object();
        if (TitleArtSchema::get().kind(kind)) {
            // normalize first (types, clamps, the default-filled form), then drop what equals it
            Json full = expandObject(style, kind, nullptr);
            out[styleKey] = sparseObject(full, kind, true);
        } else {
            out[styleKey] = style;      // S86 compiled out: kept verbatim
        }
    } else {
        for (const char* key : {"style", "style_3d", "part_effects"}) {
            if (root.contains(key) && !root[key].is_null() && !(root[key].is_object() && root[key].empty())) {
                warn(warnings, "bad_member", key, std::string("an `applies: effects` look carries no `") + key + "`: dropped");
            }
        }
    }
    out["effects"] = effectList(root.contains("effects") ? root["effects"] : NULL_JSON, "effects", warnings);
    Json parts = Json::object();
    if (styleLook && root.contains("part_effects") && root["part_effects"].is_object()) {
        std::set<std::string> keys;
        partKeysOfStyle(out, keys);
        std::map<std::string, Json> sorted;
        for (auto it = root["part_effects"].begin(); it != root["part_effects"].end(); ++it) {
            const std::string at = "part_effects." + it.key();
            if (!isPositionalPartKey(it.key())) {
                warn(warnings, "bad_part_key", at, "not a positional part key (face, rim#n, band#n, bevel, side): dropped");
                continue;
            }
            if (!keys.count(it.key())) warn(warnings, "unknown_part_key", at, "the look's own style has no part `" + it.key() + "`");
            sorted[it.key()] = effectList(it.value(), at, warnings);
        }
        for (auto& [k, v] : sorted) parts[k] = std::move(v);
    }
    out["part_effects"] = parts;
    Json generators = Json::array();
    if (root.contains("generators") && root["generators"].is_array()) {
        for (size_t i = 0; i < root["generators"].size(); i++) {
            bool ok = false;
            Json g = generatorEntry(root["generators"][i], "generators[" + std::to_string(i) + "]", warnings, ok);
            if (ok) generators.push_back(std::move(g));
        }
    }
    out["generators"] = generators;
    out["group_effects"] = effectList(root.contains("group_effects") ? root["group_effects"] : NULL_JSON, "group_effects", warnings);
    if (root.contains("lighting") && root["lighting"].is_object()) {
        // a complete override (S86's sparse kind: absent members would inherit): normalized, not thinned
        Json lighting = root["lighting"];
        if (TitleArtSchema::get().kind("lighting_3d")) {
            titleart::NormalizeContext context;
            titleart::normalizeTitleArtObject(lighting, "lighting_3d", "lighting", context);
        }
        out["lighting"] = lighting;
    }
    Json links = Json::object();
    if (root.contains("links") && root["links"].is_object()) {
        std::map<std::string, Json> sorted;
        for (auto it = root["links"].begin(); it != root["links"].end(); ++it) {
            Json entry = Json::object();
            entry["name"] = it.value().is_object() ? it.value().value("name", it.key()) : it.key();
            sorted[it.key()] = entry;
        }
        for (auto& [k, v] : sorted) links[k] = std::move(v);
    }
    out["links"] = links;
    out["lint_allow"] = allowList(root.contains("lint_allow") ? root["lint_allow"] : NULL_JSON, "rule", "lint_allow", warnings);
    out["check_allow"] = allowList(root.contains("check_allow") ? root["check_allow"] : NULL_JSON, "check", "check_allow", warnings);
    if (root.contains("assets") && root["assets"].is_array() && !root["assets"].empty()) out["assets"] = root["assets"];
    out["provenance"] = provenanceJson(root.contains("provenance") ? root["provenance"] : NULL_JSON);
    std::set<std::string> known(std::begin(LOOK_ORDER), std::end(LOOK_ORDER));
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!known.count(it.key())) out[it.key()] = it.value();
    }
    return out;
}

LookParseResult titlelibrary::parseLookJson(Json root) {
    LookParseResult result;
    if (!root.is_object()) {
        result.errorCode = "not_object";
        result.error = "a look is a JSON object";
        return result;
    }
    if (root.value("format", std::string()) != TITLE_LOOK_FORMAT) {
        result.errorCode = "bad_format";
        result.error = std::string("`format` is not \"") + TITLE_LOOK_FORMAT + "\"";
        return result;
    }
    if (!root.contains("version") || !root["version"].is_number_integer() || root["version"].get<int>() < 1) {
        result.errorCode = "bad_version";
        result.error = "`version` is not a whole number >= 1";
        return result;
    }
    const int version = root["version"].get<int>();
    if (version > TITLE_LOOK_VERSION) {
        // a newer editor's file: listed and applied as it reads, never rewritten
        result.ok = true;
        result.look.root = root;
        result.look.readOnly = true;
        result.warnings.push_back({"version_newer", "version", "written by a newer editor (version " + intText(version) + ", this one reads " +
                                                                    intText(TITLE_LOOK_VERSION) + "): read-only"});
        return result;
    }
    titleart::MigrationLog log;
    std::string error;
    if (version < titleLookMigrations().latestVersion() &&
        !titleLookMigrations().run(root, version, titleLookMigrations().latestVersion(), log, &error)) {
        result.errorCode = "migration_failed";
        result.error = error;
        return result;
    }
    for (const std::string& note : log.notes) result.warnings.push_back({"migrated", "", note});
    result.look.root = canonicalLookJson(root, &result.warnings);
    result.look.root["version"] = TITLE_LOOK_VERSION;
    result.ok = true;
    return result;
}

LookParseResult titlelibrary::parseLook(const std::string& text) {
    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded()) {
        LookParseResult result;
        result.errorCode = "not_json";
        result.error = "not JSON";
        return result;
    }
    return parseLookJson(std::move(root));
}

std::string titlelibrary::writeLook(const TitleLook& look) {
    if (look.readOnly) return titleart::writeTitleArtJson(look.root) + "\n";
    return titleart::writeTitleArtJson(canonicalLookJson(look.root, nullptr)) + "\n";
}

// ---- Template ----

std::string TitleTemplate::id() const { return meta.value("id", std::string()); }
int TitleTemplate::revision() const { return meta.value("revision", 1); }
LocalizedText TitleTemplate::name() const { return decodeLocalized(meta.contains("name") ? meta["name"] : NULL_JSON); }
std::string TitleTemplate::family() const { return meta.value("family", std::string()); }
std::vector<std::string> TitleTemplate::tags() const {
    std::vector<std::string> out;
    if (meta.contains("tags")) for (const Json& t : meta["tags"]) if (t.is_string()) out.push_back(t.get<std::string>());
    return out;
}
const Json& TitleTemplate::placeholders() const {
    static const Json empty = Json::array();
    auto it = meta.find("placeholders");
    return it != meta.end() && it->is_array() ? *it : empty;
}

namespace {

    Json canonicalTemplateMeta(const Json& meta, std::vector<LibraryWarning>* warnings) {
        Json out = Json::object();
        out["version"] = meta.contains("version") && meta["version"].is_number_integer() ? meta["version"] : Json(TITLE_TEMPLATE_MEMBER_VERSION);
        out["id"] = meta.value("id", std::string());
        out["revision"] = meta.contains("revision") && meta["revision"].is_number_integer() ? meta["revision"] : Json(1);
        out["name"] = localizedJson(meta.contains("name") ? meta["name"] : NULL_JSON);
        out["description"] = localizedJson(meta.contains("description") ? meta["description"] : NULL_JSON);
        out["family"] = meta.contains("family") && meta["family"].is_string() ? meta["family"] : Json("custom");
        out["tags"] = stringList(meta.contains("tags") ? meta["tags"] : NULL_JSON, "template.tags", warnings);
        out["synonyms"] = synonymsJson(meta.contains("synonyms") ? meta["synonyms"] : NULL_JSON, "template.synonyms", warnings);
        Json placeholders = Json::array();
        if (meta.contains("placeholders") && meta["placeholders"].is_array()) {
            for (size_t i = 0; i < meta["placeholders"].size(); i++) {
                const Json& p = meta["placeholders"][i];
                const std::string at = "template.placeholders[" + std::to_string(i) + "]";
                if (!p.is_object() || !p.contains("name") || !p["name"].is_string()) {
                    warn(warnings, "bad_member", at, "a placeholder needs a `name`: dropped");
                    continue;
                }
                Json entry = Json::object();
                entry["name"] = p["name"];
                entry["role"] = p.value("role", std::string("title"));
                Json sample = Json::object();
                const Json s = p.contains("sample") && p["sample"].is_object() ? p["sample"] : Json::object();
                sample["en"] = s.value("en", std::string());
                sample["ja"] = s.value("ja", std::string());
                entry["sample"] = sample;
                entry["max_chars"] = p.contains("max_chars") && p["max_chars"].is_number_integer() ? p["max_chars"] : Json(16);
                if (p.contains("chars") && p["chars"].is_object()) entry["chars"] = p["chars"];
                for (auto it = p.begin(); it != p.end(); ++it) {
                    if (!entry.contains(it.key())) entry[it.key()] = it.value();
                }
                placeholders.push_back(std::move(entry));
            }
        }
        out["placeholders"] = placeholders;
        out["canvas_note"] = meta.value("canvas_note", std::string());
        out["provenance"] = provenanceJson(meta.contains("provenance") ? meta["provenance"] : NULL_JSON);
        for (auto it = meta.begin(); it != meta.end(); ++it) {
            if (!out.contains(it.key())) out[it.key()] = it.value();
        }
        return out;
    }

}

TemplateParseResult titlelibrary::parseTemplate(const std::string& text) {
    TemplateParseResult result;
    Json root = Json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        result.errorCode = root.is_discarded() ? "not_json" : "not_object";
        result.error = "a template is a JSON object";
        return result;
    }
    Json meta = root.contains("template") ? root["template"] : Json::object();
    root.erase("template");
    if (!meta.is_object()) {
        result.warnings.push_back({"bad_member", "template", "not an object: empty"});
        meta = Json::object();
    }
    const int metaVersion = meta.value("version", TITLE_TEMPLATE_MEMBER_VERSION);
    titleart::TitleArtParseResult doc = titleart::parseTitleArtJson(std::move(root));
    if (!doc.ok) {
        if (doc.error.code == "version_newer") result.tmpl.readOnly = true;
        result.errorCode = doc.error.code;
        result.error = doc.error.message;
        return result;
    }
    for (const titleart::TitleWarning& w : doc.warnings) result.warnings.push_back({"document", w.path, w.message});
    result.tmpl.doc = std::move(doc.doc);
    result.tmpl.meta = canonicalTemplateMeta(meta, &result.warnings);
    if (metaVersion > TITLE_TEMPLATE_MEMBER_VERSION) {
        result.tmpl.readOnly = true;
        result.warnings.push_back({"version_newer", "template.version", "written by a newer editor: read-only"});
    }
    result.ok = true;
    return result;
}

std::string titlelibrary::writeTemplate(const TitleTemplate& tmpl) {
    titleart::TitleArtDoc doc = tmpl.doc;
    titleart::normalizeTitleArt(doc, nullptr);
    Json out = Json::object();
    for (auto it = doc.root.begin(); it != doc.root.end(); ++it) {
        out[it.key()] = it.value();
        if (it.key() == "guid") out["template"] = tmpl.readOnly ? tmpl.meta : canonicalTemplateMeta(tmpl.meta, nullptr);
    }
    return titleart::writeTitleArtJson(out) + "\n";
}

// ---- identities ----

bool titlelibrary::isLibrarySlug(std::string_view slug) {
    if (slug.empty() || slug.size() > 48) return false;
    for (char c : slug) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

std::string titlelibrary::slugOf(std::string_view name) {
    std::string out;
    bool dash = false;
    for (char ch : name) {
        char c = ch;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            if (dash && !out.empty()) out += '-';
            dash = false;
            out += c;
        } else {
            dash = true;
        }
        if (out.size() >= 48) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

bool titlelibrary::isLibraryRef(std::string_view ref) {
    const size_t slash = ref.find('/');
    if (slash == std::string_view::npos) return isLibrarySlug(ref);
    return isLibrarySlug(ref.substr(0, slash)) && isLibrarySlug(ref.substr(slash + 1));
}
