// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// check_title's runtime cost checks (docs/specs/S83-title-effects-generators.md §6.11, D12, R35;
// S81 R9): fx_bake_only_realtime warns about an effect still C4 on the mobile tier and its fix
// removes it; fx_live_budget warns about an exposed live param whose compose is over the mobile
// live budget (fixed by lowering the costliest param) or follows the suffix rule, and passes a
// scale_split; fx_realtime_budget warns about a re-render over 15 ms on the mobile tier and its
// fix brings it under; a document no runtime title shows passes them; fx_hdr_on_8bit skips
// without a renderer (its render is the MCP test's).

#include <gtest/gtest.h>

#include "titleart/TitleArtBuiltinChecks.h"
#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/effects/TfxCommon.h"
#include "titleart/fx/FxCost.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::TitleArtDoc;

namespace {

    TitleArtDoc documentOf(Json layers, Json exposed = Json::array()) {
        Json root{{"format", titleart::TITLE_ART_FORMAT}, {"version", titleart::TITLE_ART_VERSION}, {"guid", "0123456789abcdef0123456789abcdef"}};
        root["canvas"] = Json{{"width", 1024}, {"height", 512}};
        root["layers"] = std::move(layers);
        root["exposed"] = std::move(exposed);
        titleart::TitleArtParseResult parsed = titleart::parseTitleArtJson(std::move(root));
        EXPECT_TRUE(parsed.ok) << parsed.error.message;
        return parsed.doc;
    }

    Json shape(const char* id, Json effects = Json::array()) {
        return Json{{"id", id}, {"type", "shape"}, {"name", "Plate"}, {"shape", "rect"}, {"size", 60}, {"effects", std::move(effects)}};
    }

    Json effect(const char* id, const char* type, Json params = Json::object()) {
        return Json{{"id", id}, {"type", type}, {"enabled", true}, {"params", std::move(params)}};
    }

    Json exposing(const char* name, const char* layer, const std::string& field) {
        return Json{{"name", name}, {"type", "number"}, {"targets", Json::array({Json{{"layer", layer}, {"field", field}}})}};
    }

    TitleCheckResult run(const char* id, const TitleArtDoc& doc, std::optional<std::vector<std::string>> instances = std::nullopt) {
        registerBuiltinTitleChecks();
        const CheckDef* check = TitleArtChecks::find(id);
        EXPECT_TRUE(check) << id;
        TitleCheckContext context(doc, "assets/titles/test.titleart");
        context.runtimeInstances = std::move(instances);
        std::vector<TitleCheckResult> results;
        if (check) check->run(context, results);
        EXPECT_EQ(results.size(), 1u);
        return results.empty() ? TitleCheckResult() : results[0];
    }

    // the first effect whose defaults are still C4 on the mobile tier (D12)
    const titleart::EffectDef* bakeOnlyEffect() {
        const titleart::FxSize mobile{1024, 512, 1.0f};
        for (const titleart::EffectDef* def : titleart::TitleEffectRegistry::get().effects()) {
            if (!def->estimate) continue;
            Json values = Json::object();
            titleart::NormalizeContext normalize;
            titleart::normalizeParams(def->params, values, "params", normalize);
            const titleart::FxCost cost = def->estimate(titleart::ParamValues(values, &def->globalLight), mobile, titleart::FxTier::Mobile);
            if (titleart::tfx::costClassOf(cost) == titleart::CostClass::C4) return def;
        }
        return nullptr;
    }

}

TEST(TitleFxRuntimeChecks, BakeOnlyEffectOnARuntimeTitleWarnsAndItsFixRemovesIt) {
    const titleart::EffectDef* def = bakeOnlyEffect();
    ASSERT_TRUE(def) << "a definition stays C4 on the mobile tier at its defaults";
    const TitleArtDoc doc = documentOf(Json::array({shape("00000001", Json::array({effect("00000101", def->type)}))}));
    const TitleCheckResult used = run("fx_bake_only_realtime", doc, std::vector<std::string>{"scene Main: Logo (realtime)"});
    EXPECT_EQ(used.status, TitleCheckStatus::Warn) << used.message;
    EXPECT_NE(used.message.find(def->label), std::string::npos) << used.message;
    ASSERT_TRUE(used.fix.is_object() && used.fix.contains("ops"));
    const TitleOpsResult fixed = TitleArtOps::apply(doc, used.fix["ops"]);
    ASSERT_TRUE(fixed.ok) << fixed.error();
    EXPECT_EQ(run("fx_bake_only_realtime", fixed.doc).status, TitleCheckStatus::Pass);
    // a document only Baked titles show renders it once
    const TitleCheckResult baked = run("fx_bake_only_realtime", doc, std::vector<std::string>{});
    EXPECT_EQ(baked.status, TitleCheckStatus::Pass);
    EXPECT_NE(baked.message.find("no Realtime or Glyph Atlas title"), std::string::npos) << baked.message;
}

