// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLanguageEditing.h"

#include "titleart/TitleArtOps.h"
#include "titleart/TitleTranslations.h"
#include "titleart/designer/TitleDesignerState.h"

#include <algorithm>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    // the first segment of a field path ("fit.mode" -> "fit", "chars.3.rotation" -> "chars")
    std::string head(const std::string& path) {
        return path.substr(0, path.find_first_of(".["));
    }

    const Json* resolve(const titleart::TitleArtDoc& doc, const Json& ref) {
        if (!ref.is_string()) return nullptr;
        titleart::TitleArtDoc copy = doc;
        titleart::TitleIdGenerator ids;
        TitleOpsResult result;
        TitleOpContext context(copy, ids, result);
        const Json* found = context.resolveLayer(ref.get<std::string>(), "layer");
        return found ? titleart::findLayer(doc.root, str(*found, "id")) : nullptr;
    }

    // one translation value of a layer path: size -> size_scale, transform.position -> offset
    bool translate(const Json& layer, const std::string& path, const Json& value, std::string& outPath, Json& outValue) {
        if (path == "size") {
            const double base = layer.value("size", 200.0);
            if (!value.is_number() || base <= 0) return false;
            outPath = "size_scale";
            outValue = value.get<double>() / base;
            return true;
        }
        if (path == "transform.position") {
            const titleart::TitleVec2 base = titleart::decodeVec2(layer["transform"].value("position", Json::array({0, 0})));
            const titleart::TitleVec2 p = titleart::decodeVec2(value);
            outPath = "offset";
            outValue = Json::array({p.x - base.x, p.y - base.y});
            return true;
        }
        outPath = path;
        outValue = value;
        return true;
    }

}

bool editor::titleFieldIsOverridable(const std::string& path) {
    if (path == "size" || path == "transform.position" || path == "text") return true;
    const std::string first = head(path);
    const std::vector<std::string>& keys = titleart::translationOverrideKeys();
    return first != "language" && first != "size_scale" && first != "offset" && std::find(keys.begin(), keys.end(), first) != keys.end();
}

Json editor::routeTitleLanguageOps(const titleart::TitleArtDoc& doc, const std::string& language, const Json& ops) {
    if (language.empty() || titleart::isSourceLanguage(doc.root, language) || !titleart::findTitleLanguage(doc.root, language)) return ops;
    const Json list = ops.is_array() ? ops : Json::array({ops});
    Json out = Json::array();
    for (const Json& op : list) {
        const std::string name = op.is_object() ? str(op, "op") : std::string();
        const Json* layer = op.is_object() && op.contains("layer") ? resolve(doc, op["layer"]) : nullptr;
        if (!layer) {
            out.push_back(op);
            continue;
        }
        const titleart::LayerType type = titleart::layerTypeOf(*layer);
        if (name == "set_text" && type == titleart::LayerType::Text && layer->value("translate", true)) {
            out.push_back(Json{{"op", "set_translation"}, {"layer", op["layer"]}, {"language", language}, {"text", op.value("text", std::string())}});
            continue;
        }
        if (name != "set" || !op.contains("values") || !op["values"].is_object()) {
            out.push_back(op);
            continue;
        }
        Json shared = Json::object(), translated = Json::object();
        std::string text;
        bool hasText = false;
        for (auto it = op["values"].begin(); it != op["values"].end(); ++it) {
            const std::string& path = it.key();
            const bool own = (type == titleart::LayerType::Text && layer->value("translate", true) && titleFieldIsOverridable(path)) ||
                             (type == titleart::LayerType::Image && path == "image") || (type == titleart::LayerType::Vector && path == "svg");
            if (!own) {
                shared[path] = it.value();
                continue;
            }
            if (path == "text") {
                text = it.value().is_string() ? it.value().get<std::string>() : std::string();
                hasText = true;
                continue;
            }
            std::string to;
            Json value;
            if (translate(*layer, path, it.value(), to, value)) translated[to] = value;
            else shared[path] = it.value();
        }
        if (!shared.empty()) out.push_back(Json{{"op", "set"}, {"layer", op["layer"]}, {"values", shared}});
        if (hasText || !translated.empty()) {
            Json t = Json{{"op", "set_translation"}, {"layer", op["layer"]}, {"language", language}};
            if (hasText) t["text"] = text;
            if (!translated.empty()) t["values"] = translated;
            out.push_back(t);
        }
    }
    return ops.is_array() ? out : (out.size() == 1 ? out[0] : out);
}

std::vector<std::string> editor::titleLayerOverrides(const Json& layer, const std::string& language) {
    std::vector<std::string> out;
    const Json* t = titleart::findTranslation(layer, language);
    if (!t || !t->is_object()) return out;
    for (const std::string& key : titleart::translationOverrideKeys()) {
        if (t->contains(key)) out.push_back(key);
    }
    for (const char* asset : {"image", "svg"}) {
        if (t->contains(asset)) out.push_back(asset);
    }
    return out;
}

Json editor::titleApplyToSourceOp(const titleart::TitleArtDoc& doc, const std::string& layerId, const std::string& language,
                                  const std::string& member) {
    const Json* layer = titleart::findLayer(doc.root, layerId);
    const Json* t = layer ? titleart::findTranslation(*layer, language) : nullptr;
    if (!t || !t->contains(member)) return nullptr;
    const Json& value = (*t)[member];
    Json values = Json::object();
    if (member == "size_scale" && value.is_number()) {
        values["size"] = layer->value("size", 200.0) * value.get<double>();
    } else if (member == "offset") {
        const titleart::TitleVec2 base = titleart::decodeVec2((*layer)["transform"].value("position", Json::array({0, 0})));
        const titleart::TitleVec2 o = titleart::decodeVec2(value);
        values["transform.position"] = Json::array({base.x + o.x, base.y + o.y});
    } else if (member == "fit" && value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) values["fit." + it.key()] = it.value();
    } else {
        values[member] = value;
    }
    return Json::array({Json{{"op", "set"}, {"layer", "id:" + layerId}, {"values", values}},
                        Json{{"op", "revert_translation"}, {"layer", "id:" + layerId}, {"language", language}, {"fields", Json::array({member})}}});
}

void editor::titleDesignerCycleLanguage(titledesigner::TitleDesignerState& state, int step) {
    if (!state.isOpen()) return;
    const std::vector<std::string> codes = titleart::titleLanguageCodes(state.committed().root);
    if (codes.size() < 2) return;
    // the source is "" in the state
    size_t at = 0;
    for (size_t i = 0; i < codes.size(); i++) {
        if (state.language().empty() ? titleart::isSourceLanguage(state.committed().root, codes[i]) : codes[i] == state.language()) at = i;
    }
    const size_t next = (at + codes.size() + (size_t)(step % (int)codes.size() + (int)codes.size())) % codes.size();
    state.setLanguage(codes[next]);
}
