// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLanguageOps.h"

#include "TitleLanguageNames.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleFit.h"
#include "titleart/TitleTranslations.h"
#include "util/TitleArtLanguage.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    // every layer of the tree, mutable, in document order
    void eachLayer(Json& list, const std::function<void(Json&)>& visit) {
        if (!list.is_array()) return;
        for (Json& layer : list) {
            visit(layer);
            if (auto children = layer.find("children"); children != layer.end()) eachLayer(*children, visit);
        }
    }

    std::string slug(const std::string& name, const char* allowedExtra, size_t limit) {
        std::string out;
        bool gap = false;
        for (unsigned char c : name) {
            const bool keep = (c < 128 && std::isalnum(c)) || (c && std::strchr(allowedExtra, (char)c));
            if (keep) {
                if (gap && !out.empty()) out += '_';
                out += (char)std::tolower(c);
                gap = false;
            } else {
                gap = true;
            }
            if (out.size() >= limit) break;
        }
        return out;
    }

    // a language code of the op: valid and canonical, else the op fails
    bool languageCode(TitleOpContext& ctx, const Json& op, const char* member, std::string& out) {
        std::string raw;
        if (!ctx.requireString(op, member, raw)) return false;
        std::string error;
        if (!TitleArtLanguage::isValid(raw, &error)) return ctx.fail("bad_value", error, member);
        out = TitleArtLanguage::canonicalize(raw);
        return true;
    }

    std::vector<std::string> translationCodes(const Json& root) {
        std::vector<std::string> out;
        if (auto it = root.find("languages"); it != root.end() && it->is_array()) {
            for (const Json& l : *it) out.push_back(str(l, "code"));
        }
        return out;
    }

    // the Language index of `code` in the working document, -1 when none
    int languageIndex(const Json& root, const std::string& code) {
        auto it = root.find("languages");
        if (it == root.end() || !it->is_array()) return -1;
        for (size_t i = 0; i < it->size(); i++) {
            if (lower(str((*it)[i], "code")) == lower(code)) return (int)i;
        }
        return -1;
    }

    // a translation language of the document (not the source): else the op fails with the fix
    bool requireLanguage(TitleOpContext& ctx, const std::string& code, const char* member) {
        if (languageIndex(ctx.doc.root, code) >= 0) return true;
        std::vector<std::string> valid = translationCodes(ctx.doc.root);
        if (titleart::isSourceLanguage(ctx.doc.root, code) && !code.empty()) {
            return ctx.fail("bad_value", "'" + code + "' is the source language: edit the layer itself", member, valid);
        }
        return ctx.fail("language_unknown", "the title has no language '" + code + "': add it first", member, valid,
                        titleart::nearestName(code, valid), Json::array({Json{{"op", "add_language"}, {"code", code}}}));
    }

    std::string layerName(const Json& layer) {
        return str(layer, "name").empty() ? "Text" : str(layer, "name");
    }

    // ---- add_language ----

    bool addLanguage(TitleOpContext& ctx, const Json& op) {
        std::string code;
        if (!languageCode(ctx, op, "code", code)) return false;
        Json& root = ctx.doc.root;
        if (str(root, "source_language").empty()) {
            std::string source = op.contains("source") && op["source"].is_string() ? op["source"].get<std::string>() : std::string();
            if (source.empty() && ctx.options) source = ctx.options->defaultLanguage;
            if (source.empty()) source = "en";
            std::string error;
            if (!TitleArtLanguage::isValid(source, &error)) return ctx.fail("bad_value", error, "source");
            source = TitleArtLanguage::canonicalize(source);
            if (lower(source) == lower(code)) {
                return ctx.fail("bad_value", "'" + code + "' would be the source language and a translation: give another `source`", "source");
            }
            if (!ctx.set(nullptr, "source_language", source)) return false;
        }
        if (titleart::isSourceLanguage(root, code)) {
            return ctx.fail("language_exists", "'" + code + "' is the source language", "code", translationCodes(root));
        }
        if (languageIndex(root, code) >= 0) {
            return ctx.fail("language_exists", "the title has '" + code + "' already", "code", translationCodes(root));
        }
        if (root["languages"].size() >= titleart::TITLE_MAX_LANGUAGES) {
            return ctx.fail("too_many_languages", "a title has at most " + std::to_string(titleart::TITLE_MAX_LANGUAGES) + " languages", "code");
        }
        Json language = Json{{"code", code}, {"name", op.value("name", std::string())},
                             {"font", op.contains("font") ? op["font"] : Json()},
                             {"fallback_fonts", op.contains("fallback_fonts") && op["fallback_fonts"].is_array() ? op["fallback_fonts"] : Json::array()},
                             {"characters", ""}, {"bake", true}};
        titleart::NormalizeContext nc;
        titleart::normalizeTitleArtObject(language, "language", "languages", nc);
        Json list = root["languages"];
        list.push_back(language);
        if (!ctx.set(nullptr, "languages", list)) return false;

        std::string copyFrom;
        if (op.contains("copy_from")) {
            if (!languageCode(ctx, op, "copy_from", copyFrom)) return false;
            if (languageIndex(root, copyFrom) < 0) return requireLanguage(ctx, copyFrom, "copy_from");
        }
        ensureTitleTextKeys(ctx);
        // R8: every text layer gets a box (the source's extent + 10 %) and shrink, unless it has one
        bool ok = true;
        const titleart::TitleArtDoc reading = ctx.doc;
        eachLayer(root["layers"], [&](Json& layer) {
            if (!ok || titleart::layerTypeOf(layer) != titleart::LayerType::Text) return;
            if (!copyFrom.empty()) {
                if (const Json* t = titleart::findTranslation(layer, copyFrom); t && t->is_object()) {
                    Json copy = *t;
                    copy["status"] = "needs_review";
                    ok = ok && ctx.set(&layer, "translations." + code, copy);
                }
            }
            const titleart::TitleVec2 box = titleart::decodeVec2(layer["fit"].value("box", Json::array({0, 0})));
            if ((box.x > 0 || box.y > 0) || !layer.value("translate", true) || str(layer, "text").empty()) return;
            const titleart::TitleVec2 extent = titleart::measureTitleTextExtent(layer, reading, ctx.options ? ctx.options->assets : nullptr);
            if (extent.x <= 0 || extent.y <= 0) return;
            const auto round = [](double v) { return std::ceil(v * 1.1); };
            ok = ok && ctx.set(&layer, "fit.box", Json::array({round(extent.x), round(extent.y)}));
            if (layer["fit"].value("mode", std::string("none")) == "none") ok = ok && ctx.set(&layer, "fit.mode", "shrink");
        });
        if (!ok) return false;
        ctx.undoName = "Add Language " + titleLanguageLabel(code, op.value("name", std::string()));
        return true;
    }

    // ---- remove_language ----

    bool removeLanguage(TitleOpContext& ctx, const Json& op) {
        std::string code;
        if (!languageCode(ctx, op, "code", code)) return false;
        Json& root = ctx.doc.root;
        const int index = languageIndex(root, code);
        if (index < 0) return requireLanguage(ctx, code, "code");
        Json list = root["languages"];
        list.erase(list.begin() + index);
        if (!ctx.set(nullptr, "languages", list)) return false;
        size_t removed = 0;
        eachLayer(root["layers"], [&](Json& layer) {
            auto translations = layer.find("translations");
            if (translations == layer.end() || !translations->is_object()) return;
            for (auto it = translations->begin(); it != translations->end(); ++it) {
                if (lower(it.key()) != lower(code)) continue;
                TitleOpChange c;
                c.layer = str(layer, "id");
                c.path = "translations." + it.key();
                c.from = it.value();
                c.to = nullptr;
                c.note = "removed the " + code + " translation";
                translations->erase(it);
                ctx.change(std::move(c));
                removed++;
                break;
            }
        });
        ctx.result.reports[std::to_string(ctx.opIndex)] = Json{{"translations_removed", removed}};
        ctx.undoName = "Remove Language " + code;
        return true;
    }

    // ---- set_source_language (D6) ----

    // the base members a Translation's overrides replace when it becomes the source
    void swapLayer(TitleOpContext& ctx, Json& layer, const std::string& newSource, const std::string& oldSource, bool& ok) {
        if (titleart::layerTypeOf(layer) != titleart::LayerType::Text) {
            // pixel layers: a per-language asset becomes the base, the old one the old source's
            const char* asset = titleart::layerTypeOf(layer) == titleart::LayerType::Image ? "image"
                                : titleart::layerTypeOf(layer) == titleart::LayerType::Vector ? "svg" : nullptr;
            if (!asset) return;
            Json* t = titleart::findTranslation(layer, newSource);
            if (!t || !t->is_object() || !t->contains(asset)) return;
            Json translations = layer["translations"];
            const Json oldAsset = layer[asset];
            Json fresh = Json::object();
            for (auto it = translations.begin(); it != translations.end(); ++it) {
                if (lower(it.key()) != lower(newSource)) fresh[it.key()] = it.value();
            }
            fresh[oldSource] = Json{{asset, oldAsset}};
            ok = ok && ctx.set(&layer, asset, (*t)[asset]) && ctx.set(&layer, "translations", fresh);
            return;
        }
        const Json* t = titleart::findTranslation(layer, newSource);
        Json translations = layer.value("translations", Json::object());
        Json fresh = Json::object();
        for (auto it = translations.begin(); it != translations.end(); ++it) {
            if (lower(it.key()) != lower(newSource)) fresh[it.key()] = it.value();
        }
        Json oldEntry = Json{{"text", str(layer, "text")}, {"status", "translated"}, {"source_hash", ""}};
        Json base = layer;
        if (t && t->is_object() && layer.value("translate", true)) {
            if (titleart::translationHasText(t)) base["text"] = (*t)["text"];
            for (const std::string& key : titleart::translationOverrideKeys()) {
                auto it = t->find(key);
                if (it == t->end()) continue;
                if (key == "size_scale" && it->is_number() && it->get<double>() > 0) {
                    base["size"] = layer.value("size", 200.0) * it->get<double>();
                    oldEntry["size_scale"] = 1.0 / it->get<double>();
                } else if (key == "offset") {
                    const titleart::TitleVec2 o = titleart::decodeVec2(*it);
                    const titleart::TitleVec2 p = titleart::decodeVec2(layer["transform"].value("position", Json::array({0, 0})));
                    base["transform"]["position"] = Json::array({p.x + o.x, p.y + o.y});
                    oldEntry["offset"] = Json::array({-o.x, -o.y});
                } else if (key == "fit") {
                    Json previous = Json::object();
                    for (auto m = it->begin(); m != it->end(); ++m) {
                        previous[m.key()] = layer["fit"].value(m.key(), Json());
                        base["fit"][m.key()] = m.value();
                    }
                    oldEntry["fit"] = previous;
                } else {
                    oldEntry[key] = layer.contains(key) ? layer[key] : Json();
                    base[key] = *it;
                }
            }
        }
        // every Translation now translates the new base text
        const std::string hash = titleart::translationSourceHash(str(base, "text"));
        oldEntry["source_hash"] = hash;
        for (auto it = fresh.begin(); it != fresh.end(); ++it) {
            if (it->is_object() && titleart::translationHasText(&it.value())) (*it)["source_hash"] = hash;
        }
        if (layer.value("translate", true) && !str(layer, "text").empty()) fresh[oldSource] = oldEntry;
        base["translations"] = fresh;
        for (auto it = base.begin(); it != base.end(); ++it) {
            if (it.key() == "id" || it.key() == "type" || layer.value(it.key(), Json()) == it.value()) continue;
            ok = ok && ctx.set(&layer, it.key(), it.value());
            if (!ok) return;
        }
    }

    bool setSourceLanguage(TitleOpContext& ctx, const Json& op) {
        std::string code;
        if (!languageCode(ctx, op, "code", code)) return false;
        Json& root = ctx.doc.root;
        const std::string oldSource = str(root, "source_language");
        if (lower(code) == lower(oldSource)) return true;
        const bool swap = op.value("swap", true);
        const int index = languageIndex(root, code);
        if (index < 0 || !swap || oldSource.empty()) {
            if (index >= 0) {
                return ctx.fail("language_exists", "'" + code + "' is a translation language: swap: true makes its texts the source", "code");
            }
            ctx.undoName = "Set Source Language " + code;
            return ctx.set(nullptr, "source_language", code);
        }
        Json languages = root["languages"];
        const Json removed = languages[index];
        languages.erase(languages.begin() + index);
        Json oldLanguage = Json{{"code", oldSource}, {"name", ""}, {"font", Json()}, {"fallback_fonts", Json::array()},
                                {"characters", ""}, {"bake", removed.value("bake", true)}};
        languages.insert(languages.begin() + index, oldLanguage);
        if (!ctx.set(nullptr, "source_language", code) || !ctx.set(nullptr, "languages", languages)) return false;
        bool ok = true;
        eachLayer(root["layers"], [&](Json& layer) {
            if (ok) swapLayer(ctx, layer, code, oldSource, ok);
        });
        ctx.undoName = "Set Source Language " + code;
        return ok;
    }

    // ---- set_translation ----

    bool setTranslation(TitleOpContext& ctx, const Json& op) {
        Json* layer = ctx.layer(op);
        if (!layer) return false;
        std::string code;
        if (!languageCode(ctx, op, "language", code)) return false;
        if (!requireLanguage(ctx, code, "language")) return false;
        const titleart::LayerType type = titleart::layerTypeOf(*layer);
        if (type != titleart::LayerType::Text && type != titleart::LayerType::Image && type != titleart::LayerType::Vector) {
            return ctx.fail("bad_layer", "'" + layerName(*layer) + "' is a " + titleart::layerTypeName(type) +
                                             " layer: translations are on text, image and vector layers", "layer");
        }
        if (type != titleart::LayerType::Text && op.contains("text")) {
            return ctx.fail("bad_op", "an image or vector layer's translation is its asset: values {image} or {svg}", "text");
        }
        // the entry's key in the map: the canonical code
        const std::string prefix = "translations." + code + ".";
        bool any = false;
        if (op.contains("text")) {
            if (!op["text"].is_string()) return ctx.fail("bad_op", "`text` is a string", "text");
            if (!ctx.set(layer, prefix + "text", op["text"])) return false;
            if (!ctx.set(layer, prefix + "source_hash", titleart::translationSourceHash(str(*layer, "text")))) return false;
            if (!ctx.set(layer, prefix + "status", op.value("status", std::string("translated")))) return false;
            any = true;
        } else if (op.contains("status")) {
            if (!ctx.set(layer, prefix + "status", op["status"])) return false;
            any = true;
        }
        if (op.contains("values")) {
            const Json& values = op["values"];
            if (!values.is_object()) return ctx.fail("bad_op", "`values` is an object of Translation paths (font, size_scale, fit.mode ...)", "values");
            for (auto it = values.begin(); it != values.end(); ++it) {
                if (it.key() == "text" || it.key() == "source_hash") {
                    return ctx.fail("bad_op", "`" + it.key() + "` is set by the op's own `text`", "values." + it.key());
                }
                if (!ctx.set(layer, prefix + it.key(), it.value())) return false;
                any = true;
            }
        }
        if (!any) return ctx.fail("bad_op", "give `text`, `values` or `status`", "text");
        ctx.undoName = "Set Translation (" + layerName(*layer) + ": " + code + ")";
        return true;
    }

    // ---- revert_translation ----

    bool revertTranslation(TitleOpContext& ctx, const Json& op) {
        Json* layer = ctx.layer(op);
        if (!layer) return false;
        std::string code;
        if (!languageCode(ctx, op, "language", code)) return false;
        Json* t = titleart::findTranslation(*layer, code);
        if (!t) {
            ctx.note("nothing_to_revert", "'" + layerName(*layer) + "' has no " + code + " translation", "language");
            ctx.undoName = "Revert Translation (" + layerName(*layer) + ": " + code + ")";
            return true;
        }
        std::vector<std::string> fields;
        if (op.contains("fields")) {
            if (!op["fields"].is_array()) return ctx.fail("bad_op", "`fields` is a list of Translation members, or [\"all\"]", "fields");
            for (const Json& f : op["fields"]) if (f.is_string()) fields.push_back(f.get<std::string>());
        } else {
            fields = titleart::translationOverrideKeys();
        }
        Json translations = (*layer)["translations"];
        std::string key;
        for (auto it = translations.begin(); it != translations.end(); ++it) if (lower(it.key()) == lower(code)) key = it.key();
        if (std::find(fields.begin(), fields.end(), "all") != fields.end()) {
            translations.erase(key);
        } else {
            std::vector<std::string> valid = titleart::translationOverrideKeys();
            valid.push_back("all");
            for (const std::string& f : fields) {
                if (std::find(valid.begin(), valid.end(), f) == valid.end()) {
                    return ctx.fail("bad_value", "'" + f + "' is not a Translation override", "fields", valid, titleart::nearestName(f, valid));
                }
                translations[key].erase(f);
            }
        }
        if (!ctx.set(layer, "translations", translations)) return false;
        ctx.undoName = "Revert Translation (" + layerName(*layer) + ": " + code + ")";
        return true;
    }

    // ---- mark_reviewed ----

    bool markReviewed(TitleOpContext& ctx, const Json& op) {
        std::string only;
        if (op.contains("language") && !languageCode(ctx, op, "language", only)) return false;
        std::set<std::string> layerIds;
        if (op.contains("layers")) {
            if (!op["layers"].is_array()) return ctx.fail("bad_op", "`layers` is a list of layer paths", "layers");
            for (const Json& ref : op["layers"]) {
                if (!ref.is_string()) return ctx.fail("bad_op", "`layers` is a list of layer paths", "layers");
                Json* layer = ctx.resolveLayer(ref.get<std::string>(), "layers");
                if (!layer) return false;
                layerIds.insert(str(*layer, "id"));
            }
        }
        bool ok = true;
        eachLayer(ctx.doc.root["layers"], [&](Json& layer) {
            if (!ok || (!layerIds.empty() && !layerIds.count(str(layer, "id")))) return;
            auto translations = layer.find("translations");
            if (translations == layer.end() || !translations->is_object()) return;
            const std::string hash = titleart::translationSourceHash(str(layer, "text"));
            std::vector<std::string> keys;
            for (auto it = translations->begin(); it != translations->end(); ++it) keys.push_back(it.key());
            for (const std::string& key : keys) {
                if (!only.empty() && lower(key) != lower(only)) continue;
                const Json& t = (*translations)[key];
                if (!t.is_object() || !t.contains("status")) continue;
                if (str(t, "status") != "translated") ok = ok && ctx.set(&layer, "translations." + key + ".status", "translated");
                if (titleart::layerTypeOf(layer) == titleart::LayerType::Text && str(t, "source_hash") != hash) {
                    ok = ok && ctx.set(&layer, "translations." + key + ".source_hash", hash);
                }
            }
        });
        ctx.undoName = only.empty() ? std::string("Mark Translations Reviewed") : "Mark Translations Reviewed (" + only + ")";
        return ok;
    }

}