TEST(TitleFxRuntimeChecks, LiveBudget) {
    // a scale_split composes its split in one full-resolution pass: within 0.5 ms on the mobile tier
    const TitleArtDoc glow = documentOf(Json::array({shape("00000001", Json::array({effect("00000101", "outer_glow")}))}),
                                        Json::array({exposing("glow", "00000001", "fx:outer_glow.intensity")}));
    const TitleCheckResult within = run("fx_live_budget", glow);
    EXPECT_EQ(within.status, TitleCheckStatus::Pass) << within.message;
    EXPECT_EQ(within.measured["live_params"].size(), 1u);

    // the suffix rule: a sweep before a Gaussian Blur re-renders
    const TitleArtDoc suffix = documentOf(Json::array({shape("00000001", Json::array({effect("00000101", "gradient_overlay"),
                                                                                       effect("00000102", "gaussian_blur")}))}),
                                          Json::array({exposing("sweep", "00000001", "fx:gradient_overlay.position")}));
    const TitleCheckResult after = run("fx_live_budget", suffix);
    EXPECT_EQ(after.status, TitleCheckStatus::Warn);
    EXPECT_NE(after.message.find("Gaussian Blur after Gradient Overlay"), std::string::npos) << after.message;

    // Particles' time is live; thousands of particles are over the mobile live budget: the fix
    // lowers the param that costs most and the check passes
    const titleart::GeneratorDef* particles = titleart::findGenerator("particles");
    ASSERT_TRUE(particles);
    ASSERT_TRUE(titleart::findParam(particles->params, "count"));
    Json gen{{"id", "00000002"}, {"type", "generator"}, {"name", "Sparks"}, {"generator", {{"type", "particles"}, {"params", {{"count", 5000}}}}}};
    const TitleArtDoc heavy = documentOf(Json::array({shape("00000001"), gen}), Json::array({exposing("t", "00000002", "gen.time")}));
    const TitleCheckResult over = run("fx_live_budget", heavy);
    ASSERT_EQ(over.status, TitleCheckStatus::Warn) << over.message;
    ASSERT_TRUE(over.fix.contains("ops")) << over.fix.dump();
    const TitleOpsResult fixed = TitleArtOps::apply(heavy, over.fix["ops"]);
    ASSERT_TRUE(fixed.ok) << fixed.error();
    const TitleCheckResult afterFix = run("fx_live_budget", fixed.doc);
    EXPECT_EQ(afterFix.status, TitleCheckStatus::Pass) << afterFix.message;
    // no runtime title: what it would cost, no warning
    EXPECT_EQ(run("fx_live_budget", heavy, std::vector<std::string>{}).status, TitleCheckStatus::Pass);
}

TEST(TitleFxRuntimeChecks, RealtimeBudgetWarnsAndItsFixFits) {
    // enough of the heaviest kind of the catalogue to pass 15 ms on the mobile tier
    const titleart::EffectDef* heavy = bakeOnlyEffect();
    ASSERT_TRUE(heavy);
    Json effects = Json::array();
    TitleArtDoc doc;
    for (int i = 0; i < 12; i++) {
        char id[16];
        std::snprintf(id, sizeof(id), "%08x", 0x101 + i);
        effects.push_back(effect(id, heavy->type));
        doc = documentOf(Json::array({shape("00000001", effects)}));
        if (estimateTitleFxCost(doc, titleart::FxTier::Mobile).mobileMs > 15.0) break;
    }
    const double before = estimateTitleFxCost(doc, titleart::FxTier::Mobile).mobileMs;
    ASSERT_GT(before, 15.0);
    const TitleCheckResult r = run("fx_realtime_budget", doc);
    EXPECT_EQ(r.status, TitleCheckStatus::Warn) << r.message;
    EXPECT_NE(r.message.find(heavy->label), std::string::npos) << r.message;
    if (r.fix.contains("ops")) {
        const TitleOpsResult fixed = TitleArtOps::apply(doc, r.fix["ops"]);
        ASSERT_TRUE(fixed.ok) << fixed.error();
        EXPECT_LT(estimateTitleFxCost(fixed.doc, titleart::FxTier::Mobile).mobileMs, before) << "the fix lowers the cost";
    }
    EXPECT_EQ(run("fx_realtime_budget", doc, std::vector<std::string>{}).status, TitleCheckStatus::Pass);
}

TEST(TitleFxRuntimeChecks, HdrOn8BitNeedsARender) {
    const TitleArtDoc doc = documentOf(Json::array({shape("00000001", Json::array({effect("00000101", "light_glow")}))}));
    const TitleCheckResult r = run("fx_hdr_on_8bit", doc);
    EXPECT_EQ(r.status, TitleCheckStatus::Skip);
    EXPECT_EQ(r.kind, TitleCheckKind::Pixels);
}

TEST(TitleFxRuntimeChecks, HdrOn8BitReportsLightAboveFourAsInfo) {
    // the check renders the document's light at 1/4: a pixel at the full code is light above 4.0
    registerBuiltinTitleChecks();
    const CheckDef* check = TitleArtChecks::find("fx_hdr_on_8bit");
    ASSERT_TRUE(check);
    const TitleArtDoc doc = documentOf(Json::array({shape("00000001", Json::array({effect("00000101", "light_glow")}))}));
    const auto runWith = [&](uint8_t alpha) {
        TitleCheckContext context(doc, "assets/titles/test.titleart");
        context.render = [alpha](const titleart::RenderRequest& request, TitleCheckImage& out, std::string&) {
            EXPECT_EQ(request.content, titleart::OutputContent::Glow);
            EXPECT_FLOAT_EQ(request.lightScale, 0.25f);
            out.width = 2;
            out.height = 2;
            out.rgba = {255, 255, 255, alpha, 10, 10, 10, 20, 0, 0, 0, 0, 0, 0, 0, 0};
            return true;
        };
        std::vector<TitleCheckResult> results;
        check->run(context, results);
        return results.at(0);
    };
    const TitleCheckResult over = runWith(255);
    EXPECT_EQ(over.status, TitleCheckStatus::Pass) << "info, not a warning";
    EXPECT_NE(over.message.find("info:"), std::string::npos) << over.message;
    EXPECT_DOUBLE_EQ(over.measured["over_4_fraction"].get<double>(), 0.25);
    const TitleCheckResult within = runWith(200);
    EXPECT_EQ(within.status, TitleCheckStatus::Pass);
    EXPECT_EQ(within.message.find("info:"), std::string::npos) << within.message;
    EXPECT_DOUBLE_EQ(within.measured["over_4_fraction"].get<double>(), 0.0);
}
