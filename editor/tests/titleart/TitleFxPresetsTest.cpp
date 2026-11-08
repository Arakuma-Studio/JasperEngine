// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S83's presets, recipes and their ops (docs/specs/S83-title-effects-generators.md R7, R8, R45, D4,
// §6.8, §6.11, AC1, AC14, AC18):
// - every S83 effect has a preset file; every preset parses, has a snake_case id unique in its
//   file and only known params that its definition takes unchanged; a preset heavier than its type
//   is at most one class heavier and says so (`cost`);
// - every recipe names an existing type and preset; style settings name their member;
// - add_effect {preset} applies the preset over the defaults, an unknown preset fails naming the
//   type's; apply_recipe adds the recipe's effect in one undo step named after it; an unknown type
//   or recipe fails naming the nearest (red controls of the validation).

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleArtOps.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/effects/TfxCommon.h"
#include "titleart/fx/FxPresets.h"
#include "titleart/fx/FxRecipes.h"

#include <cctype>
#include <set>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;

namespace {

    const std::vector<std::string> CATALOGUE = {
        "light_glow", "contour_lines", "motion_blur", "lens_blur", "tilt_shift", "radial_blur", "extrude", "motion_trail", "halftone",
        "perspective_shadow", "floor_reflection", "wave_warp", "chromatic_aberration", "grain", "glitch", "material", "shatter", "distress",
        "drips", "scanlines", "pixelate", "brush_stroke", "frost", "neon_tube", "chalk", "stitch", "holographic"};

    bool snakeCase(const std::string& s) {
        if (s.empty() || !std::islower((unsigned char)s[0])) return false;
        for (char c : s) {
            if (!(std::islower((unsigned char)c) || std::isdigit((unsigned char)c) || c == '_')) return false;
        }
        return true;
    }

    titleart::TitleArtDoc oneTitle() {
        const std::string text = R"({"format": "jasperengine.titleart", "version": 1,
          "layers": [{"id": "00000001", "type": "text", "name": "Title", "size": 200}]})";
        titleart::TitleArtParseResult parsed = titleart::parseTitleArt(text);
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return parsed.doc;
    }

    const TitleOpProblem* failure(const TitleOpsResult& r) {
        for (const TitleOpProblem& p : r.problems) {
            if (p.severity == "error") return &p;
        }
        return nullptr;
    }

}

TEST(TitleFxPresetsTest, EveryPresetIsValid) {
    for (const std::string& type : CATALOGUE) {
        SCOPED_TRACE(type);
        const titleart::EffectDef* def = titleart::findEffect(type);
        ASSERT_NE(def, nullptr);
        const std::vector<TitleFxPreset>& presets = TitleFxPresets::forType(type);
        EXPECT_FALSE(presets.empty()) << "editor/resources/titleart/fx-presets/" << type << ".yaml";
        std::set<std::string> ids;
        for (const TitleFxPreset& p : presets) {
            SCOPED_TRACE(p.id);
            EXPECT_TRUE(snakeCase(p.id));
            EXPECT_TRUE(ids.insert(p.id).second) << "the id is used twice";
            EXPECT_FALSE(p.name.empty());
            Json params = titleart::defaultParams(def->params);
            for (auto it = p.params.begin(); it != p.params.end(); ++it) {
                const titleart::ParamDef* param = titleart::findParam(def->params, it.key());
                ASSERT_NE(param, nullptr) << it.key() << " is not a parameter of " << type;
                Json value = it.value();
                std::vector<titleart::TitleWarning> warnings;
                titleart::NormalizeContext context;
                context.warnings = &warnings;
                EXPECT_TRUE(titleart::normalizeParamValue(*param, value, it.key(), context)) << it.key();
                EXPECT_TRUE(warnings.empty()) << it.key() << ": " << (warnings.empty() ? "" : warnings.front().message);
                params[it.key()] = value;
            }
            // §6.4: a preset is at most one class heavier than its type, and says so
            const titleart::ParamValues values(params, &def->globalLight);
            const titleart::CostClass modelled = titleart::tfx::costClassOf(def->estimate(values, titleart::FxSize{1920, 1080, 1.0f}, titleart::FxTier::Desktop));
            if ((int)modelled > (int)def->costClass) {
                // Distress's dry brush is C3 where its other patterns are C1 (§6.6.18): two classes
                const bool dryBrush = type == "distress" && p.id == "dry_brush";
                EXPECT_LE((int)modelled, (int)def->costClass + (dryBrush ? 2 : 1)) << "modelled " << titleart::costClassName(modelled);
                EXPECT_EQ(p.cost, titleart::costClassName(modelled)) << "the file states the heavier class";
            } else if (!p.cost.empty()) {
                EXPECT_EQ(p.cost, titleart::costClassName(modelled)) << "a stated class is the model's";
            }
        }
    }
}

