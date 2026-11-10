// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// check_title's language checks (docs/specs/S87-title-localisation-interchange.md R11, R18):
// languages_complete, translation_stale, text_overflow, fit_scale_low, fit_without_box,
// project_language_missing and translation_unused, per language of `languages` ("all" default),
// each with the language, the layer path and fix ops; a fix that needs content is marked
// needs_input with the source text as its placeholder. Overflow fixes are offered only after fit
// re-ran with them and they fit. A monolingual document runs none of them (R50); S80's
// glyphs_present and bake_fresh run per language through CheckDef::perLanguage.

#include "TitleLanguageChecks.h"

#include "titleart/TitleArtChecks.h"
#include "titleart/TitleFit.h"
#include "titleart/TitleTranslations.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    using Out = std::vector<TitleCheckResult>;

    std::string str(const Json& object, const char* key) {
        auto it = object.find(key);
        return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    std::string lower(std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    }

    TitleCheckResult result(TitleCheckStatus status, const std::string& language, const std::string& message, const std::string& method) {
        TitleCheckResult r;
        r.status = status;
        r.language = language;
        r.message = message;
        r.method = method;
        return r;
    }

    struct LayerAt {
        const Json* layer = nullptr;
        std::string path;
    };

    // every layer with its §6.2 path ("Logo/Title#1")
    void layersWithPaths(const Json& list, const std::string& prefix, std::vector<LayerAt>& out) {
        if (!list.is_array()) return;
        std::map<std::string, int> counts;
        for (const Json& layer : list) {
            const std::string name = str(layer, "name");
            const int n = counts[name]++;
            const std::string path = prefix + name + (n ? "#" + std::to_string(n) : "");
            out.push_back({&layer, path});
            if (auto children = layer.find("children"); children != layer.end()) layersWithPaths(*children, path + "/", out);
        }
    }

    std::vector<LayerAt> layersOf(const titleart::TitleArtDoc& doc) {
        std::vector<LayerAt> out;
        layersWithPaths(doc.root["layers"], "", out);
        return out;
    }

    // the translation languages of a run (the source left out)
    std::vector<std::string> translationLanguages(TitleCheckContext& ctx) {
        std::vector<std::string> out;
        for (const std::string& code : titleCheckLanguages(ctx)) {
            if (!titleart::isSourceLanguage(ctx.doc.root, code)) out.push_back(code);
        }
        return out;
    }

    bool applies(TitleCheckContext& ctx, Out& out, const char* method) {
        if (titleart::titleIsMultilingual(ctx.doc.root)) return true;
        out.push_back(result(TitleCheckStatus::Skip, "", "a monolingual title (no languages)", method));
        return false;
    }

    // ---- languages_complete ----

    void languagesComplete(TitleCheckContext& ctx, Out& out) {
        if (!applies(ctx, out, "every translatable text layer has a Translation text per language")) return;
        for (const std::string& code : translationLanguages(ctx)) {
            Json missing = Json::array();
            Json ops = Json::array();
            for (const LayerAt& at : layersOf(ctx.doc)) {
                const Json& layer = *at.layer;
                if (titleart::layerTypeOf(layer) != titleart::LayerType::Text || !layer.value("translate", true)) continue;
                if (str(layer, "text").empty() || !titleart::layerShownInLanguage(layer, code)) continue;
                if (titleart::translationHasText(titleart::findTranslation(layer, code))) continue;
                missing.push_back(at.path);
                ops.push_back(Json{{"op", "set_translation"}, {"layer", at.path}, {"language", code}, {"text", str(layer, "text")}});
            }
            TitleCheckResult r = result(missing.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Fail, code,
                                        missing.empty() ? "every text is translated" : std::to_string(missing.size()) + " layer(s) have no " + code + " text",
                                        "a Translation `text` on each translatable text layer shown in the language");
            r.measured = Json{{"missing", missing}};
            r.threshold = Json{{"missing", 0}};
            if (!missing.empty()) {
                // the text is the agent's to write: the source text is a placeholder
                r.fix = Json{{"ops", ops}, {"needs_input", true},
                             {"alternative", Json{{"ops", Json::array({Json{{"op", "set"}, {"layer", missing[0]}, {"values", Json{{"translate", false}}}}})}}}};
            }
            out.push_back(std::move(r));
        }
    }

    // ---- translation_stale ----

    void translationStale(TitleCheckContext& ctx, Out& out) {
        if (!applies(ctx, out, "source_hash against the source text, and status")) return;
        for (const std::string& code : translationLanguages(ctx)) {
            Json stale = Json::array();
            for (const LayerAt& at : layersOf(ctx.doc)) {
                const Json& layer = *at.layer;
                if (titleart::layerTypeOf(layer) != titleart::LayerType::Text) continue;
                const Json* t = titleart::findTranslation(layer, code);
                if (!titleart::translationHasText(t)) continue;
                const bool review = str(*t, "status") == "needs_review";
                const bool changed = str(*t, "source_hash") != titleart::translationSourceHash(str(layer, "text"));
                if (review || changed) stale.push_back(Json{{"layer", at.path}, {"reason", review ? "needs_review" : "source_changed"}});
            }
            TitleCheckResult r = result(stale.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn, code,
                                        stale.empty() ? "no translation is stale" : std::to_string(stale.size()) + " " + code + " translation(s) to review",
                                        "source_hash (FNV-1a of the source text) and status of each Translation");
            r.measured = Json{{"stale", stale}};
            r.threshold = Json{{"stale", 0}};
            if (!stale.empty()) {
                Json layers = Json::array();
                for (const Json& s : stale) layers.push_back(s["layer"]);
                r.fix = Json{{"ops", Json::array({Json{{"op", "mark_reviewed"}, {"language", code}, {"layers", layers}}})}};
            }
            out.push_back(std::move(r));
        }
    }

    // ---- fit: text_overflow, fit_scale_low, fit_without_box ----

    // the document of a language, its fit not applied (the checks measure it)
    titleart::TitleArtDoc languageDoc(const TitleCheckContext& ctx, const std::string& code) {
        return titleart::resolveTitleLanguage(ctx.doc, code);
    }

    // the fix op that writes `values` into the layer's fit for the language
    Json fitOp(const TitleCheckContext& ctx, const std::string& code, const std::string& layer, const Json& values) {
        if (titleart::isSourceLanguage(ctx.doc.root, code)) {
            Json v = Json::object();
            for (auto it = values.begin(); it != values.end(); ++it) v["fit." + it.key()] = it.value();
            return Json{{"op", "set"}, {"layer", layer}, {"values", v}};
        }
        Json v = Json::object();
        for (auto it = values.begin(); it != values.end(); ++it) v["fit." + it.key()] = it.value();
        return Json{{"op", "set_translation"}, {"layer", layer}, {"language", code}, {"values", v}};
    }

    // the overflow candidates that fit once fit re-runs with them (R18)
    Json fitCandidates(TitleCheckContext& ctx, const std::string& code, const titleart::TitleArtDoc& doc, const Json& layer,
                       const std::string& path, const titleart::TitleFitResult& current, bool lowerMinScale) {
        Json ops = Json::array();
        const auto tryFit = [&](const Json& values) {
            Json candidate = layer;
            Json& fit = candidate["fit"];
            if (!fit.is_object()) fit = Json::object();
            for (auto it = values.begin(); it != values.end(); ++it) fit[it.key()] = it.value();
            const titleart::TitleFitResult r = titleart::computeTitleFit(candidate, doc, ctx.assets);
            if (r.fits && (lowerMinScale || r.scale * r.hScale >= 0.75 - 1e-9)) ops.push_back(fitOp(ctx, code, path, values));
        };
        const titleart::TitleVec2 box = titleart::decodeVec2(layer["fit"].value("box", Json::array({0, 0})));
        if (box.x > 0 || box.y > 0) {
            tryFit(Json{{"mode", "condense"}});
            tryFit(Json{{"mode", "wrap"}, {"max_lines", 2}});
            tryFit(Json{{"mode", "wrap"}, {"max_lines", 3}});
            if (lowerMinScale) {
                // the min_scale that fits (>= 0.4): the box over the measured extent
                const titleart::TitleVec2 base = titleart::measureTitleTextExtent(layer, doc, ctx.assets);
                double s = 1.0;
                if (box.x > 0 && base.x > 0) s = std::min(s, box.x / base.x);
                if (box.y > 0 && base.y > 0) s = std::min(s, box.y / base.y);
                s = std::floor(s * 100.0) / 100.0;
                if (s >= 0.4) tryFit(Json{{"mode", "shrink"}, {"min_scale", s}});
            }
        }
        // a box widened to the extent (the last resort: it changes the layout): the extent at
        // the floor for an overflow, the full text for a shrink that is merely low
        const titleart::TitleVec2 extent = lowerMinScale ? current.extent : titleart::measureTitleTextExtent(layer, doc, ctx.assets);
        if (extent.x > 0 && extent.y > 0) {
            ops.push_back(fitOp(ctx, code, path, Json{{"box", Json::array({std::ceil(std::max(extent.x, box.x)), std::ceil(std::max(extent.y, box.y))})}}));
        }
        return ops;
    }

    void fitChecks(TitleCheckContext& ctx, Out& out, const char* which) {
        const std::string id = which;
        if (!applies(ctx, out, "geometry: outline ink grown by the rims against fit.box")) return;
        for (const std::string& code : titleCheckLanguages(ctx)) {
            const titleart::TitleArtDoc doc = languageDoc(ctx, code);
            Json measured = Json::array();
            Json ops = Json::array();
            std::string first;
            for (const LayerAt& at : layersOf(doc)) {
                const Json& layer = *at.layer;
                if (titleart::layerTypeOf(layer) != titleart::LayerType::Text || !layer.value("visible", true)) continue;
                const Json fit = layer.value("fit", Json::object());
                const std::string mode = fit.value("mode", std::string("none"));
                const titleart::TitleVec2 box = titleart::decodeVec2(fit.value("box", Json::array({0, 0})));
                const bool hasBox = box.x > 0 || box.y > 0;
                if (id == "fit_without_box") {
                    if (mode != "none" && !hasBox) {
                        measured.push_back(at.path);
                        const titleart::TitleVec2 e = titleart::measureTitleTextExtent(layer, doc, ctx.assets);
                        if (e.x > 0) ops.push_back(fitOp(ctx, code, at.path, Json{{"box", Json::array({std::ceil(e.x * 1.1), std::ceil(e.y * 1.1)})}}));
                    }
                    continue;
                }
                if (!hasBox) continue;
                titleart::TitleFitResult r;
                if (mode == "none") {
                    // no fit and a box: the plain extent against it
                    r.extent = titleart::measureTitleTextExtent(layer, doc, ctx.assets);
                    r.box = box;
                    r.overflowPx = std::max(box.x > 0 ? r.extent.x - box.x : 0.0, box.y > 0 ? r.extent.y - box.y : 0.0);
                } else {
                    r = titleart::computeTitleFit(layer, doc, ctx.assets);
                }
                const double tolerance = titleart::TITLE_FIT_TOLERANCE;
                const bool over = (box.x > 0 && r.extent.x > box.x * (1 + tolerance)) || (box.y > 0 && r.extent.y > box.y * (1 + tolerance));
                const double scale = r.scale * r.hScale;
                if (id == "text_overflow" && over) {
                    measured.push_back(Json{{"layer", at.path}, {"extent", Json::array({r.extent.x, r.extent.y})}, {"box", Json::array({box.x, box.y})},
                                            {"overflow_px", r.overflowPx}, {"scale", scale}});
                    if (first.empty()) first = at.path;
                    for (const Json& op : fitCandidates(ctx, code, doc, layer, at.path, r, true)) ops.push_back(op);
                } else if (id == "fit_scale_low" && mode != "none" && scale < 0.75) {
                    measured.push_back(Json{{"layer", at.path}, {"scale", scale}});
                    if (first.empty()) first = at.path;
                    for (const Json& op : fitCandidates(ctx, code, doc, layer, at.path, r, false)) ops.push_back(op);
                }
            }
            TitleCheckStatus status = TitleCheckStatus::Pass;
            std::string message;
            if (id == "text_overflow") {
                status = measured.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Fail;
                message = measured.empty() ? "every text fits its box" : std::to_string(measured.size()) + " text(s) overflow their box (" + first + ")";
            } else if (id == "fit_scale_low") {
                status = measured.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn;
                message = measured.empty() ? "no fit shrinks below 0.75" : std::to_string(measured.size()) + " text(s) shrink below 0.75 (" + first + ")";
            } else {
                status = measured.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn;
                message = measured.empty() ? "every fit has a box" : std::to_string(measured.size()) + " fit(s) have no box: the text renders unfitted";
            }
            TitleCheckResult result1 = result(status, code, message, "geometry: outline ink grown by the rims against fit.box (2 % tolerance)");
            result1.measured = Json{{"layers", measured}};
            result1.threshold = id == "fit_scale_low" ? Json{{"scale", 0.75}} : Json{{"tolerance", titleart::TITLE_FIT_TOLERANCE}};
            if (!ops.empty()) result1.fix = Json{{"ops", ops}, {"choose", "one"}};
            out.push_back(std::move(result1));
        }
    }

    // ---- project_language_missing ----

    void projectLanguageMissing(TitleCheckContext& ctx, Out& out) {
        bool translatable = false;
        for (const LayerAt& at : layersOf(ctx.doc)) {
            const Json& layer = *at.layer;
            if (titleart::layerTypeOf(layer) == titleart::LayerType::Text && layer.value("translate", true) && !str(layer, "text").empty()) translatable = true;
        }
        if (ctx.options.projectLanguages.empty() || !translatable) {
            out.push_back(result(TitleCheckStatus::Skip, "", ctx.options.projectLanguages.empty() ? "the project lists no languages" : "no translatable text",
                                 "localization.languages against the title's languages"));
            return;
        }
        const std::vector<std::string> have = titleart::titleLanguageCodes(ctx.doc.root);
        Json missing = Json::array();
        Json ops = Json::array();
        for (const std::string& code : ctx.options.projectLanguages) {
            if (std::any_of(have.begin(), have.end(), [&](const std::string& h) { return lower(h) == lower(code); })) continue;
            missing.push_back(code);
            ops.push_back(Json{{"op", "add_language"}, {"code", code}});
        }
        TitleCheckResult r = result(missing.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn, "",
                                    missing.empty() ? "the title has every project language" : "the title lacks " + std::to_string(missing.size()) + " project language(s)",
                                    "localization.languages against the title's languages");
        r.measured = Json{{"missing", missing}};
        if (!missing.empty()) r.fix = Json{{"ops", ops}};
        out.push_back(std::move(r));
    }

    // ---- translation_unused ----

    void translationUnused(TitleCheckContext& ctx, Out& out) {
        if (!applies(ctx, out, "Translations of untranslated layers or of languages the title lacks")) return;
        Json unused = Json::array();
        Json ops = Json::array();
        const std::vector<std::string> have = titleart::titleLanguageCodes(ctx.doc.root);
        for (const LayerAt& at : layersOf(ctx.doc)) {
            const Json& layer = *at.layer;
            auto translations = layer.find("translations");
            if (translations == layer.end() || !translations->is_object()) continue;
            for (auto it = translations->begin(); it != translations->end(); ++it) {
                const bool known = std::any_of(have.begin(), have.end(), [&](const std::string& h) { return lower(h) == lower(it.key()); }) &&
                                   !titleart::isSourceLanguage(ctx.doc.root, it.key());
                const bool off = titleart::layerTypeOf(layer) == titleart::LayerType::Text && !layer.value("translate", true);
                if (known && !off) continue;
                unused.push_back(Json{{"layer", at.path}, {"language", it.key()}, {"reason", known ? "translate_false" : "language_missing"}});
                ops.push_back(Json{{"op", "revert_translation"}, {"layer", at.path}, {"language", it.key()}, {"fields", Json::array({"all"})}});
            }
        }
        TitleCheckResult r = result(unused.empty() ? TitleCheckStatus::Pass : TitleCheckStatus::Warn, "",
                                    unused.empty() ? "every Translation is used" : std::to_string(unused.size()) + " Translation(s) are kept but unused",
                                    "Translations of layers with translate false, or of languages not in the title");
        r.measured = Json{{"unused", unused}};
        if (!unused.empty()) r.fix = Json{{"ops", ops}};
        out.push_back(std::move(r));
    }

    struct LanguageCheck {
        const char* id;
        const char* description;
        std::function<void(TitleCheckContext&, Out&)> run;
    };

}

