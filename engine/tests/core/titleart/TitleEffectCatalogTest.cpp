// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The S80 effect and generator registry's table and what is derived from it (docs/specs/
// S80-title-art-core.md R30, R31, §6.5, AC24, AC25): every built-in definition passes the
// definition checks with the catalogue's ids, defaults, ranges and flags; the MCP schema
// fragments, list_title_library rows, field paths and the docs page come from the table; the
// golden list and the committed references agree.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/TitleEffectCatalog.h"
#include "titleart/TitleEffectRegistry.h"
#include "titleart/effects/TitleAdjust.h"
#include "titleart/effects/TitleFx.h"

#include "ImageCompare.h"
#include "TestPaths.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using namespace jasperengine;
using namespace jasperengine::titleart;

namespace {

    const std::vector<std::string> CORE_EFFECTS = {"outer_glow", "inner_glow", "drop_shadow", "inner_shadow", "outline",
                                                   "color_overlay", "gradient_overlay", "adjust", "pattern_overlay", "gaussian_blur"};

    std::filesystem::path repoRoot() {
        return std::filesystem::path(test::testPaths().sourceDir) / ".." / ".." / "..";
    }

    std::string readText(const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream s;
        s << in.rdbuf();
        return s.str();
    }

    ParamValues defaultsOf(const std::vector<ParamDef>& params, Json& storage) {
        storage = defaultParams(params);
        return ParamValues(storage);
    }

    // a definition only the tests register
    EffectDef testDef(const char* type) {
        EffectDef d;
        d.type = type;
        d.label = "Test Sparkle";
        d.category = "Glow";
        d.description = "A test-only definition";
        ParamDef size;
        size.key = "size";
        size.label = "Size";
        size.unit = ParamUnit::Px;
        size.min = 1;
        size.max = 50;
        size.defaultValue = 10;
        size.description = "How big";
        d.params = {size};
        d.aliases = {"sparkle"};
        d.apply = [](EffectContext&, const TargetRef&, TargetRef&, const ParamValues&) {};
        return d;
    }

}

TEST(TitleEffectCatalogTest, EveryDefinitionIsComplete) {
    const TitleEffectRegistry& registry = TitleEffectRegistry::get();
    const std::vector<std::string> problems = checkTitleEffectRegistry(registry);
    for (const std::string& p : problems) ADD_FAILURE() << p;
    std::set<std::string> effects, generators;
    for (const EffectDef* d : registry.effects()) effects.insert(d->type);
    for (const GeneratorDef* d : registry.generators()) generators.insert(d->type);
    for (const std::string& type : CORE_EFFECTS) EXPECT_TRUE(effects.count(type)) << type << " is not registered";
    EXPECT_TRUE(generators.count("fill"));
    EXPECT_TRUE(generators.count("pattern_fill"));
    EXPECT_EQ(effects.size(), CORE_EFFECTS.size()) << "S80 registers its ten core effects (S83 adds the rest)";
}

TEST(TitleEffectCatalogTest, ChecksCatchBrokenDefinitions) {
    // red controls: each mistake the checks guard is reported
    EffectDef outside = testDef("test_outside");
    outside.params[0].defaultValue = 90;
    const std::vector<std::string> a = checkTitleEffectDef(outside);
    ASSERT_FALSE(a.empty());
    EXPECT_NE(a.front().find("outside its range"), std::string::npos) << a.front();

    EffectDef badWhen = testDef("test_when");
    badWhen.params[0].when.param = "mode";
    badWhen.params[0].when.values = {"add"};
    EXPECT_FALSE(checkTitleEffectDef(badWhen).empty());

    EffectDef camel = testDef("testCamel");
    EXPECT_FALSE(checkTitleEffectDef(camel).empty()) << "a type that is not snake_case";

    EffectDef noDoc = testDef("test_doc");
    noDoc.params[0].description = "";
    EXPECT_FALSE(checkTitleEffectDef(noDoc).empty());

    EffectDef noPass = testDef("test_pass");
    noPass.apply = nullptr;
    EXPECT_FALSE(checkTitleEffectDef(noPass).empty());

    EffectDef enumDefault = testDef("test_enum");
    ParamDef mode;
    mode.key = "mode";
    mode.label = "Mode";
    mode.kind = ParamKind::Enum;
    mode.options = {{"behind", "Behind"}, {"add", "Add"}};
    mode.defaultValue = "sideways";
    mode.description = "Where";
    mode.interp = ParamInterp::Stepped;
    mode.anim = FxAnim::Step;
    enumDefault.params.push_back(mode);
    EXPECT_FALSE(checkTitleEffectDef(enumDefault).empty()) << "an enum default among no option";

    EXPECT_TRUE(checkTitleEffectDef(testDef("test_good")).empty());
}

