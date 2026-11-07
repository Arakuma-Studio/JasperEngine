// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FxCost.h"

#include "titleart/TitleArtIO.h"
#include "titleart/effects/TfxCommon.h"

#include <algorithm>
#include <cstdio>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    double msOf(const titleart::FxCost& c, bool mobile) {
        return c.fre * (mobile ? TITLE_FX_MOBILE_MS_PER_FRE : TITLE_FX_DESKTOP_MS_PER_FRE) +
               c.instances * (mobile ? TITLE_FX_MOBILE_MS_PER_INSTANCE : TITLE_FX_DESKTOP_MS_PER_INSTANCE);
    }

    std::string ms(double v) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f", v);
        return text;
    }

}

TitleFxCost editor::estimateTitleFxCost(const titleart::TitleArtDoc& doc, titleart::FxTier tier) {
    TitleFxCost out;
    const int cw = std::max(1, doc.canvasWidth()), ch = std::max(1, doc.canvasHeight());
    // the desktop tier at the canvas size, the mobile tier on a 1024 x 512 surface
    const titleart::FxSize desktop{cw, ch, 1.0f};
    const float mobileScale = std::min(1024.0f / cw, 512.0f / ch);
    const titleart::FxSize mobile{std::max(1, (int)(cw * mobileScale)), std::max(1, (int)(ch * mobileScale)), mobileScale};
    const Json& light = doc.root.value("global_light", Json::object());
    const double lightAngle = light.value("angle", 120.0), lightAltitude = light.value("altitude", 30.0);
    titleart::forEachLayer(doc.root, [&](const Json& layer, const Json*, int) {
        const std::string id = layer.value("id", std::string());
        bool sdf = false;
        float sdfReach = 0.0f;
        auto effects = layer.find("effects");
        if (effects == layer.end() || !effects->is_array()) return true;
        for (const Json& fx : *effects) {
            if (!fx.value("enabled", true)) continue;
            const titleart::EffectDef* def = titleart::findEffect(fx.value("type", std::string()));
            if (!def) continue;
            Json params = fx.value("params", Json::object());
            titleart::NormalizeContext normalize;
            titleart::normalizeParams(def->params, params, "params", normalize);
            const titleart::ParamValues values(params, &def->globalLight, lightAngle, lightAltitude);
            TitleFxCostRow row;
            row.layer = id;
            row.path = std::string("fx:") + def->type;
            row.type = def->type;
            const titleart::FxCost d = def->estimate ? def->estimate(values, desktop, titleart::FxTier::Desktop) : titleart::tfx::CostBuilder().pass(1.0, 1).cost;
            const titleart::FxCost m = def->estimate ? def->estimate(values, mobile, titleart::FxTier::Mobile) : d;
            row.cost = tier == titleart::FxTier::Mobile ? m : d;
            row.costClass = titleart::tfx::costClassOf(row.cost);
            row.desktopMs = msOf(d, false);
            row.mobileMs = msOf(m, true);
            row.liveKind = def->live.kind;
            for (const char* k : def->live.params) row.liveParams.push_back(k);
            if (row.cost.sdf) {
                sdf = true;
                sdfReach = std::max(sdfReach, def->reach ? def->reach(values) : 0.0f) + (def->gather ? std::max(0.0f, def->gather(values)) : 0.0f);
            }
            out.rows.push_back(std::move(row));
        }
        if (sdf) {
            // the content's distance field: shared by the layer's effects, rebuilt once
            TitleFxCostRow row;
            row.layer = id;
            row.path = "sdf";
            row.type = "sdf";
            const titleart::FxCost d = titleart::tfx::sdfCost(sdfReach, desktop), m = titleart::tfx::sdfCost(sdfReach * mobileScale, mobile);
            row.cost = tier == titleart::FxTier::Mobile ? m : d;
            row.costClass = titleart::CostClass::C3;
            row.desktopMs = msOf(d, false);
            row.mobileMs = msOf(m, true);
            out.rows.push_back(std::move(row));
        }
        return true;
    });
    for (const TitleFxCostRow& r : out.rows) {
        out.desktopMs += r.desktopMs;
        out.mobileMs += r.mobileMs;
        out.total = std::max(out.total, r.costClass);
    }
    return out;
}

Json TitleFxCost::toJson() const {
    Json rowsJson = Json::array();
    for (const TitleFxCostRow& r : rows) {
        Json live = Json{{"kind", titleart::liveKindName(r.liveKind)}, {"params", r.liveParams}};
        if (r.liveKind != titleart::LiveKind::None) live["mobile_ms"] = r.liveKind == titleart::LiveKind::ScaleSplit ? TITLE_FX_MOBILE_MS_PER_FRE : r.mobileMs;
        rowsJson.push_back(Json{{"layer", r.layer}, {"path", r.path}, {"type", r.type}, {"class", titleart::costClassName(r.costClass)},
                                {"passes", r.cost.passes}, {"fre", r.cost.fre}, {"taps", r.cost.maxTapsPerPixel}, {"instances", r.cost.instances},
                                {"cpu_ms", r.cost.cpuMs}, {"sdf", r.cost.sdf}, {"analysis", r.cost.analysis}, {"readback", r.cost.readback},
                                {"desktop_ms", r.desktopMs}, {"mobile_ms", r.mobileMs}, {"live", live}});
    }
    return Json{{"rows", rowsJson}, {"class", titleart::costClassName(total)}, {"desktop_ms", desktopMs}, {"mobile_ms", mobileMs},
                {"mobile_predicted", true}};
}

std::string TitleFxCost::summary() const {
    std::string text = std::string("cost: ") + titleart::costClassName(total) + " total, desktop " + ms(desktopMs) + " ms, mobile " + ms(mobileMs) +
                       " ms predicted";
    for (size_t i = 0; i < rows.size() && i < 6; i++) {
        text += (i == 0 ? "; " : ", ") + rows[i].type + " " + titleart::costClassName(rows[i].costClass) + " " + ms(rows[i].desktopMs) + " ms";
    }
    return text;
}
