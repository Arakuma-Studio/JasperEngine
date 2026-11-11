// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S83's effect catalogue on the CPU (docs/specs/S83-title-effects-generators.md §7):
// - TitleFxCatalogTest (AC1): the registry holds the 27 effects of §6.6 with complete
//   definitions (the catalogue's checks, aliases, a glyph safety, every param's animation class,
//   light colours marked, live variants naming their params and a program, the multiply rule of
//   §6.12); the format version is unchanged. Red control: a definition with a default outside its
//   range fails.
// - TitleFxBudget.StaticModel (AC14): every definition's static cost at defaults, at 1920 x 1080
//   on the desktop tier, falls in its declared class; its live kernel's within R35. Red control: a
//   definition declaring C1 with 5 FRE fails.
// - LightGlowMathTest (AC7, R19): the response's CPU twin is the identity bit for bit on colours
//   with every channel <= 1; the octave weights sum to 1; the mobile cascade keeps <= 6 octaves.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectCatalog.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/effects/LightEffects.h"
#include "titleart/effects/TfxCommon.h"
#include "titleart/effects/TfxPrograms.h"

#include <cmath>
#include <cstring>
#include <random>
#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    const std::vector<std::string> CATALOGUE = {
        "light_glow", "contour_lines", "motion_blur", "lens_blur", "tilt_shift", "radial_blur", "extrude", "motion_trail", "halftone",
        "perspective_shadow", "floor_reflection", "wave_warp", "chromatic_aberration", "grain", "glitch", "material", "shatter", "distress",
        "drips", "scanlines", "pixelate", "brush_stroke", "frost", "neon_tube", "chalk", "stitch", "holographic"};

    bool stepped(ParamKind k) {
        return k == ParamKind::Enum || k == ParamKind::Bool || k == ParamKind::Integer || k == ParamKind::Asset || k == ParamKind::Layer;
    }

}

TEST(TitleFxCatalogTest, EveryDefinitionComplete) {
    const TitleEffectRegistry& registry = TitleEffectRegistry::get();
    std::set<std::string> labels;
    for (const std::string& type : CATALOGUE) {
        SCOPED_TRACE(type);
        const EffectDef* d = registry.effect(type);
        ASSERT_NE(d, nullptr) << type << " is not registered";
        for (const std::string& problem : checkTitleEffectDef(*d)) ADD_FAILURE() << problem;
        EXPECT_TRUE(labels.insert(d->label).second) << "the label is used twice";
        EXPECT_FALSE(d->aliases.empty());
        EXPECT_NE(d->apply, nullptr);
        EXPECT_NE(d->estimate, nullptr) << "every S83 definition states its cost model";
        EXPECT_NE(d->presetFile, nullptr);
        if (d->presetFile) { EXPECT_EQ(std::string(d->presetFile), "editor/resources/titleart/fx-presets/" + type + ".yaml"); }
        const Json defaults = defaultParams(d->params);
        const ParamValues values(defaults);
        const bool light = d->addsLight && d->addsLight(values);
        for (const ParamDef& p : d->params) {
            SCOPED_TRACE(p.key);
            EXPECT_NE(p.anim, FxAnim::None) << "every param has an animation class";
            if (stepped(p.kind) && p.anim != FxAnim::Relayout) { EXPECT_EQ(p.interp, ParamInterp::Stepped); }
            // §6.3: the colours of an adds_light definition are light, but shadow colours
            if (light && (p.kind == ParamKind::Color || p.kind == ParamKind::Gradient)) { EXPECT_TRUE(p.light) << "a colour of light is marked"; }
            // §6.12: the intensities of light combine by multiplying
            const std::string key = p.key;
            if (light && (key == "intensity" || key == "glow" || key == "brightness")) { EXPECT_EQ(p.combine, ParamCombine::Multiply); }
        }
        if (d->live.kind != LiveKind::None) {
            EXPECT_FALSE(d->live.params.empty()) << "a live variant names the params it animates";
            EXPECT_GE(d->live.program, 1u);
            EXPECT_LE(d->live.program, (uint32_t)tfx::PROGRAM_LAST);
            for (const char* key : d->live.params) {
                const ParamDef* p = findParam(d->params, key);
                ASSERT_NE(p, nullptr) << key;
                EXPECT_EQ(p->anim, FxAnim::Live) << key << " is animated live, so it is marked L";
            }
        }
        // every L param is in the live variant
        for (const ParamDef& p : d->params) {
            if (p.anim != FxAnim::Live) continue;
            bool listed = std::any_of(d->live.params.begin(), d->live.params.end(), [&](const char* k) { return std::strcmp(k, p.key) == 0; });
            EXPECT_TRUE(listed) << p.key << " is marked L but no live variant animates it";
        }
    }
    EXPECT_EQ(TITLE_ART_VERSION, 1) << "S83's types and enum values need no version bump (R49)";
    for (const std::string& problem : checkTitleEffectRegistry(registry)) ADD_FAILURE() << problem;
}

