// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S84 AC16 (R35, R45): the library's pixel checks. The editor unit tests have no GPU (S82
// Deviation 13), so the metrics run on synthetic RGBA faces (a gold-ramp face and its +40 deg olive
// twin, a flat and a banded metal face) and the checks run with a render function that hands back
// such a face; without a render function they SKIP. style_lint's results through the registry.

#include <gtest/gtest.h>

#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtIO.h"
#include "titleart/library/LibraryChecks.h"
#include "titleart/render/TitleArtRenderer.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;
using namespace jasperengine::editor;
using namespace jasperengine::editor::titlelibrary;
using titleart::Json;
using titleart::TitleArtDoc;

namespace {

    // the gold scheme's face fill (white core, pale yellow, yellow, orange, brown)
    Json goldRamp() {
        return Json{{"stops", Json::array({Json{{"pos", 0}, {"color", "#ffffff"}}, Json{{"pos", 0.16}, {"color", "#fff8b8"}}, Json{{"pos", 0.38}, {"color", "#ffd820"}},
                                           Json{{"pos", 0.6}, {"color", "#ff9400"}}, Json{{"pos", 0.8}, {"color", "#b84000"}}, Json{{"pos", 1}, {"color", "#4a1000"}}})}};
    }

    // a face whose rows run down `ramp` (as a vertical gradient fill shows it), inside a
    // transparent border that must not count
    TitleCheckImage rampFace(const Json& ramp, double hueShift = 0.0, double chromaScale = 1.0) {
        const Json gradient = ramp.is_string() ? Json{{"stops", Json::array({Json{{"pos", 0}, {"color", ramp}}, Json{{"pos", 1}, {"color", ramp}}})}} : ramp;
        const titleart::TitleGradient g = titleart::decodeGradient(gradient);
        TitleCheckImage image;
        image.width = 40;
        image.height = 72;
        image.rgba.assign(static_cast<size_t>(image.width) * image.height * 4, 0);
        for (int y = 4; y < image.height - 4; y++) {
            const double pos = static_cast<double>(y - 4) / (image.height - 9);
            // the gradient by hand: linear between the stops (their midpoints are 0.5)
            const auto& s = g.stops;
            titleart::TitleColor c = s.back().color;
            for (size_t i = 0; i + 1 < s.size(); i++) {
                if (pos > s[i + 1].pos) continue;
                const double t = (pos - s[i].pos) / (s[i + 1].pos - s[i].pos);
                c = {static_cast<float>(s[i].color.r + (s[i + 1].color.r - s[i].color.r) * t), static_cast<float>(s[i].color.g + (s[i + 1].color.g - s[i].color.g) * t),
                     static_cast<float>(s[i].color.b + (s[i + 1].color.b - s[i].color.b) * t)};
                break;
            }
            Oklch o = toOklch(c.r, c.g, c.b);
            const bool white = std::min({c.r, c.g, c.b}) >= COLOR_MATCH_WHITE_CORE;
            if (!white) {
                o.h = std::fmod(o.h + hueShift + 360.0, 360.0);
                o.C *= chromaScale;
                c = fromOklch(o);
            }
            for (int x = 4; x < image.width - 4; x++) {
                uint8_t* p = &image.rgba[(static_cast<size_t>(y) * image.width + x) * 4];
                p[0] = static_cast<uint8_t>(std::lround(c.r * 255.0));
                p[1] = static_cast<uint8_t>(std::lround(c.g * 255.0));
                p[2] = static_cast<uint8_t>(std::lround(c.b * 255.0));
                p[3] = 255;
            }
        }
        image.frame = {0, 0, image.width, image.height};
        return image;
    }

    // a flat face of one colour, or one banded from dark to light
    TitleCheckImage metalFace(bool banded) {
        TitleCheckImage image;
        image.width = 32;
        image.height = 64;
        image.rgba.assign(static_cast<size_t>(image.width) * image.height * 4, 0);
        for (int y = 0; y < image.height; y++) {
            const uint8_t v = banded ? static_cast<uint8_t>(40 + (y * 210) / (image.height - 1)) : 200;
            for (int x = 0; x < image.width; x++) {
                uint8_t* p = &image.rgba[(static_cast<size_t>(y) * image.width + x) * 4];
                p[0] = v;
                p[1] = static_cast<uint8_t>(v * 0.8);
                p[2] = static_cast<uint8_t>(v * 0.3);
                p[3] = 255;
            }
        }
        // a dark drop shadow at alpha 0.3 outside the face: not face pixels
        for (int x = 0; x < image.width; x++) image.rgba[(static_cast<size_t>(image.height - 1) * image.width + x) * 4 + 3] = 77;
        image.frame = {0, 0, image.width, image.height};
        return image;
    }

