// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "FxGeneratorCost.h"

#include "titleart/TitleArtIO.h"
#include "titleart/effects/TfxCommon.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

void editor::appendTitleGeneratorCostRow(const Json& layer, const titleart::FxSize& desktop, const titleart::FxSize& mobile, titleart::FxTier tier,
                                         TitleFxCost& out) {
    if (titleart::layerTypeOf(layer) != titleart::LayerType::Generator || !layer.value("visible", true)) return;
    const Json& gen = layer.value("generator", Json::object());
    const titleart::GeneratorDef* def = titleart::findGenerator(gen.value("type", std::string()));
    if (!def) return;
    Json params = gen.value("params", Json::object());
    titleart::NormalizeContext normalize;
    titleart::normalizeParams(def->params, params, "params", normalize);
    const titleart::ParamValues values(params);
    TitleFxCostRow row;
    row.layer = layer.value("id", std::string());
    row.path = "gen";
    row.type = def->type;
    const titleart::FxCost d = def->estimate ? def->estimate(values, desktop, titleart::FxTier::Desktop) : titleart::tfx::CostBuilder().pass(1.0, 1).cost;
    const titleart::FxCost m = def->estimate ? def->estimate(values, mobile, titleart::FxTier::Mobile) : d;
    row.cost = tier == titleart::FxTier::Mobile ? m : d;
    row.costClass = titleart::tfx::costClassOf(row.cost);
    row.desktopMs = d.fre * TITLE_FX_DESKTOP_MS_PER_FRE + d.instances * TITLE_FX_DESKTOP_MS_PER_INSTANCE + d.cpuMs;
    row.mobileMs = m.fre * TITLE_FX_MOBILE_MS_PER_FRE + m.instances * TITLE_FX_MOBILE_MS_PER_INSTANCE + m.cpuMs * 2.5;
    row.liveKind = def->live.kind;
    for (const char* k : def->live.params) row.liveParams.push_back(k);
    out.rows.push_back(std::move(row));
}
