// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S83's effect catalogue on the GPU (docs/specs/S83-title-effects-generators.md §7). Each
// definition runs as the renderer runs it, its `apply` on the standard title (S80 §7) uploaded into
// a pooled target, through TitleFxRig.h:
// - TitleFxGoldenGpu.Effects (AC2): every S83 effect at defaults and with its first two presets of
//   §6.8 against golden/titleart/effects/<type>[-<preset>].png (S80's folder and naming, whose list
//   check covers them; written on glcore, Vulkan matches), with
//   S80 §6.12's golden limits; TitleFxGoldenGpu.PatternKinds: each pattern kind of §6.6.28 through
//   Pattern Fill. TitleFxGoldenGpu.SeeASettingChange is the goldens' red control.
// - TitleFxDeterminism (AC5): two renders of each case are identical; a seed change changes them.
// - TitleFxFallbackGpu (AC6): the RGBA8 path (TITLE_ART_TEST_NO_FLOAT_TARGETS) agrees with the
//   float path within each definition's class (F0, F1, F2 of §6.5); the mobile Light Glow (D11)
//   agrees with the desktop one within F1. Red control: without the light headroom (D14) Light
//   Glow's RGBA8 render leaves F2.
// - TitleFxScaleGpu: px_scale 0.5 equals px_scale 1 filtered down for the definitions without
//   sub-pixel noise (decision 5); one that ignores px_scale fails (red control).
// - LightGlowGpu (AC7): with a threshold above every channel the ink pixels equal the input;
//   doubling the radius keeps the glow's total light.
// - TitleFxParityGpu (AC4): the seventeen ported effects at defaults on the standard title,
//   through the renderer at 0.25, against Pachislop's references (S80 AC12's block and histogram
//   metric); the parity exceptions are named with the S83 change responsible.

#include "../support/TitleFxRig.h"

#include "titleart/effects/LightEffects.h"

#include <set>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::fxtest;

namespace {

    // S80's ten core effects (TitleArtEffectsGpuTest.cpp covers them)
    const std::set<std::string> CORE_EFFECTS = {"outer_glow", "inner_glow", "drop_shadow", "inner_shadow", "outline", "color_overlay",
                                                "gradient_overlay", "pattern_overlay", "gaussian_blur", "adjust"};

    struct FxCase {
        std::string name;       // the golden's name: "<type>" at defaults, "<type>-<preset>"
        std::string type;
        Json params;
    };

