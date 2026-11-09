// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleTranslations.h"

#include "TitleArtIO.h"
#include "TitleArtSchema.h"
#include "util/TitleArtLanguage.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    // ---- schema ----

    SchemaMember member(const char* key, ValueType type, Json def, const char* label = nullptr) {
        SchemaMember m;
        m.key = key;
        m.type = type;
        m.defaultValue = std::move(def);
        if (label) m.label = label;
        m.owner = "S87";
        // the renderer draws a resolved copy (TitleTranslations.h): these members never reach a
        // render as themselves, so they feed no content hash
        m.hash = HashScope::Identity;
        switch (type) {
            case ValueType::Number: case ValueType::Vec2:
                m.anim = FxAnim::Rerender;
                m.interp = ParamInterp::Smooth;
                break;
            case ValueType::Object: case ValueType::ObjectArray: case ValueType::ObjectMap: case ValueType::Json:
                m.anim = FxAnim::None;
                m.interp = ParamInterp::Stepped;
                break;
            default:
                m.anim = FxAnim::Step;
                m.interp = ParamInterp::Stepped;
                break;
        }
        return m;
    }

    SchemaMember number(const char* key, double def, double min, double max, const char* label = nullptr) {
        SchemaMember m = member(key, ValueType::Number, def, label);
        m.min = min;
        m.max = max;
        return m;
    }

    SchemaMember integer(const char* key, int64_t def, double min, double max, const char* label = nullptr) {
        SchemaMember m = member(key, ValueType::Integer, def, label);
        m.min = min;
        m.max = max;
        return m;
    }

    SchemaMember object(const char* key, ValueType type, const char* kind, Json def, const char* label = nullptr) {
        SchemaMember m = member(key, type, std::move(def), label);
        m.kind = kind;
        return m;
    }

    SchemaKind kindOf(const char* name, const char* label, std::vector<SchemaMember> members, bool sparse = false) {
        SchemaKind k;
        k.name = name;
        k.label = label;
        k.hash = HashScope::Identity;
        k.members = std::move(members);
        k.sparse = sparse;
        return k;
    }

    // a Language's code in canonical case (its uniqueness is the document pass's)
    void finishLanguage(Json& language, const std::string&, NormalizeContext&) {
        language["code"] = TitleArtLanguage::canonicalize(str(language, "code"));
    }

    std::vector<SchemaMember> fitMembers() {
        SchemaMember mode = member("mode", ValueType::Enum, "none");
        mode.options = {"none", "shrink", "condense", "wrap"};
        SchemaMember box = member("box", ValueType::Vec2, Json::array({0, 0}));
        box.unit = ParamUnit::Px;
        return {mode, box, number("min_scale", 0.6, 0.05, 1, "Min Scale"), number("min_h_scale", 0.8, 0.1, 1, "Min Horizontal Scale"),
                integer("max_lines", 2, 1, 16, "Max Lines")};
    }

}