void editor::registerTitleLanguageChecks() {
    static bool done = false;
    if (done) return;
    done = true;
    const LanguageCheck checks[] = {
        {"languages_complete", "Every translatable text has a translation per language", languagesComplete},
        {"translation_stale", "Translations whose source text changed or that need review", translationStale},
        {"text_overflow", "Texts that overflow their fit box after fit, per language", [](TitleCheckContext& c, Out& o) { fitChecks(c, o, "text_overflow"); }},
        {"fit_scale_low", "Fits that shrink a text below 0.75", [](TitleCheckContext& c, Out& o) { fitChecks(c, o, "fit_scale_low"); }},
        {"fit_without_box", "A fit mode without a box: the text renders unfitted", [](TitleCheckContext& c, Out& o) { fitChecks(c, o, "fit_without_box"); }},
        {"project_language_missing", "The project's languages the title lacks", projectLanguageMissing},
        {"translation_unused", "Translations nothing uses (kept)", translationUnused},
    };
    for (const LanguageCheck& c : checks) {
        CheckDef def;
        def.id = c.id;
        def.kind = TitleCheckKind::Document;
        def.owner = "S87";
        def.group = "languages";
        def.description = c.description;
        def.run = c.run;
        TitleArtChecks::registerCheck(std::move(def));
    }
}