// the red control of the completeness check: a default outside its range fails
TEST(TitleFxCatalogTest, BadDefaultFails) {
    EffectDef d;
    d.type = "test_bad_default";
    d.label = "Test Bad Default";
    d.category = "Glow";
    d.description = "A test definition";
    d.aliases = {"test"};
    ParamDef p;
    p.key = "amount";
    p.label = "Amount";
    p.kind = ParamKind::Number;
    p.min = 0;
    p.max = 1;
    p.defaultValue = 2.0;
    p.description = "Out of its range";
    d.params = {p};
    d.apply = [](EffectContext&, const TargetRef&, TargetRef&, const ParamValues&) {};
    EXPECT_FALSE(checkTitleEffectDef(d).empty());
}

// §6.4: the static model at defaults falls in the declared class, and a live kernel stays within R35
TEST(TitleFxBudget, StaticModel) {
    const FxSize desktop{1920, 1080, 1.0f};
    for (const std::string& type : CATALOGUE) {
        SCOPED_TRACE(type);
        const EffectDef* d = findEffect(type);
        ASSERT_NE(d, nullptr);
        ASSERT_NE(d->estimate, nullptr);
        Json defaults = defaultParams(d->params);
        const ParamValues values(defaults, &d->globalLight);
        const FxCost cost = d->estimate(values, desktop, FxTier::Desktop);
        EXPECT_GT(cost.passes, 0);
        const CostClass modelled = tfx::costClassOf(cost);
        EXPECT_LE((int)modelled, (int)d->costClass) << "modelled " << costClassName(modelled) << " (" << cost.fre << " FRE, "
                                                     << cost.maxTapsPerPixel << " taps) over the declared " << costClassName(d->costClass);
        // the mobile tier's estimate exists and runs its reductions
        const FxCost mobile = d->estimate(values, FxSize{1024, 512, 1.0f}, FxTier::Mobile);
        EXPECT_GT(mobile.passes, 0);
    }
}

TEST(TitleFxBudget, StaticModelRedControl) {
    tfx::CostBuilder b;
    b.pass(1.0, 1).pass(1.0, 4, 4);
    EXPECT_EQ(b.cost.fre, 5.0);
    EXPECT_GT((int)tfx::costClassOf(b.cost), (int)CostClass::C1) << "5 FRE is above C1's 3";
}

// R19: the response is the identity where no channel exceeds 1
TEST(LightGlowMathTest, ResponseIsIdentityBelowOne) {
    std::mt19937 rng(83);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (int i = 0; i < 1000000; i++) {
        const float in[3] = {unit(rng), unit(rng), unit(rng)};
        const float shift = unit(rng), white = unit(rng);
        float out[3];
        tfx::lightResponse(in, shift, white, out);
        ASSERT_EQ(std::memcmp(in, out, sizeof(in)), 0) << in[0] << " " << in[1] << " " << in[2];
    }
    // above 1 the light bleeds: a hot red pushes green up (orange, yellow, white)
    const float hot[3] = {3.0f, 0.2f, 0.1f};
    float out[3];
    tfx::lightResponse(hot, 0.6f, 0.2f, out);
    EXPECT_GT(out[1], hot[1]);
    EXPECT_LE(out[0], 1.5f);
}

TEST(LightGlowMathTest, OctaveWeightsSumToOne) {
    for (double falloff = 0.0; falloff <= 100.0; falloff += 12.5) {
        for (double radius : {4.0, 30.0, 200.0, 600.0, 1000.0}) {
            const auto octaves = tfx::psfOctaves(radius, tfx::falloffExponent(falloff), 0.0, 0, 1.0);
            double sum = 0.0;
            for (const auto& o : octaves) sum += o.weight;
            EXPECT_NEAR(sum, 1.0, 1e-6) << "falloff " << falloff << " radius " << radius;
            // the top octave's sigma is radius / 3
            EXPECT_NEAR(octaves.back().sigma[1], std::max(1.0, radius / 3.0), 1e-9);
        }
    }
    // the mobile tier spreads the span over at most 6 octaves (D11)
    EXPECT_LE(tfx::psfOctaves(1000.0, 2.4, 0.0, 6, 1.0).size(), 6u);
    // the power law: p = 1.6 at falloff 0 to 3.2 at 100
    EXPECT_DOUBLE_EQ(tfx::falloffExponent(0.0), 1.6);
    EXPECT_DOUBLE_EQ(tfx::falloffExponent(100.0), 3.2);
}

// D11: the cascade's per-level variance accounts for one tent per level it climbs
TEST(LightGlowMathTest, CascadeVariance) {
    EXPECT_EQ(tfx::glowLevelVariance(0, false), 0.0);
    EXPECT_NEAR(tfx::glowLevelVariance(1, false), 0.25 + 4.0 / 6.0, 1e-12);
    EXPECT_NEAR(tfx::glowLevelVariance(2, true), (16.0 - 1.0) / 12.0 + 4.0 / 6.0 + 16.0 / 6.0, 1e-12);
}