    // the first two presets of each type (editor/resources/titleart/fx-presets/<type>.yaml; the
    // editor's TitleFxPresetsTest checks the files say the same)
    std::map<std::string, std::vector<std::pair<std::string, Json>>> presetCases() {
        Json longShadow = Json{{"angle", 45}, {"length", 260}, {"fade", 1}, {"opacity", 0.55}, {"shading", 0}, {"edge", 0},
                               {"gradient", Json{{"stops", Json::array({Json{{"pos", 0}, {"color", "#000000"}}, Json{{"pos", 1}, {"color", "#000000"}}})}}}};
        return {
            {"light_glow", {{"soft_bloom", {{"radius", 120}, {"falloff", 70}, {"exposure", 0.5}}},
                            {"jackpot_flash", {{"exposure", 3}, {"radius", 400}, {"highlight_shift", 80}, {"core_white", 50}}}}},
            {"contour_lines", {{"light_guide", Json::object()}, {"outside_rings", {{"mode", "outside"}, {"count", 4}, {"spacing", 14}}}}},
            {"neon_tube", {{"pink_neon", Json::object()}, {"cyan_neon", {{"color", "#29f0ff"}}}}},
            {"motion_blur", {{"speed", {{"distance", 120}, {"mode", "trail"}}}}},
            {"lens_blur", {{"bokeh_highlights", {{"radius", 40}, {"brightness", 60}, {"threshold", 200}}}}},
            {"tilt_shift", {{"miniature", {{"blur", 24}, {"width", 120}}}}},
            {"radial_blur", {{"impact_zoom", {{"amount", 0.5}}}, {"spin", {{"type", "spin"}, {"spin", 30}}}}},
            {"extrude", {{"long_shadow", longShadow}, {"block_letters", {{"length", 40}, {"shading", 0.8}}}}},
            {"perspective_shadow", {{"low_sun", {{"length", 400}, {"perspective", 0.6}}}}},
            {"floor_reflection", {{"glossy_floor", {{"opacity", 0.6}, {"blur", 6}}}}},
            {"wave_warp", {{"heat_haze", {{"type", "turbulent"}, {"amplitude", 3}, {"wavelength", 40}}}, {"ripple", {{"type", "radial"}, {"amplitude", 12}}}}},
            {"motion_trail", {{"echo", {{"mode", "echo"}, {"copies", 4}}}, {"smear", Json::object()}}},
            {"chromatic_aberration", {{"rgb_split", {{"type", "directional"}, {"amount", 10}, {"smooth", false}}}}},
            {"glitch", {{"digital", {{"mode", "blocks"}, {"amount", 0.5}}}, {"vhs", {{"mode", "slices"}, {"split", 12}, {"slices", 30}}}}},
            {"halftone", {{"comic_dots", {{"source", "luminance"}, {"dot", "round"}, {"size", 6}, {"color", "#000000"}, {"mode", "multiply"}}},
                          {"ben_day", {{"source", "uniform"}, {"size", 8}, {"dot_scale", 0.6}, {"color", "#ff2a6d"}}}}},
            {"scanlines", {{"scanlines", Json::object()}, {"crt", {{"style", "crt"}, {"mask_strength", 0.6}}}}},
            {"pixelate", {{"retro_8bit", {{"size", 6}, {"levels", 4}, {"dither", "bayer4"}}}, {"led_board", {{"shape", "led"}, {"gap", 0.2}}}}},
            {"grain", {{"film", {{"amount", 0.2}, {"size", 1.6}}}}},
            {"holographic", {{"foil", {{"pattern", "glitter"}}}, {"rainbow_sticker", {{"pattern", "stars"}}}}},
            {"distress", {{"grunge", Json::object()}, {"dry_brush", {{"pattern", "dry_brush"}, {"edge", 30}, {"scale", 40}}}}},
            {"drips", {{"paint", Json::object()}, {"slime", {{"style", "slime"}, {"color_mode", "custom"}, {"color", "#7cff2a"}}}}},
            {"frost", {{"frost", Json::object()}, {"frozen_solid", {{"amount", 1}, {"reach", 60}}}}},
            {"chalk", {{"chalkboard", Json::object()}, {"pastel", {{"color_mode", "layer"}, {"grain", 5}}}}},
            {"brush_stroke", {{"sumi", {{"bristles", 0.7}, {"dry", 0.5}}}, {"marker", {{"bristles", 0.1}, {"roughness", 0.1}}}}},
            {"stitch", {{"patch_border", {{"style", "running"}}}, {"cross_stitch", {{"style", "cross"}}}}},
            {"shatter", {{"glass_break", Json::object()}, {"cracks", {{"cracks_only", true}}}}},
            {"material", {{"bevel", Json::object()},
                          {"chrome", {{"color_mode", "fill"},
                                      {"material", {{"fill", {{"kind", "solid"}, {"color", "#ffffff"}}},
                                                    {"bevel", {{"enabled", true}, {"profile", "smooth"}, {"size_mode", "fit"}, {"fit", 100}, {"depth", 5}, {"soften", 1},
                                                               {"gloss", "linear"}, {"highlight_opacity", 0.7}, {"shadow_opacity", 0.35}}},
                                                    {"reflection", {{"enabled", true}, {"blur", 20}}},
                                                    {"finish", {{"enabled", true}, {"amount", 1},
                                                                {"gradient", standard::standardGradient({{0, "#1b1d22"}, {0.32, "#e9edf2"}, {0.5, "#4a4f58"},
                                                                                                         {0.68, "#f7f9fb"}, {1, "#ffffff"}})}}}}}}}}},
        };
    }

    std::vector<const EffectDef*> catalogueEffects() {
        std::vector<const EffectDef*> out;
        for (const EffectDef* d : TitleEffectRegistry::get().effects()) {
            if (!CORE_EFFECTS.count(d->type) && std::string_view(d->type).rfind("test_", 0) != 0) out.push_back(d);
        }
        return out;
    }