    TitleArtDoc docWith(const Json& layer, const Json& extra = Json::object()) {
        Json root = Json{{"format", titleart::TITLE_ART_FORMAT}, {"version", titleart::TITLE_ART_VERSION}, {"layers", Json::array({layer})}};
        for (auto it = extra.begin(); it != extra.end(); ++it) root[it.key()] = it.value();
        titleart::TitleArtParseResult parsed = titleart::parseTitleArtJson(root);
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return parsed.doc;
    }

    Json textLayer(Json face) {
        return Json{{"id", "00000001"}, {"type", "text"}, {"name", "Title"}, {"text", "WIN"}, {"size", 200},
                    {"style", {{"face", std::move(face)}, {"rims", Json::array({Json{{"width", 6}, {"material", {{"fill", {{"kind", "solid"}, {"color", "#000000"}}}}}}})}}}};
    }

    TitleCheckReport runCheck(const char* id, TitleCheckContext& context) {
        registerLibraryChecks();
        const CheckDef* def = TitleArtChecks::find(id);
        EXPECT_NE(def, nullptr) << id;
        if (!def) return {};
        return TitleArtChecks::run({def}, context);
    }

}

TEST(LibraryChecksTest, AGoldFaceGoneOliveFailsAndItsTwinPasses) {
    const ColorMatchMetric gold = measureColorSchemeMatch(rampFace(goldRamp()), goldRamp());
    ASSERT_TRUE(gold.judged) << gold.note;
    EXPECT_FALSE(gold.hueFails) << "hue " << gold.faceHue << ", deviation " << gold.hueDeviation;
    EXPECT_FALSE(gold.chromaFails) << gold.chromaRatio;
    EXPECT_NEAR(gold.hueDeviation, 0.0, 3.0);
    EXPECT_GT(gold.pixels, 1000u);

    const ColorMatchMetric olive = measureColorSchemeMatch(rampFace(goldRamp(), 40.0), goldRamp());
    ASSERT_TRUE(olive.judged);
    EXPECT_TRUE(olive.hueFails);
    EXPECT_NEAR(olive.hueDeviation, 40.0, 4.0);
    EXPECT_NEAR(olive.faceHue, gold.faceHue + 40.0, 4.0);
    // why the lightness-matched deviation: gold's ramp spans brown to yellow, so the mean hue of an
    // evenly lit olive face lands only a little past the span
    EXPECT_LT(olive.hueOutside, COLOR_MATCH_HUE_SLACK) << "span " << olive.rampHueFrom << ".." << olive.rampHueTo;
}

TEST(LibraryChecksTest, AFaceThatLostItsChromaFails) {
    const Json red = Json{{"stops", Json::array({Json{{"pos", 0}, {"color", "#ff4040"}}, Json{{"pos", 1}, {"color", "#800000"}}})}};
    EXPECT_FALSE(measureColorSchemeMatch(rampFace(red), red).chromaFails);
    const ColorMatchMetric pale = measureColorSchemeMatch(rampFace(red, 0.0, 0.3), red);
    EXPECT_TRUE(pale.chromaFails);
    EXPECT_NEAR(pale.chromaRatio, 0.3, 0.05);
    EXPECT_FALSE(pale.hueFails) << "the hue itself stayed";
}

TEST(LibraryChecksTest, WhiteCoresAndAchromaticRampsAreNotJudged) {
    // a silver ramp has no hue to compare
    const Json silver = Json{{"stops", Json::array({Json{{"pos", 0}, {"color", "#ffffff"}}, Json{{"pos", 1}, {"color", "#303438"}}})}};
    const ColorMatchMetric m = measureColorSchemeMatch(rampFace(silver), silver);
    EXPECT_FALSE(m.judged);
    // a face that is all white core draws no ink to judge
    const ColorMatchMetric white = measureColorSchemeMatch(rampFace(Json("#ffffff")), goldRamp());
    EXPECT_FALSE(white.judged);
    EXPECT_EQ(white.pixels, 0u);
}

TEST(LibraryChecksTest, AFlatMetalFailsTheRangeAndABandedOnePasses) {
    EXPECT_LT(measureMetalRange(metalFace(false)), 1.0);
    EXPECT_GE(measureMetalRange(metalFace(true)), METAL_MIN_L_RANGE);
    TitleCheckImage empty;
    EXPECT_LT(measureMetalRange(empty), 0.0);
}