void titleart::registerTitleLanguageMembers(TitleArtSchema& schema) {
    // ---- kinds ----
    {
        SchemaKind k = kindOf("language", "Language",
                              {member("code", ValueType::String, ""), member("name", ValueType::String, ""), member("font", ValueType::AssetRef, nullptr),
                               member("fallback_fonts", ValueType::AssetRefList, Json::array(), "Fallback Fonts"),
                               member("characters", ValueType::String, ""), member("bake", ValueType::Bool, true)});
        k.members[3].maxItems = 16;
        k.finish = &finishLanguage;
        schema.addKind(std::move(k));
    }
    schema.addKind(kindOf("text_fit", "Fit", fitMembers()));
    // a Translation's fit: only the members given
    schema.addKind(kindOf("translation_fit", "Fit", fitMembers(), true));
    {
        SchemaMember status = member("status", ValueType::Enum, "translated");
        status.options = {"translated", "needs_review"};
        SchemaMember fallbacks = member("fallback_fonts", ValueType::AssetRefList, Json::array(), "Fallback Fonts");
        fallbacks.maxItems = 16;
        SchemaMember align = member("align", ValueType::Enum, "center");
        align.options = {"start", "center", "end"};
        SchemaMember variations = member("variations", ValueType::NumberMap, Json::object());
        variations.mapKeys = MapKeys::Tag;
        SchemaMember features = member("features", ValueType::IntegerMap, Json::object());
        features.mapKeys = MapKeys::Tag;
        features.min = 0;
        features.max = 65535;
        SchemaMember chars = object("chars", ValueType::ObjectMap, "char_adjust", Json::object(), "Characters");
        chars.mapKeys = MapKeys::Index;
        SchemaMember lines = object("lines", ValueType::ObjectMap, "line_adjust", Json::object());
        lines.mapKeys = MapKeys::Index;
        SchemaMember offset = member("offset", ValueType::Vec2, Json::array({0, 0}));
        offset.unit = ParamUnit::Px;
        SchemaKind k = kindOf("translation", "Translation",
                              {member("text", ValueType::String, ""), status, member("source_hash", ValueType::String, ""),
                               member("font", ValueType::AssetRef, nullptr), integer("face_index", 0, 0, 65535), fallbacks,
                               member("language", ValueType::String, ""), number("size_scale", 1, 0.1, 10),
                               number("tracking", 0, -1000, 1000), number("line_height", 1.1, 0.1, 10),
                               number("h_scale", 1, 0.1, 10, "Horizontal Scale"), number("v_scale", 1, 0.1, 10, "Vertical Scale"),
                               member("vertical", ValueType::Bool, false), align, features, variations, chars, lines, offset,
                               object("fit", ValueType::Object, "translation_fit", Json::object())},
                              true);
        k.alwaysWritten = {"text", "status", "source_hash"};
        schema.addKind(std::move(k));
    }
    schema.addKind(kindOf("image_translation", "Image Translation", {member("image", ValueType::AssetRef, nullptr)}, true));
    schema.addKind(kindOf("vector_translation", "SVG Translation", {member("svg", ValueType::AssetRef, nullptr, "SVG")}, true));

    // ---- document ----
    {
        SchemaMember source = member("source_language", ValueType::String, "", "Source Language");
        schema.addMember("document", source);
        SchemaMember languages = object("languages", ValueType::ObjectArray, "language", Json::array());
        languages.maxItems = TITLE_MAX_LANGUAGES;
        schema.addMember("document", languages);
        SchemaMember ns = member("string_namespace", ValueType::String, "", "String Namespace");
        ns.pattern = "^$|^[a-z0-9_]{1,48}$";
        schema.addMember("document", ns);
        SchemaMember dpi = number("dpi", TITLE_DEFAULT_DPI, 1, 2400, "DPI");
        dpi.anim = FxAnim::None;
        schema.addMember("document", dpi);
        schema.addMember("document", member("import_notes", ValueType::StringList, Json::array(), "Import Notes"));
    }
    // ---- layers ----
    schema.addMember("layer", member("only_languages", ValueType::StringList, Json::array(), "Only Languages"));
    {
        SchemaMember key = member("text_key", ValueType::String, "", "Text Key");
        key.pattern = "^$|^[a-z0-9_.-]{1,64}$";
        schema.addMember("layer.text", key);
        schema.addMember("layer.text", member("translate", ValueType::Bool, true));
        schema.addMember("layer.text", integer("max_chars", 0, 0, 100000, "Max Chars"));
        schema.addMember("layer.text", member("translator_note", ValueType::String, "", "Translator Note"));
        schema.addMember("layer.text", object("fit", ValueType::Object, "text_fit", nullptr));
        SchemaMember translations = object("translations", ValueType::ObjectMap, "translation", Json::object());
        translations.mapKeys = MapKeys::LanguageCode;
        schema.addMember("layer.text", translations);
    }
    for (const auto& [variant, kind] : {std::pair<const char*, const char*>{"layer.image", "image_translation"}, {"layer.vector", "vector_translation"}}) {
        SchemaMember translations = object("translations", ValueType::ObjectMap, kind, Json::object());
        translations.mapKeys = MapKeys::LanguageCode;
        schema.addMember(variant, translations);
    }
}

// ---- normalize ----

namespace {

    std::string uniqueOf(const std::string& base, std::set<std::string>& used, size_t limit) {
        if (used.insert(base).second) return base;
        for (int n = 2;; n++) {
            const std::string suffix = "_" + std::to_string(n);
            std::string candidate = base.substr(0, std::min(base.size(), limit - suffix.size())) + suffix;
            if (used.insert(candidate).second) return candidate;
        }
    }

