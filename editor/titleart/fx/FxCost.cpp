// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FxCost.h"

#include <algorithm>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
namespace ta = jasperengine::titleart;

titleart::CostClass editor::titleFxCostClass(const ta::FxCost& c) {
    if (c.fre <= 1.0 && c.maxTapsPerPixel <= 16 && !c.sdf && !c.analysis) return ta::CostClass::C0;
    if (c.fre <= 3.0 && c.maxTapsPerPixel <= 64 && !c.sdf && !c.analysis) return ta::CostClass::C1;
    if (c.fre <= 10.0 && c.maxTapsPerPixel <= 200 && !c.sdf && !c.analysis) return ta::CostClass::C2;
    if (c.fre <= 40.0) return ta::CostClass::C3;
    return ta::CostClass::C4;
}

namespace {

    std::string shortMs(double v) {
        char t[32];
        std::snprintf(t, sizeof(t), "%.2g", v);
        return t;
    }

    ta::Json liveOf(const ta::LiveVariant& live) {
        ta::Json params = ta::Json::array();
        for (const char* p : live.params) params.push_back(p);
        return ta::Json{{"kind", ta::liveKindName(live.kind)}, {"params", params}};
    }

    struct Accum {
        int passes = 0;
        double fre = 0.0, desktopMs = 0.0, mobileMs = 0.0;
        ta::CostClass worst = ta::CostClass::C0;
    };

    ta::Json item(const std::string& layerId, const std::string& path, const char* type, const char* entry, ta::CostClass declared,
                  const ta::LiveVariant& live, const ta::FxCost& desktop, const ta::FxCost& mobile, ta::FxTier tier, Accum& total) {
        const ta::FxCost& shown = tier == ta::FxTier::Mobile ? mobile : desktop;
        const double desktopMs = desktop.fre * TITLE_FX_DESKTOP_MS_PER_FRE + desktop.instances * TITLE_FX_DESKTOP_MS_PER_INSTANCE + desktop.cpuMs;
        const double mobileMs = mobile.fre * TITLE_FX_MOBILE_MS_PER_FRE + mobile.instances * TITLE_FX_MOBILE_MS_PER_INSTANCE + mobile.cpuMs * 2.5;
        const ta::CostClass cls = std::max(titleFxCostClass(shown), declared == ta::CostClass::C4 ? ta::CostClass::C4 : ta::CostClass::C0);
        total.passes += shown.passes;
        total.fre += shown.fre;
        total.desktopMs += desktopMs;
        total.mobileMs += mobileMs;
        total.worst = std::max(total.worst, cls);
        return ta::Json{{"layer", layerId}, {"path", path}, {"type", type}, {"entry", entry}, {"class", ta::costClassName(cls)},
                        {"declared", ta::costClassName(declared)}, {"passes", shown.passes}, {"fre", shown.fre}, {"taps", shown.maxTapsPerPixel},
                        {"instances", shown.instances}, {"cpu_ms", shown.cpuMs}, {"sdf", shown.sdf}, {"analysis", shown.analysis},
                        {"readback", shown.readback}, {"desktop_ms", desktopMs}, {"mobile_ms", mobileMs}, {"live", liveOf(live)}};
    }

    ta::FxCost onePass() {
        ta::FxCost c;
        c.passes = 1;
        c.fre = 1.0;
        c.maxTapsPerPixel = 1;
        return c;
    }

}

ta::Json editor::estimateTitleFxCost(const ta::TitleArtDoc& doc, ta::FxTier tier) {
    const int w = std::max(1, doc.canvasWidth()), h = std::max(1, doc.canvasHeight());
    const ta::FxSize desktopSize{w, h, 1.0f};
    const float mobileScale = std::min(1.0f, 1024.0f / (float)w);
    const ta::FxSize mobileSize{(int)(w * mobileScale), (int)(h * mobileScale), mobileScale};
    Accum total;
    ta::Json items = ta::Json::array();
    ta::forEachLayer(doc.root, [&](const ta::Json& layer, const ta::Json*, int) {
        if (!layer.value("visible", true)) return true;
        const std::string id = layer.value("id", std::string());
        const std::string name = layer.value("name", std::string());
        if (ta::layerTypeOf(layer) == ta::LayerType::Generator) {
            const ta::Json& gen = layer.value("generator", ta::Json::object());
            if (const ta::GeneratorDef* def = ta::findGenerator(gen.value("type", std::string()))) {
                const ta::ParamValues p(gen.value("params", ta::Json::object()));
                const ta::FxCost d = def->estimate ? def->estimate(p, desktopSize, ta::FxTier::Desktop) : onePass();
                const ta::FxCost m = def->estimate ? def->estimate(p, mobileSize, ta::FxTier::Mobile) : onePass();
                items.push_back(item(id, name, def->type, "generator", def->costClass, def->live, d, m, tier, total));
            }
        }
        for (const ta::Json& e : layer.value("effects", ta::Json::array())) {
            if (!e.value("enabled", true)) continue;
            const ta::EffectDef* def = ta::findEffect(e.value("type", std::string()));
            if (!def) continue;
            const ta::ParamValues p(e.value("params", ta::Json::object()));
            const ta::FxCost d = def->estimate ? def->estimate(p, desktopSize, ta::FxTier::Desktop) : onePass();
            const ta::FxCost m = def->estimate ? def->estimate(p, mobileSize, ta::FxTier::Mobile) : onePass();
            items.push_back(item(id, name + "/fx:" + def->type, def->type, "effect", def->costClass, def->live, d, m, tier, total));
        }
        return true;
    });
    return ta::Json{{"tier", tier == ta::FxTier::Mobile ? "mobile" : "desktop"},
                    {"total", {{"class", ta::costClassName(total.worst)}, {"passes", total.passes}, {"fre", total.fre},
                               {"desktop_ms", total.desktopMs}, {"mobile_ms", total.mobileMs}}},
                    {"items", items}};
}

std::string editor::titleFxCostSummary(const ta::Json& cost) {
    const ta::Json& t = cost["total"];
    std::string text = "cost: " + t.value("class", std::string("C0")) + " total, desktop " + shortMs(t.value("desktop_ms", 0.0)) + " ms, mobile " +
                       shortMs(t.value("mobile_ms", 0.0)) + " ms predicted";
    std::string rows;
    for (const ta::Json& i : cost.value("items", ta::Json::array())) {
        rows += (rows.empty() ? "; " : ", ") + i.value("path", std::string()) + ": " + i.value("type", std::string()) + " " +
                i.value("class", std::string()) + " " + shortMs(i.value(cost.value("tier", std::string()) == "mobile" ? "mobile_ms" : "desktop_ms", 0.0)) +
                " ms";
    }
    return text + rows;
}