// §6.5's catalogue: Pachislop's params one to one, snake_case, same defaults, ranges and units
TEST(TitleEffectCatalogTest, CatalogueMatchesTheSpec) {
    Json storage;
    const EffectDef* glow = findEffect("outer_glow");
    ASSERT_TRUE(glow);
    ParamValues p = defaultsOf(glow->params, storage);
    EXPECT_EQ(p.string("fill"), "solid");
    EXPECT_EQ(p.string("color"), "#ffb000");
    EXPECT_EQ(p.number("size"), 40);
    EXPECT_EQ(findParam(glow->params, "size")->min, 1);
    EXPECT_EQ(findParam(glow->params, "size")->max, 400);
    EXPECT_EQ(findParam(glow->params, "size")->unit, ParamUnit::Px);
    EXPECT_DOUBLE_EQ(p.number("intensity"), 1.6);
    EXPECT_DOUBLE_EQ(p.number("spread"), 0.15);
    EXPECT_EQ(p.string("mode"), "behind");
    EXPECT_DOUBLE_EQ(p.number("on_face"), 0.15);
    EXPECT_EQ(p.string("technique"), "softer");
    EXPECT_EQ(glow->reach(p), 40.0f);
    EXPECT_FALSE(glow->addsLight(p));
    storage["mode"] = "add";
    EXPECT_TRUE(glow->addsLight(ParamValues(storage))) << "adds light when add";
    EXPECT_EQ(glow->costClass, CostClass::C1);
    EXPECT_EQ(glow->fallback, FallbackClass::F1);

    const EffectDef* inner = findEffect("inner_glow");
    p = defaultsOf(inner->params, storage);
    EXPECT_EQ(p.string("color"), "#fff4a0");
    EXPECT_EQ(p.number("size"), 18);
    EXPECT_EQ(p.string("mode"), "screen");
    EXPECT_EQ(p.string("source"), "edge");
    EXPECT_FLOAT_EQ(inner->gather(p), 27.0f) << "gather 1.5 x size";
    EXPECT_EQ(inner->reach, nullptr) << "it stays inside the ink";

    const EffectDef* drop = findEffect("drop_shadow");
    p = defaultsOf(drop->params, storage);
    EXPECT_EQ(p.number("angle"), 60);
    EXPECT_EQ(p.flag("use_global_light"), true);
    EXPECT_EQ(p.number("distance"), 10);
    EXPECT_EQ(p.number("size"), 14);
    EXPECT_DOUBLE_EQ(p.number("spread"), 0.1);
    EXPECT_DOUBLE_EQ(p.number("opacity"), 0.75);
    EXPECT_EQ(drop->reach(p), 24.0f) << "reach distance + size";
    // the default global light (120, upper left) casts it down-right: the default angle
    const ParamValues lit(storage, &drop->globalLight, 120.0, 30.0);
    EXPECT_NEAR(lit.number("angle"), 60.0, 1e-9);
    const ParamValues fromRight(storage, &drop->globalLight, 180.0, 30.0);
    EXPECT_NEAR(fromRight.number("angle"), 0.0, 1e-9) << "light from the left: the shadow falls right";

    const EffectDef* innerShadow = findEffect("inner_shadow");
    p = defaultsOf(innerShadow->params, storage);
    EXPECT_EQ(p.number("distance"), 6);
    EXPECT_EQ(p.number("size"), 8);
    EXPECT_DOUBLE_EQ(p.number("opacity"), 0.6);
    EXPECT_TRUE(innerShadow->globalLight.angleKey);

    const EffectDef* outline = findEffect("outline");
    p = defaultsOf(outline->params, storage);
    EXPECT_DOUBLE_EQ(p.number("width"), 1.5);
    EXPECT_EQ(p.string("color"), "#0a0604");
    EXPECT_DOUBLE_EQ(p.number("threshold"), 0.5);
    EXPECT_EQ(p.string("follow"), "layer");
    EXPECT_TRUE(outline->outline);
    EXPECT_FLOAT_EQ(outline->reach(p), 1.5f);

    const EffectDef* color = findEffect("color_overlay");
    p = defaultsOf(color->params, storage);
    EXPECT_EQ(p.string("color"), "#ff0000");
    EXPECT_EQ(findParam(color->params, "mode")->options.size(), 18u) << "the 18 blend modes";
    EXPECT_EQ(color->costClass, CostClass::C0);
    EXPECT_EQ(color->fallback, FallbackClass::F0);

    const EffectDef* gradient = findEffect("gradient_overlay");
    p = defaultsOf(gradient->params, storage);
    EXPECT_EQ(p.string("style"), "linear");
    EXPECT_EQ(p.number("angle"), 90);
    EXPECT_EQ(p.number("length"), 200);
    EXPECT_EQ(p.gradient("gradient").stops.size(), 6u) << "the gold bands";
    EXPECT_EQ(findParam(gradient->params, "mode")->options.size(), 20u) << "18 modes, light and shade";

    const EffectDef* pattern = findEffect("pattern_overlay");
    p = defaultsOf(pattern->params, storage);
    EXPECT_EQ(p.string("pattern"), "stripes");
    EXPECT_EQ(p.number("scale"), 40);
    EXPECT_EQ(p.string("align"), "layer");

    const EffectDef* blur = findEffect("gaussian_blur");
    p = defaultsOf(blur->params, storage);
    EXPECT_EQ(p.number("radius"), 8);
    EXPECT_TRUE(p.flag("repeat_edges"));
    EXPECT_FLOAT_EQ(blur->reach(p), 24.0f) << "3 x radius x max scale";

    const EffectDef* adjust = findEffect("adjust");
    p = defaultsOf(adjust->params, storage);
    EXPECT_EQ(p.raw("adjustment")["type"], "hue_saturation");
    EXPECT_EQ(p.raw("adjustment")["saturation"], 25);
    EXPECT_EQ(adjust->gather(p), 0.0f);

    const GeneratorDef* fill = findGenerator("fill");
    p = defaultsOf(fill->params, storage);
    EXPECT_EQ(p.string("kind"), "solid");
    EXPECT_EQ(p.string("color"), "#202020");
    const GeneratorDef* patternFill = findGenerator("pattern_fill");
    p = defaultsOf(patternFill->params, storage);
    EXPECT_EQ(p.string("kind"), "zebra");
    EXPECT_EQ(p.number("scale"), 140);

    // S80's twelve definitions at defaults: C0 / F0 for the overlays, adjust and the generators,
    // C1 / F1 for the glows, shadows, outline and blur; before S83 lands no live variant
    for (const EffectDef* d : TitleEffectRegistry::get().effects()) {
        const bool cheap = std::string(d->type) == "color_overlay" || std::string(d->type) == "gradient_overlay" ||
                           std::string(d->type) == "pattern_overlay" || std::string(d->type) == "adjust";
        EXPECT_EQ(d->costClass, cheap ? CostClass::C0 : CostClass::C1) << d->type;
        EXPECT_EQ(d->fallback, cheap ? FallbackClass::F0 : FallbackClass::F1) << d->type;
        EXPECT_EQ(d->live.kind, LiveKind::None) << d->type;
        for (const ParamDef& param : d->params) {
            const bool stepped = param.kind == ParamKind::Enum || param.kind == ParamKind::Bool || param.kind == ParamKind::Integer ||
                                 param.kind == ParamKind::Asset;
            EXPECT_EQ(param.anim, stepped ? FxAnim::Step : FxAnim::Rerender) << d->type << "." << param.key;
        }
    }
}