    void normalizeLayerLanguages(Json& layer, const std::string& path, std::set<std::string>& textKeys, NormalizeContext& context) {
        if (auto it = layer.find("only_languages"); it != layer.end() && it->is_array()) {
            Json codes = Json::array();
            std::set<std::string> seen;
            for (const Json& c : *it) {
                const std::string code = TitleArtLanguage::canonicalize(c.is_string() ? c.get<std::string>() : std::string());
                if (!code.empty() && seen.insert(lower(code)).second) codes.push_back(code);
            }
            *it = std::move(codes);
        }
        if (auto it = layer.find("translations"); it != layer.end() && it->is_object() && !it->empty()) {
            Json out = Json::object();
            for (auto entry = it->begin(); entry != it->end(); ++entry) {
                const std::string code = TitleArtLanguage::canonicalize(entry.key());
                if (code.empty() || out.contains(code)) {
                    context.warn(jsonPathMember(jsonPathMember(path, "translations"), entry.key()),
                                 code.empty() ? "not a language tag: dropped" : "the same language as '" + code + "': dropped");
                    continue;
                }
                out[code] = entry.value();
            }
            // keys sorted as the canonical writer sorts maps
            Json sorted = Json::object();
            std::vector<std::string> keys;
            for (auto entry = out.begin(); entry != out.end(); ++entry) keys.push_back(entry.key());
            std::sort(keys.begin(), keys.end());
            for (const std::string& k : keys) sorted[k] = std::move(out[k]);
            *it = std::move(sorted);
        }
        if (layerTypeOf(layer) == LayerType::Text) {
            std::string key = str(layer, "text_key");
            if (!key.empty()) {
                const std::string unique = uniqueOf(key, textKeys, 64);
                if (unique != key) {
                    context.warn(jsonPathMember(path, "text_key"), "'" + key + "' is used twice: '" + unique + "' is given");
                    layer["text_key"] = unique;
                }
            }
        }
        if (auto it = layer.find("children"); it != layer.end() && it->is_array()) {
            for (size_t i = 0; i < it->size(); i++) {
                normalizeLayerLanguages((*it)[i], jsonPathIndex(jsonPathMember(path, "children"), i), textKeys, context);
            }
        }
    }

}

void titleart::normalizeTitleLanguages(Json& root, NormalizeContext& context) {
    const std::string source = TitleArtLanguage::canonicalize(str(root, "source_language"));
    if (root.contains("source_language")) root["source_language"] = source;
    std::set<std::string> codes;
    if (!source.empty()) codes.insert(lower(source));
    if (auto it = root.find("languages"); it != root.end() && it->is_array()) {
        Json kept = Json::array();
        for (size_t i = 0; i < it->size(); i++) {
            Json& language = (*it)[i];
            const std::string code = str(language, "code");
            const std::string at = jsonPathIndex("languages", i);
            std::string error;
            if (!TitleArtLanguage::isValid(code, &error)) {
                context.warn(jsonPathMember(at, "code"), error + ": the language is dropped");
                continue;
            }
            if (!codes.insert(lower(code)).second) {
                context.warn(jsonPathMember(at, "code"), lower(code) == lower(source) ? "'" + code + "' is the source language: dropped"
                                                                                      : "'" + code + "' is listed twice: dropped");
                continue;
            }
            kept.push_back(std::move(language));
        }
        *it = std::move(kept);
    }
    std::set<std::string> textKeys;
    if (auto it = root.find("layers"); it != root.end() && it->is_array()) {
        for (size_t i = 0; i < it->size(); i++) normalizeLayerLanguages((*it)[i], jsonPathIndex("layers", i), textKeys, context);
    }
    // R16: an output id equal to a language code would make bake names ambiguous
    if (codes.empty()) return;
    if (auto it = root.find("outputs"); it != root.end() && it->is_array()) {
        std::set<std::string> used;
        for (const Json& o : *it) used.insert(str(o, "id"));
        for (size_t i = 0; i < it->size(); i++) {
            Json& output = (*it)[i];
            const std::string id = str(output, "id");
            if (!codes.count(lower(id))) continue;
            std::string renamed;
            for (int n = 2; renamed.empty() || used.count(renamed); n++) renamed = id + "_" + std::to_string(n);
            used.insert(renamed);
            context.warn(jsonPathMember(jsonPathIndex("outputs", i), "id"),
                         "output_id_is_language: '" + id + "' is a language code: '" + renamed + "' is given");
            output["id"] = renamed;
        }
    }
}

// ---- queries ----

bool titleart::titleIsMultilingual(const Json& root) {
    auto languages = root.find("languages");
    return !str(root, "source_language").empty() || (languages != root.end() && languages->is_array() && !languages->empty());
}