    std::vector<FxCase> catalogueCases() {
        const auto presets = presetCases();
        std::vector<FxCase> cases;
        for (const EffectDef* d : catalogueEffects()) {
            cases.push_back({d->type, d->type, Json::object()});
            auto it = presets.find(d->type);
            if (it == presets.end()) continue;
            for (const auto& [id, params] : it->second) {
                if (params.empty()) continue;   // a preset at the defaults is the defaults' golden
                cases.push_back({std::string(d->type) + "-" + id, d->type, params});
            }
        }
        return cases;
    }

    // golden/titleart/effects/<name>.png with S80 §6.12's limits; JASPER_TEST_UPDATE_GOLDEN=1 writes it (glcore)
    void expectFxGolden(const std::string& name, const Rgba8Image& image) {
        const std::filesystem::path path = goldenPath("titleart/effects/" + name + ".png");
        if (isGoldenUpdateMode()) {
            if (std::string(HeadlessContext::getBackendName()) == "glcore") {
                std::filesystem::create_directories(path.parent_path());
                ASSERT_TRUE(writePng(path.string(), image.rgba, image.width, image.height)) << path.string();
            }
            return;
        }
        Rgba8Image golden;
        ASSERT_TRUE(readImage(path, golden)) << "no golden " << path.string() << " (JASPER_TEST_UPDATE_GOLDEN=1 writes it)";
        const PixelStats stats = compareImages(golden, image);
        const bool ok = stats.max <= 48 && stats.mean <= 0.25 && stats.fractionOver8 <= 0.001;
        if (!ok) {
            const std::filesystem::path actual = std::filesystem::path(getOutputDirectory()) / ("titleart-fx-" + name + ".actual.png");
            std::filesystem::create_directories(actual.parent_path());
            writePng(actual.string(), image.rgba, image.width, image.height);
        }
        EXPECT_TRUE(ok) << name << ": " << stats.text() << (golden.width != image.width || golden.height != image.height ? " (size differs)" : "");
    }

    // the input: the standard title at scale 1 with room around it for what the effects draw past
    // the ink (the canvas, `frame`, is the title's rect: light is cut at its edge as at the canvas's)
    constexpr int BORDER = 48;

    class TitleFxCatalogueGpu : public ::testing::Test {
    protected:
        void SetUp() override {
            JASPER_REQUIRE_GPU();
            rig = std::make_unique<EffectRig>();
            ASSERT_TRUE(rig->warmUp()) << "the title art programs did not compile: " << rig->kit().error();
        }
        void TearDown() override { rig.reset(); }
        std::unique_ptr<EffectRig> rig;
    };

    class TitleFxGoldenGpu : public TitleFxCatalogueGpu {};
    class TitleFxDeterminism : public TitleFxCatalogueGpu {};
    class TitleFxFallbackGpu : public TitleFxCatalogueGpu {};
    class TitleFxScaleGpu : public TitleFxCatalogueGpu {};
    class LightGlowGpu : public TitleFxCatalogueGpu {};

    // the fallback classes of §6.5: mean / max / share of pixels over 8/255
    struct FallbackLimit {
        double mean;
        int max;
        double over;
    };
    FallbackLimit limitOf(FallbackClass c) {
        switch (c) {
            case FallbackClass::F0: return {1.0, 48, 0.005};
            case FallbackClass::F1: return {1.5, 64, 0.02};
            case FallbackClass::F2: return {3.0, 96, 0.05};
        }
        return {1.0, 48, 0.005};
    }

}