TEST(TitleEffectCatalogTest, ParamsAreFieldPathsOfEveryDefinition) {
    for (const EffectDef* d : TitleEffectRegistry::get().effects()) {
        TitleArtDoc doc = makeNewTitleArt();
        doc.layers()[0]["effects"] = Json::array({Json{{"id", "0000000e"}, {"type", d->type}, {"params", Json::object()}}});
        normalizeTitleArt(doc);
        const std::vector<std::string> paths = titleEffectFieldPaths(*d);
        ASSERT_EQ(paths.size(), d->params.size());
        for (size_t i = 0; i < paths.size(); i++) {
            const ParamDef& p = d->params[i];
            EXPECT_EQ(paths[i], std::string("fx:") + d->type + "." + p.key);
            // a param's own default set through its field path is taken as it is
            FieldSetResult set = setTitleArtField(doc.layers()[0], "layer", paths[i], p.defaultValue);
            EXPECT_TRUE(set.ok) << paths[i] << ": " << set.error;
            EXPECT_FALSE(set.clamped) << paths[i];
        }
        FieldSetResult typo = setTitleArtField(doc.layers()[0], "layer", std::string("fx:") + d->type + ".opacityy", 1);
        EXPECT_FALSE(typo.ok);
    }
    // a number beyond its range is clamped and echoed; an enum value it does not take is refused
    TitleArtDoc doc = makeNewTitleArt();
    doc.layers()[0]["effects"] = Json::array({Json{{"id", "0000000e"}, {"type", "outer_glow"}, {"params", Json::object()}}});
    normalizeTitleArt(doc);
    FieldSetResult clamped = setTitleArtField(doc.layers()[0], "layer", "fx:outer_glow.size", 1000);
    ASSERT_TRUE(clamped.ok);
    EXPECT_TRUE(clamped.clamped);
    EXPECT_EQ(clamped.to, 400);
    FieldSetResult badMode = setTitleArtField(doc.layers()[0], "layer", "fx:outer_glow.mode", "sideways");
    EXPECT_FALSE(badMode.ok);
}