std::string titleart::titleSourceLanguage(const Json& root) {
    return str(root, "source_language");
}

std::vector<std::string> titleart::titleLanguageCodes(const Json& root) {
    std::vector<std::string> out;
    const std::string source = str(root, "source_language");
    if (!source.empty()) out.push_back(source);
    if (auto it = root.find("languages"); it != root.end() && it->is_array()) {
        for (const Json& l : *it) {
            const std::string code = str(l, "code");
            if (!code.empty()) out.push_back(code);
        }
    }
    return out;
}

const Json* titleart::findTitleLanguage(const Json& root, const std::string& code) {
    auto it = root.find("languages");
    if (it == root.end() || !it->is_array()) return nullptr;
    const std::string wanted = lower(code);
    for (const Json& l : *it) {
        if (lower(str(l, "code")) == wanted) return &l;
    }
    return nullptr;
}

const Json* titleart::findTranslation(const Json& layer, const std::string& code) {
    auto it = layer.find("translations");
    if (it == layer.end() || !it->is_object() || code.empty()) return nullptr;
    if (auto exact = it->find(code); exact != it->end()) return &*exact;
    const std::string wanted = lower(code);
    for (auto entry = it->begin(); entry != it->end(); ++entry) {
        if (lower(entry.key()) == wanted) return &entry.value();
    }
    return nullptr;
}

Json* titleart::findTranslation(Json& layer, const std::string& code) {
    return const_cast<Json*>(findTranslation(static_cast<const Json&>(layer), code));
}

bool titleart::translationHasText(const Json* translation) {
    return translation && translation->is_object() && !str(*translation, "text").empty();
}

bool titleart::layerShownInLanguage(const Json& layer, const std::string& code) {
    auto it = layer.find("only_languages");
    if (it == layer.end() || !it->is_array() || it->empty()) return true;
    const std::string wanted = lower(code);
    for (const Json& c : *it) {
        if (c.is_string() && lower(c.get<std::string>()) == wanted) return true;
    }
    return false;
}

std::string titleart::translationSourceHash(const std::string& sourceText) {
    uint32_t h = 2166136261u;
    for (unsigned char c : sourceText) {
        h ^= c;
        h *= 16777619u;
    }
    char out[9];
    std::snprintf(out, sizeof(out), "%08x", h);
    return out;
}

const std::vector<std::string>& titleart::translationOverrideKeys() {
    static const std::vector<std::string> keys = {"font", "face_index", "fallback_fonts", "language", "size_scale", "tracking",
                                                  "line_height", "h_scale", "v_scale", "vertical", "align", "features",
                                                  "variations", "chars", "lines", "offset", "fit"};
    return keys;
}

bool titleart::isSourceLanguage(const Json& root, const std::string& code) {
    return code.empty() || lower(code) == lower(str(root, "source_language"));
}

// ---- resolution ----

namespace {

    struct Resolver {
        std::string code;           // the language resolved (canonical)
        bool source = false;        // the source language
        std::string sourceCode;
        const Json* language = nullptr;     // the Language entry (null for the source)

        void textLayer(Json& layer) const {
            if (source) {
                if (str(layer, "language").empty() && !sourceCode.empty()) layer["language"] = sourceCode;
                return;
            }
            // a brand word keeps its text and fonts; it shapes as the rest of the language does
            if (!layer.value("translate", true)) {
                if (str(layer, "language").empty()) layer["language"] = code;
                return;
            }
            const Json* t = findTranslation(layer, code);
            if (translationHasText(t)) layer["text"] = (*t)["text"];
            // the Language entry's fonts (R4 step 2), then the Translation's overrides (step 1)
            if (language) {
                if (auto f = language->find("font"); f != language->end() && !decodeAssetRef(*f).empty()) layer["font"] = *f;
                if (auto f = language->find("fallback_fonts"); f != language->end() && f->is_array() && !f->empty()) layer["fallback_fonts"] = *f;
            }
            std::string shaping = code;
            if (t && t->is_object()) {
                for (const std::string& key : translationOverrideKeys()) {
                    auto it = t->find(key);
                    if (it == t->end()) continue;
                    if (key == "language") {
                        if (it->is_string() && !it->get<std::string>().empty()) shaping = it->get<std::string>();
                    } else if (key == "size_scale") {
                        if (it->is_number()) layer["size"] = layer.value("size", 200.0) * it->get<double>();
                    } else if (key == "offset") {
                        const TitleVec2 offset = decodeVec2(*it);
                        Json& transform = layer["transform"];
                        const TitleVec2 position = decodeVec2(transform.value("position", Json::array({0, 0})));
                        transform["position"] = Json::array({position.x + offset.x, position.y + offset.y});
                    } else if (key == "fit") {
                        Json& fit = layer["fit"];
                        if (!fit.is_object()) fit = Json::object();
                        if (it->is_object()) {
                            for (auto m = it->begin(); m != it->end(); ++m) fit[m.key()] = m.value();
                        }
                    } else {
                        layer[key] = *it;
                    }
                }
            }
            layer["language"] = shaping;
        }

