// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FxRuntimeChecks.h"

#include "FxCost.h"
#include "../TitleArtChecks.h"
#include "titleart/TitleArtIO.h"
#include "titleart/effects/TfxCommon.h"
#include "titleart/render/TitleArtRenderer.h"
#include "titleart/runtime/TitleLiveCompose.h"

#include <algorithm>
#include <cstdio>
#include <functional>

using namespace jasperengine;
using namespace jasperengine::editor;
using TJson = titleart::Json;

namespace {

    // S83 §6.11: a re-render over this on the mobile tier is fx_realtime_budget's
    constexpr double REALTIME_BUDGET_MS = 15.0;
    // §6.5: the share of light above 4.0 fx_hdr_on_8bit reports
    constexpr double HDR_LIMIT = 0.005;

    std::string ms(double v) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.2f", v);
        return text;
    }

    // §6.4's mobile tier surface: the canvas fit into 1024 x 512
    titleart::FxSize mobileSize(const titleart::TitleArtDoc& doc) {
        const int cw = std::max(1, doc.canvasWidth()), ch = std::max(1, doc.canvasHeight());
        const float scale = std::min(1024.0f / (float)cw, 512.0f / (float)ch);
        return titleart::FxSize{std::max(1, (int)((float)cw * scale)), std::max(1, (int)((float)ch * scale)), scale};
    }

    // one effect or generator of the document, as the checks walk them
    struct Item {
        std::string layer;          // layer id
        std::string layerName;
        int effect = -1;            // index in the layer's effects; -1: the generator layer
        std::string effectId;
        std::string label, type;
        std::string paramPrefix;    // "effects[2].params." or "generator.params."
        const std::vector<titleart::ParamDef>* params = nullptr;
        titleart::Json values;      // normalized
        titleart::FxCost (*estimate)(const titleart::ParamValues&, const titleart::FxSize&, titleart::FxTier) = nullptr;
        const titleart::GlobalLightBinding* light = nullptr;
    };

    std::vector<Item> itemsOf(const titleart::TitleArtDoc& doc) {
        std::vector<Item> out;
        titleart::forEachLayer(doc.root, [&](const TJson& layer, const TJson*, int) {
            const std::string id = layer.value("id", std::string());
            const std::string name = layer.value("name", id);
            if (titleart::layerTypeOf(layer) == titleart::LayerType::Generator) {
                const TJson& gen = layer.value("generator", TJson::object());
                const titleart::GeneratorDef* def = titleart::findGenerator(gen.value("type", std::string()));
                if (def && layer.value("visible", true)) {
                    Item item;
                    item.layer = id;
                    item.layerName = name;
                    item.label = def->label;
                    item.type = def->type;
                    item.paramPrefix = "generator.params.";
                    item.params = &def->params;
                    item.values = gen.value("params", TJson::object());
                    item.estimate = def->estimate;
                    item.light = def->globalLight.angleKey ? &def->globalLight : nullptr;
                    out.push_back(std::move(item));
                }
            }
            const TJson& effects = layer.value("effects", TJson::array());
            for (size_t i = 0; i < effects.size(); i++) {
                const TJson& fx = effects[i];
                if (!fx.value("enabled", true)) continue;
                const titleart::EffectDef* def = titleart::findEffect(fx.value("type", std::string()));
                if (!def) continue;
                Item item;
                item.layer = id;
                item.layerName = name;
                item.effect = (int)i;
                item.effectId = fx.value("id", std::string());
                item.label = def->label;
                item.type = def->type;
                item.paramPrefix = "effects[" + std::to_string(i) + "].params.";
                item.params = &def->params;
                item.values = fx.value("params", TJson::object());
                item.estimate = def->estimate;
                item.light = &def->globalLight;
                out.push_back(std::move(item));
            }
            return true;
        });
        for (Item& item : out) {
            titleart::NormalizeContext normalize;
            titleart::normalizeParams(*item.params, item.values, "params", normalize);
        }
        return out;
    }

    titleart::FxCost costOf(const titleart::TitleArtDoc& doc, const Item& item, const TJson& values) {
        if (!item.estimate) return titleart::tfx::CostBuilder().pass(1.0, 1).cost;
        const TJson& light = doc.root.value("global_light", TJson::object());
        const titleart::ParamValues v(values, item.light, light.value("angle", 120.0), light.value("altitude", 30.0));
        return item.estimate(v, mobileSize(doc), titleart::FxTier::Mobile);
    }

    double mobileMs(const titleart::FxCost& c) {
        return c.fre * titleart::tfx::MOBILE_MS_PER_FRE + c.instances * titleart::tfx::MOBILE_MS_PER_INSTANCE;
    }

    // the number param of `item` whose lowering saves the most on the mobile tier, set to the
    // largest value that brings `cost(values)` within `limit` (its minimum when none does); false
    // when no param lowers it
    bool lowerCostliestParam(const Item& item, const std::function<double(const TJson&)>& cost, double limit, TJson& op, TJson& about) {
        const double now = cost(item.values);
        const titleart::ParamDef* best = nullptr;
        double bestSaving = 0.0;
        for (const titleart::ParamDef& p : *item.params) {
            if ((p.kind != titleart::ParamKind::Number && p.kind != titleart::ParamKind::Integer) || !(p.min < p.max)) continue;
            if (!item.values.contains(p.key) || !item.values[p.key].is_number()) continue;
            TJson lowered = item.values;
            lowered[p.key] = p.min;
            const double saving = now - cost(lowered);
            if (saving > bestSaving + 1e-9) {
                bestSaving = saving;
                best = &p;
            }
        }
        if (!best) return false;
        const double current = item.values[best->key].get<double>();
        double lo = best->min, hi = current;
        auto at = [&](double v) {
            TJson values = item.values;
            values[best->key] = best->kind == titleart::ParamKind::Integer ? TJson((int64_t)std::floor(v)) : TJson(v);
            return cost(values);
        };
        double value = lo;
        if (at(lo) <= limit) {
            for (int i = 0; i < 24; i++) {
                const double mid = (lo + hi) * 0.5;
                if (at(mid) <= limit) lo = mid;
                else hi = mid;
            }
            value = lo;
        }
        const TJson stored = best->kind == titleart::ParamKind::Integer ? TJson((int64_t)std::floor(value)) : TJson(value);
        op = TJson{{"op", "set"}, {"layer", "id:" + item.layer}, {"values", TJson{{item.paramPrefix + best->key, stored}}}};
        about = TJson{{"layer", item.layerName}, {"effect", item.type}, {"param", best->key}, {"from", current}, {"to", stored},
                      {"mobile_ms", at(value)}};
        return true;
    }

    bool runtimeTitle(const TitleCheckContext& ctx) {
        return !ctx.runtimeInstances || !ctx.runtimeInstances->empty();
    }

    TJson instancesJson(const TitleCheckContext& ctx) {
        if (!ctx.runtimeInstances) return TJson(nullptr);
        TJson list = TJson::array();
        for (const std::string& s : *ctx.runtimeInstances) list.push_back(s);
        return list;
    }

    const char* noRuntimeTitle = "no Realtime or Glyph Atlas title of the project shows this document (Baked renders it once)";

    // ---- fx_bake_only_realtime (D12)

    void bakeOnlyRealtime(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out) {
        TitleCheckResult r;
        r.id = "fx_bake_only_realtime";
        r.kind = TitleCheckKind::Document;
        r.method = "each enabled effect and visible generator by S83's static cost model on the mobile tier (1024 x 512): C4 is bake only (D12)";
        r.threshold = TJson{{"mobile_class", "C3"}};
        TJson list = TJson::array();
        TJson ops = TJson::array();
        for (const Item& item : itemsOf(ctx.doc)) {
            const titleart::FxCost cost = costOf(ctx.doc, item, item.values);
            if (titleart::tfx::costClassOf(cost) != titleart::CostClass::C4) continue;
            list.push_back(TJson{{"layer", item.layerName}, {"effect", item.type}, {"label", item.label}, {"mobile_ms", mobileMs(cost)}});
            if (item.effect >= 0) ops.push_back(TJson{{"op", "remove_effect"}, {"layer", "id:" + item.layer}, {"effect", "id:" + item.effectId}});
            else ops.push_back(TJson{{"op", "remove_layer"}, {"layer", "id:" + item.layer}});
        }
        r.measured = TJson{{"bake_only", list}, {"runtime_instances", instancesJson(ctx)}};
        if (list.empty()) {
            r.message = "every effect and generator renders in Realtime on the mobile tier";
        } else if (!runtimeTitle(ctx)) {
            r.message = std::to_string(list.size()) + " effect(s) are bake only on the mobile tier; " + noRuntimeTitle;
        } else {
            r.status = TitleCheckStatus::Warn;
            std::string names;
            for (const TJson& e : list) names += (names.empty() ? "" : ", ") + e["label"].get<std::string>() + " (" + e["layer"].get<std::string>() + ")";
            r.message = "bake only on the mobile tier (C4): " + names +
                        ". On a phone or the web they are skipped when their layer changes at runtime: set Render Mode to Baked, or remove them";
            r.fix = TJson{{"ops", ops}, {"note", "set Render Mode to Baked on the Title Art Renderers that show this document"}};
        }
        out.push_back(std::move(r));
    }

    // ---- fx_live_budget (R35, S81 R9)

    void liveBudget(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out) {
        TitleCheckResult r;
        r.id = "fx_live_budget";
        r.kind = TitleCheckKind::Document;
        r.method = "each Exposed parameter whose targets are live effect params, as the runtime decides a change of it: the suffix rule (S83 "
                   "§6.12) and the live compose's prediction on the mobile tier (S83 §6.4, TitleLiveCompose)";
        const double budget = titleart::titleLiveBudgetMs(DevicePlatformGroup::Mobile);
        r.threshold = TJson{{"mobile_ms", budget}};
        TJson params = TJson::array();
        TJson over = TJson::array();
        TJson ops = TJson::array();
        const std::vector<Item> items = itemsOf(ctx.doc);
        for (const TJson& exposed : ctx.doc.root.value("exposed", TJson::array())) {
            if (!exposed.is_object()) continue;
            std::vector<titleart::TitleLiveTarget> changed;
            bool live = true;
            for (const TJson& target : exposed.value("targets", TJson::array())) {
                titleart::TitleLiveTarget t;
                if (!target.is_object() ||
                    !titleart::titleLiveTargetOf(ctx.doc, target.value("layer", std::string()), target.value("field", std::string()), t)) {
                    live = false;
                    break;
                }
                changed.push_back(std::move(t));
            }
            if (!live || changed.empty()) continue;
            const std::string name = exposed.value("name", std::string());
            const std::string suffix = titleart::titleLiveSuffixReason(ctx.doc, changed);
            const double cost = titleart::titleLiveCostMs(ctx.doc, changed, titleart::FxTier::Mobile);
            params.push_back(TJson{{"name", name}, {"mobile_ms", cost}, {"suffix", suffix}});
            if (suffix.empty() && cost <= budget) continue;
            TJson entry{{"name", name}, {"mobile_ms", cost}};
            if (!suffix.empty()) {
                entry["reason"] = suffix;
            } else {
                entry["reason"] = "the live compose is predicted at " + ms(cost) + " ms on the mobile tier, over " + ms(budget) + " ms";
                // the live definitions the change re-runs, costliest param first
                for (const titleart::TitleLiveTarget& t : changed) {
                    for (const Item& item : items) {
                        if (item.layer != t.layerId || item.effect != t.effect) continue;
                        TJson op, about;
                        const auto liveCost = [&](const TJson& values) {
                            titleart::TitleArtDoc trial = ctx.doc;
                            titleart::Json* layer = titleart::findLayer(trial.root, item.layer);
                            if (!layer) return cost;
                            if (item.effect >= 0) (*layer)["effects"][(size_t)item.effect]["params"] = values;
                            else (*layer)["generator"]["params"] = values;
                            return titleart::titleLiveCostMs(trial, changed, titleart::FxTier::Mobile);
                        };
                        if (lowerCostliestParam(item, liveCost, budget, op, about)) {
                            ops.push_back(op);
                            entry["lower"] = about;
                        }
                    }
                }
            }
            over.push_back(entry);
        }
        r.measured = TJson{{"live_params", params}, {"over", over}, {"runtime_instances", instancesJson(ctx)}};
        if (params.empty()) {
            r.message = "no Exposed parameter drives a live effect param";
        } else if (over.empty()) {
            r.message = std::to_string(params.size()) + " live Exposed parameter(s) compose within the mobile live budget";
        } else if (!runtimeTitle(ctx)) {
            r.message = std::to_string(over.size()) + " live Exposed parameter(s) would re-render; " + noRuntimeTitle;
        } else {
            r.status = TitleCheckStatus::Warn;
            std::string text;
            for (const TJson& e : over) text += (text.empty() ? "" : "; ") + e["name"].get<std::string>() + ": " + e["reason"].get<std::string>();
            r.message = "these Exposed parameters re-render on each change instead of composing live: " + text;
            r.fix = ops.empty() ? TJson{{"note", "bake the animation (S85 flipbook) or remove the effect after the live one"}}
                                : TJson{{"ops", ops}, {"note", "bake the animation (S85 flipbook)"}};
        }
        out.push_back(std::move(r));
    }

    // ---- fx_realtime_budget (§6.11)

    void realtimeBudget(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out) {
        TitleCheckResult r;
        r.id = "fx_realtime_budget";
        r.kind = TitleCheckKind::Document;
        r.method = "S83's static cost model of a full re-render on the mobile tier (1024 x 512), the shared distance fields included";
        r.threshold = TJson{{"mobile_ms", REALTIME_BUDGET_MS}};
        const TitleFxCost total = estimateTitleFxCost(ctx.doc, titleart::FxTier::Mobile);
        r.measured = TJson{{"mobile_ms", total.mobileMs}, {"class", titleart::costClassName(total.total)}, {"runtime_instances", instancesJson(ctx)}};
        if (total.mobileMs <= REALTIME_BUDGET_MS) {
            r.message = "a re-render is predicted at " + ms(total.mobileMs) + " ms on the mobile tier";
            out.push_back(std::move(r));
            return;
        }
        if (!runtimeTitle(ctx)) {
            r.message = "a re-render would take " + ms(total.mobileMs) + " ms on the mobile tier; " + noRuntimeTitle;
            out.push_back(std::move(r));
            return;
        }
        r.status = TitleCheckStatus::Warn;
        // the heaviest definition, and its costliest param lowered until the whole fits
        const std::vector<Item> items = itemsOf(ctx.doc);
        const Item* heaviest = nullptr;
        double heaviestMs = 0.0;
        for (const Item& item : items) {
            const double m = mobileMs(costOf(ctx.doc, item, item.values));
            if (m > heaviestMs) {
                heaviestMs = m;
                heaviest = &item;
            }
        }
        r.message = "a re-render is predicted at " + ms(total.mobileMs) + " ms on the mobile tier, over " + ms(REALTIME_BUDGET_MS) +
                    " ms (it takes many frames there)";
        if (heaviest) {
            r.message += "; the heaviest is " + heaviest->label + " on " + heaviest->layerName + " (" + ms(heaviestMs) + " ms)";
            const double rest = total.mobileMs - heaviestMs;
            TJson op, about;
            const auto cost = [&](const TJson& values) { return rest + mobileMs(costOf(ctx.doc, *heaviest, values)); };
            if (lowerCostliestParam(*heaviest, cost, REALTIME_BUDGET_MS, op, about)) {
                r.measured["lower"] = about;
                r.fix = TJson{{"ops", TJson::array({op})}, {"note", "set Render Mode to Baked"}};
            } else {
                r.fix = TJson{{"note", "set Render Mode to Baked, or remove " + heaviest->label}};
            }
        }
        out.push_back(std::move(r));
    }

    // ---- fx_hdr_on_8bit (§6.5, info)

    void hdrOn8bit(TitleCheckContext& ctx, std::vector<TitleCheckResult>& out) {
        TitleCheckResult r;
        r.id = "fx_hdr_on_8bit";
        r.kind = TitleCheckKind::Pixels;
        r.method = "the document's light (its Glow content) rendered on the float path at 1/4: a pixel at the full code holds light above 4.0, "
                   "which the RGBA8 path's headroom clips (S83 D14)";
        r.threshold = TJson{{"over_4_fraction", HDR_LIMIT}};
        if (!ctx.render) {
            r.status = TitleCheckStatus::Skip;
            r.message = "no renderer: the light is not measured";
            out.push_back(std::move(r));
            return;
        }
        titleart::RenderRequest request;
        request.doc = std::make_shared<titleart::TitleArtDoc>(ctx.doc);
        const int side = std::max(ctx.doc.canvasWidth(), ctx.doc.canvasHeight());
        request.pxScale = std::min(1.0f, 1024.0f / (float)std::max(side, 1));
        request.assets = ctx.assets;
        request.content = titleart::OutputContent::Glow;
        request.lightScale = 0.25f;
        TitleCheckImage image;
        std::string error;
        if (!ctx.render(request, image, error)) {
            r.status = TitleCheckStatus::Skip;
            r.message = "the light did not render: " + error;
            out.push_back(std::move(r));
            return;
        }
        size_t lit = 0, over = 0;
        for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
            const uint8_t a = image.rgba[i + 3];
            if (a == 0) continue;
            lit++;
            // the output pass dithers under half a code: 254 and up is the full code
            if (a >= 254 && std::max({image.rgba[i], image.rgba[i + 1], image.rgba[i + 2]}) >= 254) over++;
        }
        const size_t pixels = std::max<size_t>(1, image.rgba.size() / 4);
        const double fraction = (double)over / (double)pixels;
        r.measured = TJson{{"over_4_fraction", fraction}, {"lit_fraction", (double)lit / (double)pixels}};
        if (fraction > HDR_LIMIT) {
            char text[200];
            std::snprintf(text, sizeof(text),
                          "info: %.2f %% of the canvas holds light above 4.0: devices without float targets (the RGBA8 path) clip it there", fraction * 100.0);
            r.message = text;
        } else {
            r.message = "the light stays within the RGBA8 path's headroom (4.0)";
        }
        out.push_back(std::move(r));
    }

    void add(const char* id, TitleCheckKind kind, const char* description, void (*run)(TitleCheckContext&, std::vector<TitleCheckResult>&)) {
        CheckDef def;
        def.id = id;
        def.kind = kind;
        def.owner = "S83";
        def.description = description;
        def.run = run;
        TitleArtChecks::registerCheck(std::move(def));
    }

}

void editor::registerTitleFxRuntimeChecks() {
    add("fx_bake_only_realtime", TitleCheckKind::Document, "Effects still bake only (C4) on the mobile tier in a document a Realtime title shows",
        bakeOnlyRealtime);
    add("fx_live_budget", TitleCheckKind::Document, "Exposed live effect parameters whose changes re-render instead of composing live", liveBudget);
    add("fx_realtime_budget", TitleCheckKind::Document, "A re-render predicted over 15 ms on the mobile tier", realtimeBudget);
    add("fx_hdr_on_8bit", TitleCheckKind::Pixels, "Light above 4.0, which the RGBA8 path clips (info)", hdrOn8bit);
}