TEST(TitleEffectCatalogTest, SchemaFragmentsAndRows) {
    for (const EffectDef* d : TitleEffectRegistry::get().effects()) {
        const Json schema = titleParamsSchema(d->params);
        EXPECT_EQ(schema["type"], "object");
        EXPECT_EQ(schema["additionalProperties"], false) << "closed objects";
        for (const ParamDef& p : d->params) {
            ASSERT_TRUE(schema["properties"].contains(p.key)) << d->type << "." << p.key;
            const Json& s = schema["properties"][p.key];
            EXPECT_TRUE(s.contains("type"));
            EXPECT_EQ(s["description"], p.description);
            if (p.kind == ParamKind::Number) {
                EXPECT_EQ(s["minimum"], p.min);
                EXPECT_EQ(s["maximum"], p.max);
            }
            if (p.kind == ParamKind::Enum) {
                EXPECT_EQ(s["enum"].size(), p.options.size());
            }
        }
        const Json row = titleEffectRow(*d);
        EXPECT_EQ(row["id"], d->type);
        EXPECT_EQ(row["name"], d->label);
        EXPECT_EQ(row["cost"], costClassName(d->costClass));
        const Json detail = titleEffectDetail(*d);
        EXPECT_EQ(detail["params"].size(), d->params.size());
        EXPECT_EQ(detail["add"]["op"], "add_effect");
        EXPECT_EQ(detail["add"]["type"], d->type);
    }
    EXPECT_EQ(titleParamLine(*findParam(findEffect("outer_glow")->params, "size")), "size px 40 [1..400]: How far the glow reaches past the edge");
    for (const GeneratorDef* d : TitleEffectRegistry::get().generators()) {
        const Json row = titleGeneratorRow(*d);
        EXPECT_EQ(row["fills_canvas"], true) << d->type;
        EXPECT_EQ(titleGeneratorDetail(*d)["add"]["generator"], d->type);
        EXPECT_EQ(titleGeneratorFieldPaths(*d).front(), std::string("gen.") + d->params.front().key);
    }
}

TEST(TitleEffectCatalogTest, DocsPageIsUpToDate) {
    const std::string generated = titleEffectDocsMarkdown(TitleEffectRegistry::get());
    const std::filesystem::path path = repoRoot() / TITLE_EFFECT_DOCS_PATH;
    if (test::isGoldenUpdateMode()) std::ofstream(path, std::ios::binary) << generated;
    ASSERT_TRUE(std::filesystem::exists(path)) << path.string();
    EXPECT_EQ(readText(path), generated) << TITLE_EFFECT_DOCS_PATH
                                         << " is stale: rerun with JASPER_TEST_UPDATE_GOLDEN=1 (UNIT_TEST_ARGS=--gtest_filter=TitleEffectCatalogTest.*)";
    for (const EffectDef* d : TitleEffectRegistry::get().effects()) {
        EXPECT_NE(generated.find(std::string("(`") + d->type + "`)"), std::string::npos) << d->type;
    }
}