TEST(LibraryChecksTest, ThePixelChecksSkipWithoutARenderFunction) {
    const TitleArtDoc doc = docWith(textLayer(Json{{"bevel", {{"enabled", true}}}}));
    TitleCheckContext context{doc, "assets/title.titleart"};
    for (const char* id : {"color_scheme_match", "metal_range"}) {
        const TitleCheckReport report = runCheck(id, context);
        ASSERT_EQ(report.checks.size(), 1u) << id;
        EXPECT_EQ(report.checks[0].status, TitleCheckStatus::Skip) << id;
    }
}

TEST(LibraryChecksTest, MetalRangeRendersTheFaceWithoutRimsAndFixesTheFinish) {
    const TitleArtDoc doc = docWith(textLayer(Json{{"bevel", {{"enabled", true}}}}));
    TitleCheckContext context{doc, "assets/title.titleart"};
    bool banded = false;
    std::vector<titleart::RenderRequest> requests;
    context.render = [&](const titleart::RenderRequest& request, TitleCheckImage& out, std::string&) {
        requests.push_back(request);
        out = metalFace(banded);
        return true;
    };
    TitleCheckReport report = runCheck("metal_range", context);
    ASSERT_EQ(report.checks.size(), 1u);
    const TitleCheckResult& flat = report.checks[0];
    EXPECT_EQ(flat.status, TitleCheckStatus::Fail) << flat.message;
    EXPECT_LT(flat.measured["l_range"].get<double>(), METAL_MIN_L_RANGE);
    ASSERT_TRUE(flat.fix.is_object());
    EXPECT_EQ(flat.fix["ops"][0]["values"]["face.finish.enabled"], true);
    ASSERT_EQ(requests.size(), 2u) << "coverage and colour";
    for (const titleart::RenderRequest& r : requests) {
        EXPECT_EQ(r.isolateLayer, "00000001");
        ASSERT_FALSE(r.overrides.empty());
        EXPECT_EQ(r.overrides[0].path, "style.rims");
        EXPECT_TRUE(r.overrides[0].value.empty());
    }

    // the same face with light and dark bands (a new document: a new run renders again)
    banded = true;
    const TitleArtDoc twin = docWith(textLayer(Json{{"bevel", {{"enabled", true}}}, {"opacity", 0.99}}));
    TitleCheckContext twinContext{twin, "assets/title.titleart"};
    twinContext.render = context.render;
    report = runCheck("metal_range", twinContext);
    ASSERT_EQ(report.checks.size(), 1u);
    EXPECT_EQ(report.checks[0].status, TitleCheckStatus::Pass) << report.checks[0].message;

    // a face that is not metal is not judged
    const TitleArtDoc paint = docWith(textLayer(Json::object()));
    TitleCheckContext paintContext{paint, "assets/title.titleart"};
    paintContext.render = context.render;
    report = runCheck("metal_range", paintContext);
    ASSERT_EQ(report.checks.size(), 1u);
    EXPECT_EQ(report.checks[0].status, TitleCheckStatus::Skip);
}