TEST_F(TitleFxGoldenGpu, Effects) {
    LinearImage title;
    std::string titleError;
    ASSERT_TRUE(titleInput(title, titleError)) << titleError;
    const Padded input = padded(title, BORDER);
    const std::vector<FxCase> cases = catalogueCases();
    ASSERT_GE(catalogueEffects().size(), 26u) << "S83's effects are registered";
    for (const FxCase& c : cases) {
        SCOPED_TRACE(c.name);
        LinearImage out;
        std::string error;
        ASSERT_TRUE(rig->runEffect(c.type, c.params, input.image, 1.0f, out, error, input.canvas)) << error;
        expectFxGolden(c.name, toRgba8(out));
        // a definition without reach draws nothing past the ink's region
        const EffectDef* def = findEffect(c.type);
        Json params = c.params;
        NormalizeContext normalize;
        normalizeParams(def->params, params, "params", normalize);
        const ParamValues values(params, &def->globalLight, 120.0, 30.0);
        const bool canvas = def->fillsCanvas && def->fillsCanvas(values);
        if (!canvas && (!def->reach || def->reach(values) <= 0.0f)) {
            float leak = 0.0f;
            for (int y = 0; y < out.height; y++) {
                for (int x = 0; x < out.width; x++) {
                    const bool inside = x >= input.canvas.x && y >= input.canvas.y && x < input.canvas.x + input.canvas.width &&
                                        y < input.canvas.y + input.canvas.height;
                    if (!inside) leak = std::max(leak, out.at(x, y)[3]);
                }
            }
            EXPECT_LT(leak, 1.0f / 255.0f) << "an effect without reach drew past the ink";
        }
    }
}

// S83 §6.6.28: every new pattern kind through Pattern Fill
TEST_F(TitleFxGoldenGpu, PatternKinds) {
    for (const char* kind : {"wood", "marble", "granite", "concrete", "brushed_metal", "diamond_plate", "carbon_fiber", "rust", "leather", "brick",
                             "camo", "circuit"}) {
        SCOPED_TRACE(kind);
        LinearImage out;
        std::string error;
        ASSERT_TRUE(rig->runGenerator("pattern_fill", Json{{"kind", kind}, {"scale", 60}, {"color", "#5a3214"}, {"color2", "#c8a070"}, {"background", "#8a5a30"}},
                                      400, 220, 1.0f, out, error))
            << error;
        expectFxGolden(std::string("pattern_fill-") + kind, toRgba8(out));
    }
}

// the goldens' red control: a changed setting moves the image well past the golden limits
TEST_F(TitleFxGoldenGpu, SeeASettingChange) {
    LinearImage title;
    std::string titleError;
    ASSERT_TRUE(titleInput(title, titleError)) << titleError;
    const Padded input = padded(title, BORDER);
    LinearImage base, moved;
    std::string error;
    ASSERT_TRUE(rig->runEffect("extrude", Json::object(), input.image, 1.0f, base, error, input.canvas)) << error;
    ASSERT_TRUE(rig->runEffect("extrude", Json{{"length", 26}}, input.image, 1.0f, moved, error, input.canvas)) << error;
    const PixelStats stats = compareImages(toRgba8(base), toRgba8(moved));
    EXPECT_TRUE(stats.max > 48 || stats.mean > 0.25 || stats.fractionOver8 > 0.001) << "4 px more depth passes the golden: " << stats.text();
}

// R15, D7: a render is a function of the document; a seed changes it
TEST_F(TitleFxDeterminism, Effects) {
    LinearImage title;
    std::string titleError;
    ASSERT_TRUE(titleInput(title, titleError)) << titleError;
    const LinearImage input = downsample(title, 2);
    for (const FxCase& c : catalogueCases()) {
        SCOPED_TRACE(c.name);
        LinearImage a, b;
        std::string error;
        ASSERT_TRUE(rig->runEffect(c.type, c.params, input, 0.5f, a, error)) << error;
        ASSERT_TRUE(rig->runEffect(c.type, c.params, input, 0.5f, b, error)) << error;
        EXPECT_TRUE(a.rgba == b.rgba) << "two renders differ";
    }
    // a seed moves every seeded definition's random elements (Holographic's glitter, Grain, Glitch,
    // Distress, Chalk, Frost, Shatter's layout, Drips' columns)
    for (const char* type : {"grain", "glitch", "distress", "chalk", "frost", "shatter", "drips", "holographic", "brush_stroke"}) {
        SCOPED_TRACE(type);
        LinearImage a, b;
        std::string error;
        ASSERT_TRUE(rig->runEffect(type, Json{{"seed", 1}}, input, 0.5f, a, error)) << error;
        ASSERT_TRUE(rig->runEffect(type, Json{{"seed", 7}}, input, 0.5f, b, error)) << error;
        EXPECT_FALSE(a.rgba == b.rgba) << "the seed changed nothing";
    }
}

