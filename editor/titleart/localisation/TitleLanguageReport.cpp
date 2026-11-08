// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "TitleLanguageReport.h"

#include "TitleLanguageNames.h"
#include "titleart/TitleArtBake.h"
#include "titleart/TitleTranslations.h"

#include <algorithm>
#include <cstdio>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    void eachLayer(const Json& list, const std::function<void(const Json&)>& visit) {
        if (!list.is_array()) return;
        for (const Json& layer : list) {
            visit(layer);
            if (auto children = layer.find("children"); children != layer.end()) eachLayer(*children, visit);
        }
    }

    // overflowing texts of a (resolved) language document
    int overflowing(const titleart::TitleArtDoc& doc, const titleart::AssetResolver* assets) {
        int count = 0;
        eachLayer(doc.root["layers"], [&](const Json& layer) {
            if (titleart::layerTypeOf(layer) != titleart::LayerType::Text || !layer.value("visible", true)) return;
            const Json fit = layer.value("fit", Json::object());
            const titleart::TitleVec2 box = titleart::decodeVec2(fit.value("box", Json::array({0, 0})));
            if (box.x <= 0 && box.y <= 0) return;
            titleart::TitleVec2 e;
            if (fit.value("mode", std::string("none")) == "none") e = titleart::measureTitleTextExtent(layer, doc, assets);
            else e = titleart::computeTitleFit(layer, doc, assets).extent;
            const double t = 1 + titleart::TITLE_FIT_TOLERANCE;
            if ((box.x > 0 && e.x > box.x * t) || (box.y > 0 && e.y > box.y * t)) count++;
        });
        return count;
    }

}

std::vector<TitleLanguageState> editor::titleLanguageStates(const titleart::TitleArtDoc& doc, const titleart::AssetResolver* assets, Project* project,
                                                            const std::filesystem::path& document) {
    std::vector<TitleLanguageState> out;
    if (!titleart::titleIsMultilingual(doc.root)) return out;
    for (const std::string& code : titleart::titleLanguageCodes(doc.root)) {
        TitleLanguageState s;
        s.code = code;
        s.source = titleart::isSourceLanguage(doc.root, code);
        const Json* language = titleart::findTitleLanguage(doc.root, code);
        s.name = language && !str(*language, "name").empty() ? str(*language, "name") : titleLanguageEnglishName(code);
        if (!s.source) {
            eachLayer(doc.root["layers"], [&](const Json& layer) {
                if (titleart::layerTypeOf(layer) != titleart::LayerType::Text || !layer.value("translate", true)) return;
                if (str(layer, "text").empty() || !titleart::layerShownInLanguage(layer, code)) return;
                const Json* t = titleart::findTranslation(layer, code);
                if (!titleart::translationHasText(t)) {
                    s.missing++;
                    return;
                }
                s.translated++;
                if (str(*t, "status") == "needs_review" || str(*t, "source_hash") != titleart::translationSourceHash(str(layer, "text"))) s.stale++;
            });
        }
        s.overflow = overflowing(titleart::resolveTitleLanguage(doc, code), assets);
        if (project && !document.empty() && assets) {
            if (!s.source && language && !language->value("bake", true)) {
                s.bake = "off";
            } else if (!s.source && !titleart::titleLanguageDiffers(doc, code)) {
                s.bake = "source";
            } else {
                const TitleBakeStatus status = titleArtBakeStatus(document, doc, *assets, s.source ? std::string() : code);
                bool missing = status.outputs.empty(), fresh = true;
                for (const TitleOutputState& o : status.outputs) {
                    missing = missing || o.status == TitleOutputStatus::Missing;
                    fresh = fresh && o.status == TitleOutputStatus::Fresh;
                }
                s.bake = missing ? "missing" : fresh ? "fresh" : "stale";
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

Json editor::titleLanguagesJson(const std::vector<TitleLanguageState>& states, const titleart::TitleArtDoc& doc) {
    Json list = Json::array();
    for (const TitleLanguageState& s : states) {
        Json item = Json{{"code", s.code}, {"name", s.name}, {"translated", s.translated}, {"missing", s.missing}, {"stale", s.stale},
                         {"overflow", s.overflow}};
        if (s.source) item["source"] = true;
        if (!s.bake.empty()) item["bake"] = s.bake;
        list.push_back(std::move(item));
    }
    return Json{{"source", titleart::titleSourceLanguage(doc.root)}, {"list", list}};
}

Json editor::titleLayerStringsJson(const Json& layer) {
    Json translated = Json::array();
    if (auto it = layer.find("translations"); it != layer.end() && it->is_object()) {
        for (auto t = it->begin(); t != it->end(); ++t) {
            if (titleart::translationHasText(&t.value()) || titleart::layerTypeOf(layer) != titleart::LayerType::Text) translated.push_back(t.key());
        }
    }
    Json out = Json::object();
    if (titleart::layerTypeOf(layer) == titleart::LayerType::Text) {
        out["text_key"] = str(layer, "text_key");
        out["translate"] = layer.value("translate", true);
        out["max_chars"] = layer.value("max_chars", 0);
        if (!str(layer, "translator_note").empty()) out["translator_note"] = str(layer, "translator_note");
    }
    out["translated"] = translated;
    if (auto only = layer.find("only_languages"); only != layer.end() && only->is_array() && !only->empty()) out["only_languages"] = *only;
    return out;
}

Json editor::titleFitJson(const titleart::TitleFitResult& fit) {
    return Json{{"scale", fit.scale}, {"h_scale", fit.hScale}, {"lines", fit.lines}, {"fits", fit.fits}, {"overflow_px", fit.overflowPx},
                {"extent", Json::array({fit.extent.x, fit.extent.y})}, {"box", Json::array({fit.box.x, fit.box.y})}};
}

std::string editor::titleLanguagesSummaryLine(const std::vector<TitleLanguageState>& states) {
    if (states.empty()) return {};
    std::string line = "Languages: ";
    int missing = 0, stale = 0, overflow = 0;
    for (size_t i = 0; i < states.size(); i++) {
        line += (i ? ", " : "") + states[i].code + (states[i].source ? " (source)" : "");
        missing += states[i].missing;
        stale += states[i].stale;
        overflow += states[i].overflow;
    }
    if (missing) line += " \xc2\xb7 " + std::to_string(missing) + " missing";
    if (stale) line += " \xc2\xb7 " + std::to_string(stale) + " stale";
    if (overflow) line += " \xc2\xb7 " + std::to_string(overflow) + " overflow";
    return line;
}

std::string editor::titleLanguageRowLabel(const titleart::TitleArtDoc& doc, const std::string& code, const titleart::AssetResolver* assets) {
    const titleart::TitleArtDoc resolved = titleart::resolveTitleLanguage(doc, code);
    double scale = 1.0;
    bool any = false;
    eachLayer(resolved.root["layers"], [&](const Json& layer) {
        if (titleart::layerTypeOf(layer) != titleart::LayerType::Text || !layer.value("visible", true)) return;
        const titleart::TitleFitResult fit = titleart::computeTitleFit(layer, resolved, assets);
        if (!fit.active) return;
        any = true;
        scale = std::min(scale, fit.scale * fit.hScale);
    });
    std::string label = code.empty() ? titleart::titleSourceLanguage(doc.root) : code;
    if (label.empty()) label = "source";
    if (any) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.2f", scale);
        label += " \xc2\xb7 fit " + std::string(buffer);
    }
    const int over = overflowing(resolved, assets);
    if (over) label += " \xc2\xb7 overflow " + std::to_string(over);
    return label;
}