std::string editor::titleNamespaceFromPath(const std::string& documentPath) {
    std::string stem = std::filesystem::path(documentPath).filename().string();
    const size_t dot = stem.find('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    std::string out = slug(stem, "", 48);
    return out.empty() ? "title" : out;
}

std::string editor::titleStringKey(const Json& root, const Json& layer) {
    const std::string ns = str(root, "string_namespace");
    return ns.empty() ? str(layer, "text_key") : ns + "." + str(layer, "text_key");
}

bool editor::ensureTitleTextKeys(TitleOpContext& ctx) {
    Json& root = ctx.doc.root;
    bool changed = false;
    if (str(root, "string_namespace").empty()) {
        changed = ctx.set(nullptr, "string_namespace", titleNamespaceFromPath(ctx.options ? ctx.options->documentPath : std::string()));
    }
    std::set<std::string> used;
    eachLayer(root["layers"], [&](Json& layer) {
        if (titleart::layerTypeOf(layer) == titleart::LayerType::Text && !str(layer, "text_key").empty()) used.insert(str(layer, "text_key"));
    });
    eachLayer(root["layers"], [&](Json& layer) {
        if (titleart::layerTypeOf(layer) != titleart::LayerType::Text || !str(layer, "text_key").empty()) return;
        std::string base = slug(str(layer, "name"), ".-", 60);
        if (base.empty()) base = "text";
        std::string key = base;
        for (int n = 2; used.count(key); n++) key = base + "_" + std::to_string(n);
        used.insert(key);
        changed = ctx.set(&layer, "text_key", key) || changed;
    });
    return changed;
}

void editor::registerTitleLanguageOps() {
    TitleArtOps::registerOp("add_language", addLanguage);
    TitleArtOps::registerOp("remove_language", removeLanguage);
    TitleArtOps::registerOp("set_source_language", setSourceLanguage);
    TitleArtOps::registerOp("set_translation", setTranslation);
    TitleArtOps::registerOp("revert_translation", revertTranslation);
    TitleArtOps::registerOp("mark_reviewed", markReviewed);
}