TEST(TitleFxPresetsTest, ParseRejectsMalformedFiles) {
    std::vector<TitleFxPreset> out;
    std::string error;
    EXPECT_FALSE(TitleFxPresets::parse("presets: {id: x}", out, error));
    EXPECT_FALSE(TitleFxPresets::parse("presets:\n  - name: No Id\n", out, error));
    ASSERT_TRUE(TitleFxPresets::parse("presets:\n  - id: a\n    name: A\n    params: {color: \"#ff0000\", amount: 0.5, on: true}\n", out, error)) << error;
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].params["color"], "#ff0000");
    EXPECT_EQ(out[0].params["amount"], 0.5);
    EXPECT_EQ(out[0].params["on"], true);
}

TEST(TitleFxPresetsTest, RecipesAndStyleSettings) {
    std::set<std::string> ids;
    for (const TitleFxRecipe& r : titleFxRecipes()) {
        SCOPED_TRACE(r.id);
        EXPECT_TRUE(ids.insert(r.id).second);
        ASSERT_NE(titleart::findEffect(r.type), nullptr) << r.type;
        if (r.preset) EXPECT_NE(TitleFxPresets::find(r.type, r.preset), nullptr) << r.preset;
        const titleart::EffectDef* def = titleart::findEffect(r.type);
        const Json params = titleFxRecipeParams(r);
        ASSERT_TRUE(params.is_object());
        EXPECT_FALSE(params.empty());
        for (auto it = params.begin(); it != params.end(); ++it) {
            const titleart::ParamDef* param = titleart::findParam(def->params, it.key());
            ASSERT_NE(param, nullptr) << it.key();
            Json value = it.value();
            std::vector<titleart::TitleWarning> warnings;
            titleart::NormalizeContext context;
            context.warnings = &warnings;
            EXPECT_TRUE(titleart::normalizeParamValue(*param, value, it.key(), context)) << it.key();
            EXPECT_TRUE(warnings.empty()) << it.key() << ": " << (warnings.empty() ? "" : warnings.front().message);
        }
    }
    for (const char* id : {"long_shadow", "shine", "rainbow", "vignette", "emboss", "deboss", "icicles", "crt", "comic_dots", "ben_day"}) {
        EXPECT_NE(findTitleFxRecipe(id), nullptr) << id;
    }
    ASSERT_EQ(titleFxStyleSettings().size(), 2u);
    EXPECT_STREQ(titleFxStyleSettings()[0].member, "style.erode.enabled");
}

TEST(TitleFxPresetsTest, AddEffectAppliesAPreset) {
    const titleart::TitleArtDoc doc = oneTitle();
    const TitleOpsResult r = TitleArtOps::apply(doc, Json{{"op", "add_effect"}, {"layer", "Title"}, {"type", "light_glow"}, {"preset", "soft_bloom"},
                                                          {"params", {{"intensity", 2}}}});
    ASSERT_TRUE(r.ok) << r.error();
    const Json& params = r.doc.layers()[0]["effects"][0]["params"];
    EXPECT_EQ(params["radius"], 120);
    EXPECT_EQ(params["falloff"], 70);
    EXPECT_EQ(params["intensity"], 2) << "the op's params win over the preset";
    const TitleOpsResult bad = TitleArtOps::apply(doc, Json{{"op", "add_effect"}, {"layer", "Title"}, {"type", "light_glow"}, {"preset", "soft_blom"}});
    ASSERT_FALSE(bad.ok);
    ASSERT_NE(failure(bad), nullptr);
    EXPECT_EQ(failure(bad)->nearest, "soft_bloom");
    const TitleOpsResult typo = TitleArtOps::apply(doc, Json{{"op", "add_effect"}, {"layer", "Title"}, {"type", "lite_glow"}});
    ASSERT_FALSE(typo.ok);
    EXPECT_EQ(failure(typo)->nearest, "light_glow");
}

TEST(TitleFxPresetsTest, ApplyRecipeIsOneNamedStep) {
    registerTitleFxOps();
    const titleart::TitleArtDoc doc = oneTitle();
    const TitleOpsResult r = TitleArtOps::apply(doc, Json{{"op", "apply_recipe"}, {"layer", "Title"}, {"recipe", "long_shadow"}});
    ASSERT_TRUE(r.ok) << r.error();
    EXPECT_EQ(r.undoName, "Long Shadow");
    const Json& fx = r.doc.layers()[0]["effects"][0];
    EXPECT_EQ(fx["type"], "extrude");
    EXPECT_EQ(fx["params"]["length"], 260);
    EXPECT_EQ(fx["params"]["shading"], 0);
    const TitleOpsResult shine = TitleArtOps::apply(doc, Json{{"op", "apply_recipe"}, {"layer", "Title"}, {"recipe", "shine"}});
    ASSERT_TRUE(shine.ok) << shine.error();
    EXPECT_EQ(shine.doc.layers()[0]["effects"][0]["params"]["style"], "sweep");
    EXPECT_EQ(shine.doc.layers()[0]["effects"][0]["params"]["mode"], "light");
    const TitleOpsResult bad = TitleArtOps::apply(doc, Json{{"op", "apply_recipe"}, {"layer", "Title"}, {"recipe", "long_shadw"}});
    ASSERT_FALSE(bad.ok);
    EXPECT_EQ(failure(bad)->nearest, "long_shadow");
}