// §6.5, D14, D17: the RGBA8 path keeps every pass and agrees within the definition's class
TEST_F(TitleFxFallbackGpu, AgreesWithinItsClass) {
    EffectRig fallback(TITLE_ART_TEST_NO_FLOAT_TARGETS | TITLE_ART_TEST_NO_R16F | TITLE_ART_TEST_NO_RGBA32F);
    ASSERT_FALSE(fallback.kit().caps().floatPath);
    ASSERT_TRUE(fallback.warmUp()) << fallback.kit().error();
    LinearImage title;
    std::string titleError;
    ASSERT_TRUE(titleInput(title, titleError)) << titleError;
    const Padded input = padded(title, BORDER);
    const std::set<std::string> hardEdged = {"chalk", "frost", "brush_stroke", "stitch"};
    for (const EffectDef* def : catalogueEffects()) {
        SCOPED_TRACE(def->type);
        LinearImage f, e;
        std::string error;
        const uint64_t before = rig->kit().passes(), beforeFallback = fallback.kit().passes();
        ASSERT_TRUE(rig->runEffect(def->type, Json::object(), input.image, 1.0f, f, error, input.canvas)) << error;
        ASSERT_TRUE(fallback.runEffect(def->type, Json::object(), input.image, 1.0f, e, error, input.canvas)) << error;
        // the same passes: the fallback rig's readback adds one copy (SRGB8A8 decoded to half floats)
        EXPECT_EQ(rig->kit().passes() - before + 1, fallback.kit().passes() - beforeFallback) << "the fallback runs the same passes";
        const PixelStats stats = compareImages(toRgba8(f), toRgba8(e));
        const FallbackLimit l = limitOf(def->fallback);
        EXPECT_LE(stats.mean, l.mean) << fallbackClassName(def->fallback) << ": " << stats.text();
        // hard-edged procedural cuts (chalk grain, frost crystals, brush gaps, stitch ends) flip
        // single pixels where the RGBA8 distance's 1/64 px steps cross their threshold: their
        // mean and share stay within the class, a lone pixel may differ fully (Deviations)
        if (!hardEdged.count(def->type)) { EXPECT_LE(stats.max, l.max) << fallbackClassName(def->fallback) << ": " << stats.text(); }
        EXPECT_LE(stats.fractionOver8, l.over) << fallbackClassName(def->fallback) << ": " << stats.text();
    }
}

// D11: the mobile tier's cascaded Light Glow agrees with the desktop one within F1
TEST_F(TitleFxFallbackGpu, MobileLightGlowAgrees) {
    LinearImage title;
    std::string titleError;
    ASSERT_TRUE(titleInput(title, titleError)) << titleError;
    const Padded input = padded(title, BORDER);
    LinearImage desktop, mobile;
    std::string error;
    ASSERT_TRUE(rig->runEffect("light_glow", Json{{"chroma", 0}}, input.image, 1.0f, desktop, error, input.canvas)) << error;
    rig->tier = FxTier::Mobile;
    ASSERT_TRUE(rig->runEffect("light_glow", Json{{"chroma", 0}}, input.image, 1.0f, mobile, error, input.canvas)) << error;
    rig->tier = FxTier::Desktop;
    const PixelStats stats = compareImages(toRgba8(desktop), toRgba8(mobile));
    const FallbackLimit l = limitOf(FallbackClass::F1);
    EXPECT_LE(stats.mean, l.mean) << stats.text();
    EXPECT_LE(stats.fractionOver8, l.over) << stats.text();
}