// AC25: a definition added to the table appears in every derived surface, and leaves with it
TEST(TitleEffectCatalogTest, DrivesEverySurface) {
    TitleEffectRegistry& registry = TitleEffectRegistry::get();
    ASSERT_TRUE(registry.add(testDef("test_sparkle")));
    const std::vector<std::string> cases = titleGoldenCases(registry);
    EXPECT_NE(std::find(cases.begin(), cases.end(), "effect/test_sparkle"), cases.end());
    EXPECT_NE(titleEffectDocsMarkdown(registry).find("Test Sparkle (`test_sparkle`)"), std::string::npos);
    EXPECT_TRUE(checkTitleEffectRegistry(registry).empty());
    // the document reads, fills and clamps its params; ops address them
    TitleArtDoc doc = makeNewTitleArt();
    doc.layers()[0]["effects"] = Json::array({Json{{"id", "0000000e"}, {"type", "test_sparkle"}, {"params", Json{{"size", 99}}}}});
    normalizeTitleArt(doc);
    EXPECT_EQ(doc.layers()[0]["effects"][0]["params"]["size"], 50);
    EXPECT_TRUE(setTitleArtField(doc.layers()[0], "layer", "fx:test_sparkle.size", 20).ok);
    EXPECT_GT(titleArtBakeVersion(doc), (uint32_t)TITLE_ART_BAKE_VERSION_BASE);
    ASSERT_TRUE(registry.removeEffect("test_sparkle"));
    EXPECT_EQ(titleEffectDocsMarkdown(registry).find("test_sparkle"), std::string::npos);
    const std::vector<std::string> after = titleGoldenCases(registry);
    EXPECT_EQ(std::find(after.begin(), after.end(), "effect/test_sparkle"), after.end());
}

// AC24's list check: every case has its reference and every reference a case
TEST(TitleEffectCatalogTest, GoldenListMatchesTheReferences) {
    const std::filesystem::path dir = std::filesystem::path(test::testPaths().goldenDir) / "titleart" / "effects";
    ASSERT_TRUE(std::filesystem::is_directory(dir)) << dir.string();
    std::set<std::string> cases;
    for (const std::string& c : titleGoldenCases(TitleEffectRegistry::get())) cases.insert(c.substr(c.find('/') + 1));
    std::set<std::string> defaults;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".png") continue;
        const std::string stem = entry.path().stem().string();
        // "<type>.png" at defaults, "<type>-<variant>.png" with settings, "<type>.rgba8.png" the fallback
        const std::string type = stem.substr(0, std::min(stem.find('-'), stem.find('.')));
        EXPECT_TRUE(cases.count(type)) << entry.path().filename().string() << " has no definition";
        if (stem == type) defaults.insert(type);
    }
    for (const std::string& c : cases) EXPECT_TRUE(defaults.count(c)) << c << " has no reference at defaults";
}

TEST(TitleEffectCatalogTest, FloodPassesAreBoundedByTheReach) {
    // R43: at most ceil(log2(R + 2)) + 1 flood passes for a reach of R px
    for (float reach : {1.5f, 4.0f, 10.0f, 40.0f, 100.0f, 400.0f}) {
        EXPECT_LE(fx::floodPasses(reach, 4096, 4096), (int)std::ceil(std::log2(reach + 2.0f)) + 1) << reach;
    }
    EXPECT_EQ(fx::floodPasses(0.0f, 1024, 512), 11) << "unbounded: from the larger side";
    EXPECT_EQ(fx::floodPasses(4000.0f, 64, 64), 7) << "never more than the unbounded field";
}