        void pixelLayer(Json& layer, const char* asset) const {
            if (source) return;
            const Json* t = findTranslation(layer, code);
            if (!t || !t->is_object()) return;
            if (auto it = t->find(asset); it != t->end() && !decodeAssetRef(*it).empty()) layer[asset] = *it;
        }

        void list(Json& layers) const {
            if (!layers.is_array()) return;
            for (Json& layer : layers) {
                if (!layerShownInLanguage(layer, source ? sourceCode : code)) layer["visible"] = false;
                switch (layerTypeOf(layer)) {
                    case LayerType::Text: textLayer(layer); break;
                    case LayerType::Image: pixelLayer(layer, "image"); break;
                    case LayerType::Vector: pixelLayer(layer, "svg"); break;
                    default: break;
                }
                if (layer.contains("translations")) layer["translations"] = Json::object();
                if (auto children = layer.find("children"); children != layer.end()) list(*children);
            }
        }
    };

}

TitleArtDoc titleart::resolveTitleLanguage(const TitleArtDoc& doc, const std::string& code, std::vector<TitleProblem>* problems) {
    if (!titleIsMultilingual(doc.root)) return doc;
    Resolver r;
    r.sourceCode = str(doc.root, "source_language");
    r.code = TitleArtLanguage::canonicalize(code);
    r.source = isSourceLanguage(doc.root, r.code);
    if (!r.source) {
        r.language = findTitleLanguage(doc.root, r.code);
        if (!r.language) {
            if (problems) {
                TitleProblem p;
                p.code = "language_unknown";
                p.message = "the title has no language '" + code + "': the source (" + (r.sourceCode.empty() ? "unnamed" : r.sourceCode) +
                            ") is shown";
                problems->push_back(std::move(p));
            }
            r.source = true;
        }
    }
    TitleArtDoc out = doc;
    if (out.root.contains("layers")) r.list(out.root["layers"]);
    // the copy is one language's: resolving it again changes nothing
    out.root["source_language"] = r.source ? r.sourceCode : r.code;
    out.root["languages"] = Json::array();
    return out;
}

namespace {

    bool layerDiffers(const Json& layer, const std::string& code, const std::string& source, const Json* language) {
        if (layerShownInLanguage(layer, code) != layerShownInLanguage(layer, source)) return true;
        const LayerType type = layerTypeOf(layer);
        const Json* t = findTranslation(layer, code);
        if (type == LayerType::Text && layer.value("translate", true)) {
            if (language && (!decodeAssetRef(language->value("font", Json())).empty() ||
                             (language->contains("fallback_fonts") && !(*language)["fallback_fonts"].empty()))) {
                return true;
            }
            if (t && t->is_object()) {
                if (translationHasText(t) && str(*t, "text") != str(layer, "text")) return true;
                for (const std::string& key : translationOverrideKeys()) {
                    if (key != "language" && t->contains(key)) return true;
                }
            }
        } else if ((type == LayerType::Image || type == LayerType::Vector) && t && t->is_object()) {
            if (!decodeAssetRef(t->value(type == LayerType::Image ? "image" : "svg", Json())).empty()) return true;
        }
        if (auto children = layer.find("children"); children != layer.end() && children->is_array()) {
            for (const Json& child : *children) {
                if (layerDiffers(child, code, source, language)) return true;
            }
        }
        return false;
    }

}

bool titleart::titleLanguageDiffers(const TitleArtDoc& doc, const std::string& code) {
    if (!titleIsMultilingual(doc.root) || isSourceLanguage(doc.root, code)) return false;
    const Json* language = findTitleLanguage(doc.root, code);
    if (!language) return false;
    const std::string source = str(doc.root, "source_language");
    for (const Json& layer : doc.layers()) {
        if (layerDiffers(layer, code, source, language)) return true;
    }
    return false;
}