// D14's red control: without the headroom the RGBA8 Light Glow clips its light at 1 and leaves F2
TEST_F(TitleFxFallbackGpu, HeadroomMatters) {
    EffectRig fallback(TITLE_ART_TEST_NO_FLOAT_TARGETS | TITLE_ART_TEST_NO_R16F | TITLE_ART_TEST_NO_RGBA32F);
    ASSERT_TRUE(fallback.warmUp()) << fallback.kit().error();
    LinearImage title;
    std::string titleError;
    ASSERT_TRUE(titleInput(title, titleError)) << titleError;
    const Padded input = padded(title, BORDER);
    const Json bright = Json{{"exposure", 3}, {"radius", 120}};
    LinearImage f, e;
    std::string error;
    ASSERT_TRUE(rig->runEffect("light_glow", bright, input.image, 1.0f, f, error, input.canvas)) << error;
    tfx::setLightHeadroomForTesting(1.0f);
    const bool ran = fallback.runEffect("light_glow", bright, input.image, 1.0f, e, error, input.canvas);
    tfx::setLightHeadroomForTesting(0.0f);
    ASSERT_TRUE(ran) << error;
    const PixelStats stats = compareImages(toRgba8(f), toRgba8(e));
    const FallbackLimit l = limitOf(FallbackClass::F2);
    EXPECT_TRUE(stats.mean > l.mean || stats.max > l.max || stats.fractionOver8 > l.over) << "no headroom passed F2: " << stats.text();
}

// decision 5: Px params scale with px_scale, so a render at 0.5 is the render at 1 filtered down
// (the analytic fixture, drawn exactly at both scales). Definitions with sub-pixel noise or a
// canvas-locked screen (grain, halftone dots, scanlines, pixel cells, glitter, wear) are left out:
// their pattern is finer than a pixel at half scale by design
TEST_F(TitleFxScaleGpu, PxParamsScale) {
    const std::set<std::string> noisy = {"grain", "halftone", "scanlines", "pixelate", "holographic", "distress", "chalk", "frost",
                                         "lens_blur", "glitch", "stitch", "brush_stroke", "shatter", "drips"};
    const Padded full = padded(fixture(1.0f), 32);
    const Padded half = padded(fixture(0.5f), 16);
    auto check = [&](const std::string& type, float halfContextScale, PixelStats& stats) {
        LinearImage a, b;
        std::string error;
        if (!rig->runEffect(type, Json::object(), full.image, 1.0f, a, error, full.canvas)) return error;
        if (!rig->runEffect(type, Json::object(), half.image, 0.5f, b, error, half.canvas, halfContextScale)) return error;
        stats = compareImages(toRgba8(downsample(a, 2)), toRgba8(b));
        return std::string();
    };
    for (const EffectDef* def : catalogueEffects()) {
        if (noisy.count(def->type)) continue;
        SCOPED_TRACE(def->type);
        PixelStats stats;
        const std::string error = check(def->type, -1.0f, stats);
        ASSERT_TRUE(error.empty()) << error;
        EXPECT_LE(stats.mean, 2.0) << stats.text();
        EXPECT_LE(stats.fractionOver8, 0.05) << stats.text();
    }
    // red control: Extrude rendered at half scale as if px_scale were 1 (its depth in output px)
    PixelStats broken;
    ASSERT_TRUE(check("extrude", 1.0f, broken).empty());
    EXPECT_TRUE(broken.mean > 2.0 || broken.fractionOver8 > 0.05) << "ignoring px_scale passed: " << broken.text();
}

// R19, AC7: where no channel exceeds 1 and nothing emits, the response is the identity
TEST_F(LightGlowGpu, Identity) {
    const Padded input = padded(fixture(1.0f), 16);
    LinearImage out;
    std::string error;
    ASSERT_TRUE(rig->runEffect("light_glow", Json{{"threshold", 2}, {"on_face", 0}}, input.image, 1.0f, out, error, input.canvas)) << error;
    float worst = 0.0f;
    for (int y = 0; y < out.height; y++) {
        for (int x = 0; x < out.width; x++) {
            const float* o = out.at(x, y);
            const float* i = input.image.at(x, y);
            if (i[3] <= 0.0f) continue;
            for (int c = 0; c < 3; c++) worst = std::max(worst, std::fabs(o[c] - i[c]));
        }
    }
    // the targets are half floats: the identity holds to their precision
    EXPECT_LE(worst, 1e-3f);
}