TEST(TitleEffectCatalogTest, GradientRampsAsPachislopBakesThem) {
    TitleGradient g = decodeGradient(Json{{"stops", Json::array({Json{{"pos", 0}, {"color", "#000000"}, {"alpha", 1}, {"mid", 0.25}},
                                                                 Json{{"pos", 1}, {"color", "#ffffff"}, {"alpha", 0}, {"mid", 0.5}}})},
                                          {"interp", "rgb"}});
    float c[4];
    fx::sampleGradient(g, 0.25, c);
    EXPECT_NEAR(c[0], 0.5f, 1e-5f) << "the midpoint moves the half-way colour";
    EXPECT_NEAR(c[3], 0.5f, 1e-5f) << "opacity blends with the same midpoint";
    const TitleLutData lut = fx::gradientLut(g);
    ASSERT_EQ(lut.bytes.size(), 1024u);
    EXPECT_EQ(lut.bytes[0], 0);
    EXPECT_EQ(lut.bytes[255 * 4], 255);
    EXPECT_EQ(lut.bytes[255 * 4 + 3], 0);
    // vivid: blue to white stays blue-ish instead of greying (the OKLCH hue is kept)
    TitleGradient vivid = decodeGradient(Json{{"stops", Json::array({Json{{"pos", 0}, {"color", "#0000ff"}}, Json{{"pos", 1}, {"color", "#ffffff"}}})},
                                              {"interp", "vivid"}});
    fx::sampleGradient(vivid, 0.5, c);
    EXPECT_GT(c[2], c[0] + 0.05f);
}

TEST(TitleEffectCatalogTest, AdjustmentToneCurves) {
    std::vector<float> rgb;
    Json levels = Json{{"type", "levels"}, {"channels", Json::object()}};
    NormalizeContext context;
    normalizeTitleArtObject(levels, "adjustment", "", context);
    EXPECT_TRUE(titleAdjustmentIsIdentity(levels));
    levels["channels"]["rgb"]["in_white"] = 128;
    EXPECT_FALSE(titleAdjustmentIsIdentity(levels));
    ASSERT_TRUE(bakeTitleToneCurve(levels, rgb));
    EXPECT_NEAR(rgb[64 * 3], 64.0 / 255.0 * 255.0 / 128.0, 1e-4) << "input white 128 doubles the levels";
    EXPECT_NEAR(rgb[200 * 3], 1.0, 1e-6) << "and clips above it";

    Json curves = Json{{"type", "curves"}, {"channels", Json::object()}};
    normalizeTitleArtObject(curves, "adjustment", "", context);
    EXPECT_TRUE(titleAdjustmentIsIdentity(curves));
    ASSERT_TRUE(bakeTitleToneCurve(curves, rgb));
    for (int i = 0; i < 256; i += 51) EXPECT_NEAR(rgb[(size_t)i * 3 + 1], i / 255.0, 1e-4);

    Json bc = Json{{"type", "brightness_contrast"}, {"brightness", 0}, {"contrast", 0}};
    normalizeTitleArtObject(bc, "adjustment", "", context);
    EXPECT_TRUE(titleAdjustmentIsIdentity(bc));
    bc["contrast"] = 50;
    ASSERT_TRUE(bakeTitleToneCurve(bc, rgb));
    EXPECT_NEAR(rgb[128 * 3], 0.5 + (128.0 / 255.0 - 0.5) * 1.5, 0.01) << "the midtone slope is 1 + contrast / 100";
    EXPECT_LT(rgb[32 * 3], 32.0 / 255.0) << "an S-curve: shadows go down";
    EXPECT_NEAR(rgb[0], 0.0, 1e-6) << "never clips black";
    EXPECT_NEAR(rgb[255 * 3], 1.0, 1e-6) << "or white";

    Json hue = Json{{"type", "hue_saturation"}};
    normalizeTitleArtObject(hue, "adjustment", "", context);
    EXPECT_TRUE(titleAdjustmentIsIdentity(hue));
    EXPECT_EQ(titleAdjustOp(hue), TitleAdjustOp::HueSaturation);
    EXPECT_FALSE(bakeTitleToneCurve(hue, rgb)) << "closed form, no lookup";
    Json invert = Json{{"type", "invert"}};
    EXPECT_FALSE(titleAdjustmentIsIdentity(invert));
    Json effectsOnly = Json{{"type", "effects_only"}};
    EXPECT_TRUE(titleAdjustmentIsIdentity(effectsOnly));
    EXPECT_EQ(titleAdjustOp(effectsOnly), TitleAdjustOp::None);
}