TEST(LibraryChecksTest, ColorSchemeMatchFailsAnOliveGoldFaceWithTheSchemeAsItsFix) {
    // the scheme travels in the document (as an exposed color_scheme parameter's data does)
    const Json scheme = Json{{"format", titleart::TITLE_COLORS_FORMAT}, {"version", titleart::TITLE_COLORS_VERSION}, {"id", "s84-test-gold"},
                             {"paints", {{"face", {{"fill", {{"kind", "linear"}, {"gradient", goldRamp()}}}}}}}};
    Json layer = textLayer(Json{{"fill", {{"kind", "linear"}, {"gradient", goldRamp()}}}});
    layer["look"] = Json{{"id", "casino/outline-gold"}, {"color_scheme", "s84-test-gold"}};
    const TitleArtDoc doc = docWith(layer, Json{{"color_schemes", {{"s84-test-gold", scheme}}}});
    double shift = 40.0;
    TitleCheckContext context{doc, "assets/title.titleart"};
    context.render = [&](const titleart::RenderRequest&, TitleCheckImage& out, std::string&) {
        out = rampFace(goldRamp(), shift);
        return true;
    };
    TitleCheckReport report = runCheck("color_scheme_match", context);
    ASSERT_EQ(report.checks.size(), 1u);
    const TitleCheckResult& olive = report.checks[0];
    EXPECT_EQ(olive.status, TitleCheckStatus::Fail) << olive.message;
    ASSERT_TRUE(olive.fix.is_object());
    EXPECT_EQ(olive.fix["ops"][0], (Json{{"op", "apply_color_scheme"}, {"layer", "id:00000001"}, {"color_scheme", "s84-test-gold"}}));

    shift = 0.0;
    Json twinLayer = layer;
    twinLayer["name"] = "Twin";
    const TitleArtDoc twin = docWith(twinLayer, Json{{"color_schemes", {{"s84-test-gold", scheme}}}});
    TitleCheckContext twinContext{twin, "assets/title.titleart"};
    twinContext.render = context.render;
    report = runCheck("color_scheme_match", twinContext);
    ASSERT_EQ(report.checks.size(), 1u);
    EXPECT_EQ(report.checks[0].status, TitleCheckStatus::Pass) << report.checks[0].message;

    // no layer records a scheme: nothing to compare
    const TitleArtDoc plain = docWith(textLayer(Json::object()));
    TitleCheckContext plainContext{plain, "assets/title.titleart"};
    plainContext.render = context.render;
    report = runCheck("color_scheme_match", plainContext);
    ASSERT_EQ(report.checks.size(), 1u);
    EXPECT_EQ(report.checks[0].status, TitleCheckStatus::Skip);
}

TEST(LibraryChecksTest, StyleLintReportsOneResultPerFindingWithTheLook) {
    Json layer = textLayer(Json::object());
    layer["style"]["rims"] = Json::array({Json{{"width", 1}, {"material", {{"fill", {{"kind", "solid"}, {"color", "#000000"}}}}}}});
    layer["look"] = Json{{"id", "casino/outline-gold"}};
    const TitleArtDoc doc = docWith(layer);
    TitleCheckContext context{doc, "assets/title.titleart"};
    TitleCheckReport report = runCheck("style_lint", context);
    ASSERT_EQ(report.checks.size(), 1u);
    const TitleCheckResult& thin = report.checks[0];
    EXPECT_EQ(thin.id, "style_lint.rims_thin");
    EXPECT_EQ(thin.status, TitleCheckStatus::Warn);
    EXPECT_EQ(thin.measured["value"], 1.0);
    EXPECT_EQ(thin.measured["element"], "rims");
    EXPECT_EQ(thin.measured["look"], "casino/outline-gold");
    EXPECT_EQ(thin.threshold["value"], LINT_MIN_RIMS);
    EXPECT_NE(thin.message.find("look casino/outline-gold"), std::string::npos) << thin.message;
    EXPECT_TRUE(thin.fix.contains("ops"));

    // hollow letters with nothing to cut through render nothing: a failure
    Json hollow = textLayer(Json::object());
    hollow["style"]["rims"] = Json::array();
    hollow["style"]["knockout"] = true;
    const TitleArtDoc hollowDoc = docWith(hollow);
    TitleCheckContext hollowContext{hollowDoc, "assets/title.titleart"};
    report = runCheck("style_lint", hollowContext);
    ASSERT_EQ(report.checks.size(), 1u);
    EXPECT_EQ(report.checks[0].id, "style_lint.noop_knockout");
    EXPECT_EQ(report.checks[0].status, TitleCheckStatus::Fail);

    const TitleArtDoc clean = docWith(textLayer(Json::object()));
    TitleCheckContext cleanContext{clean, "assets/title.titleart"};
    report = runCheck("style_lint", cleanContext);
    ASSERT_EQ(report.checks.size(), 1u);
    EXPECT_EQ(report.checks[0].id, "style_lint");
    EXPECT_EQ(report.checks[0].status, TitleCheckStatus::Pass);
}

TEST(LibraryChecksTest, RegisteringTwiceKeepsOneOfEachInTheStyleGroup) {
    registerLibraryChecks();
    registerLibraryChecks();
    std::vector<const CheckDef*> selected;
    std::string error;
    std::vector<std::string> valid;
    ASSERT_TRUE(TitleArtChecks::select({"style"}, selected, error, valid));
    std::vector<std::string> ids;
    for (const CheckDef* d : selected) ids.push_back(d->id);
    EXPECT_EQ(ids, (std::vector<std::string>{"style_lint", "color_scheme_options_empty", "color_scheme_match", "metal_range"}));
    EXPECT_EQ(TitleArtChecks::find("metal_range")->owner, "S84");
}