// R19, AC7: Radius reshapes the halo without changing how much light there is
TEST_F(LightGlowGpu, RadiusKeepsEnergy) {
    LinearImage dot;
    dot.width = 512;
    dot.height = 512;
    dot.rgba.assign((size_t)dot.width * dot.height * 4, 0.0f);
    for (int y = 252; y < 260; y++) {
        for (int x = 252; x < 260; x++) {
            float* p = dot.at(x, y);
            p[0] = p[1] = p[2] = p[3] = 1.0f;
        }
    }
    auto glowEnergy = [&](double radius) {
        LinearImage out;
        std::string error;
        // all light scattered, none of the layer's own: the output minus the input is the glow
        EXPECT_TRUE(rig->runEffect("light_glow", Json{{"radius", radius}, {"exposure", 0}, {"highlight_shift", 0}, {"on_face", 1}}, dot, 1.0f, out, error))
            << error;
        double sum = 0.0;
        for (size_t i = 0; i < out.rgba.size(); i += 4) sum += out.rgba[i + 1] - dot.rgba[i + 1];
        return sum;
    };
    const double a = glowEnergy(40.0), b = glowEnergy(80.0);
    ASSERT_GT(a, 0.0);
    EXPECT_NEAR(b / a, 1.0, 0.01);
}

// AC4: Pachislop parity of the ported effects. The exceptions (at most ten, §7) are the effects
// whose S83 design departs from Pachislop's on purpose; each still renders and is reported
TEST_F(TitleFxCatalogueGpu, PachislopParity) {
    const std::map<std::string, const char*> references = {
        {"light_glow", "deepGlow"}, {"contour_lines", "contourLines"}, {"motion_blur", "motionBlur"}, {"lens_blur", "lensBlur"},
        {"tilt_shift", "tiltShift"}, {"radial_blur", "radialBlur"}, {"extrude", "extrude2d"}, {"motion_trail", "motionTrail"},
        {"halftone", "halftone"}, {"perspective_shadow", "perspectiveShadow"}, {"floor_reflection", "floorReflection"}, {"wave_warp", "wave"},
        {"chromatic_aberration", "chromaticAberration"}, {"grain", "grain"}, {"glitch", "glitch"}, {"material", "material"}, {"shatter", "shatter"}};
    // the departures (docs/specs/S83-title-effects-generators.md Deviations): Shatter's pieces are
    // a function of their index on the GPU (another layout); Extrude's trimmed image is one pixel
    // deeper at 0.25, which shifts the centring the block metric is sensitive to
    const std::set<std::string> exceptions = {"shatter", "extrude"};
    int passed = 0;
    for (const auto& [type, reference] : references) {
        SCOPED_TRACE(type);
        Rgba8Image out;
        std::string error;
        ASSERT_TRUE(renderDocument(standard::standardTitleDocument({standard::standardEffect("00000201", type.c_str())}), 0.25f, TitleOutputFormat::Srgb8,
                                   nullptr, &out, error))
            << error;
        Rgba8Image ref;
        ASSERT_TRUE(readImage(goldenPath(std::string("titleart/pachislop/effect/") + reference + ".png"), ref)) << reference;
        const Rgba8Image ours = centred(trim(out, 8), ref.width, ref.height);
        const BlockStats stats = compareBlocks(ref, ours);
        const bool ok = stats.fractionOver <= 0.03 && stats.histogramDistance <= 6.0;
        std::printf("[parity] %s: %s%s\n", reference, stats.text().c_str(), ok ? "" : (exceptions.count(type) ? " (a listed exception)" : " FAIL"));
        if (!ok) {
            const std::filesystem::path actual = std::filesystem::path(getOutputDirectory()) / (std::string("titleart-fx-parity-") + reference + ".png");
            std::filesystem::create_directories(actual.parent_path());
            writePng(actual.string(), ours.rgba, ours.width, ours.height);
        }
        if (exceptions.count(type)) continue;
        EXPECT_TRUE(ok) << reference << ": " << stats.text();
        if (ok) passed++;
    }
    EXPECT_LE(exceptions.size(), 10u);
    EXPECT_GE(passed, (int)(references.size() - exceptions.size()));
}
