// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// fallback_parity (S80 §6.3, S83 §6.5, S86 AC9) through a render function that hands back
// synthetic float and RGBA8 renders (the editor unit tests have no GPU): a document with 3D
// glints is held to AC9's mean with them on and to the 3D share and max with them off; and the
// renders of one check_title call are not those of the call before it.

#include <gtest/gtest.h>

#include "titleart/TitleArtBuiltinChecks.h"
#include "titleart/TitleArtChecks.h"
#include "titleart/TitleArtIO.h"
#include "titleart/render/TitleArtCaps.h"
#include "titleart/render/TitleArtRenderer.h"

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::TitleArtDoc;

namespace {

    TitleArtDoc docOf(bool glint) {
        Json layer{{"id", "00000001"}, {"type", "text"}, {"name", "Title"}, {"text", "WIN"}, {"size", 200}, {"dimension", "3d"},
                   {"style_3d", {{"glint", {{"enabled", glint}}}}}};
        Json root{{"format", titleart::TITLE_ART_FORMAT}, {"version", titleart::TITLE_ART_VERSION}, {"layers", Json::array({layer})}};
        titleart::TitleArtParseResult parsed = titleart::parseTitleArtJson(root);
        EXPECT_TRUE(parsed.ok) << parsed.error.toString();
        return parsed.doc;
    }

    bool glintOn(const TitleArtDoc& doc) {
        const Json* layer = titleart::findLayer(doc.root, "00000001");
        return layer && (*layer)["style_3d"]["glint"].value("enabled", false);
    }

    // a flat grey render; `changed` of its pixels (a band at the top) 60 codes brighter in colour
    TitleCheckImage render(double changed) {
        TitleCheckImage image;
        image.width = 100;
        image.height = 100;
        image.rgba.assign(100 * 100 * 4, 100);
        for (int i = 0; i < 100 * 100; i++) image.rgba[(size_t)i * 4 + 3] = 255;
        const int n = (int)(changed * 100 * 100 + 0.5);
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < 3; c++) image.rgba[(size_t)i * 4 + c] = 160;
        }
        return image;
    }

    // the RGBA8 path differs from the float path by `withGlints` of the pixels while the glints are
    // on, by `withoutGlints` while they are off
    TitleCheckResult parity(bool glint, double withGlints, double withoutGlints) {
        registerBuiltinTitleChecks();
        const TitleArtDoc doc = docOf(glint);
        TitleCheckContext context{doc, "assets/title.titleart"};
        context.render = [&](const titleart::RenderRequest& request, TitleCheckImage& out, std::string&) {
            const bool forced = (request.testFlags & titleart::TITLE_ART_TEST_NO_FLOAT_TARGETS) != 0;
            out = render(forced ? (glintOn(*request.doc) ? withGlints : withoutGlints) : 0.0);
            return true;
        };
        const TitleCheckReport report = TitleArtChecks::run({TitleArtChecks::find("fallback_parity")}, context);
        EXPECT_EQ(report.checks.size(), 1u);
        return report.checks.empty() ? TitleCheckResult{} : report.checks[0];
    }

}

TEST(FallbackParityCheckTest, GlintStarsAreHeldToTheMeanAndTheRestPerPixel) {
    // the stars moved: 2 % of the pixels differ (over AC9's 1 % share) at a mean of 0.9
    const TitleCheckResult moved = parity(true, 0.02, 0.0);
    EXPECT_EQ(moved.status, TitleCheckStatus::Pass) << moved.message << " " << moved.measured.dump();
    EXPECT_NEAR(moved.measured["glints_off"]["fraction_over"].get<double>(), 0.0, 1e-9);

    // red control: the fallback is broken outside the glints (5 % with them off): it fails
    const TitleCheckResult broken = parity(true, 0.05, 0.05);
    EXPECT_EQ(broken.status, TitleCheckStatus::Fail) << broken.measured.dump();
    EXPECT_NE(broken.message.find("glints off"), std::string::npos) << broken.message;

    // red control: the glint's own light differs past AC9's mean (10 % of pixels, mean 4.5): it fails
    const TitleCheckResult bright = parity(true, 0.10, 0.0);
    EXPECT_EQ(bright.status, TitleCheckStatus::Fail) << bright.measured.dump();
    EXPECT_GT(bright.measured["mean"].get<double>(), 3.0);

    // without glints the share stays per pixel: the same 2 % fails
    const TitleCheckResult plain = parity(false, 0.02, 0.02);
    EXPECT_EQ(plain.status, TitleCheckStatus::Fail) << plain.measured.dump();
    EXPECT_FALSE(plain.measured.contains("glints_off"));
}

TEST(FallbackParityCheckTest, EachCallRendersAtItsOwnOptions) {
    // two check_title calls on one document, the second at another render scale, from contexts
    // that live where the first one did (red: the second reused the first call's renders)
    registerBuiltinTitleChecks();
    const TitleArtDoc doc = docOf(false);
    std::vector<float> scales;
    for (float scale : {0.25f, 0.5f}) {
        TitleCheckContext context{doc, "assets/title.titleart"};
        context.options.renderScale = scale;
        context.render = [&](const titleart::RenderRequest& request, TitleCheckImage& out, std::string&) {
            scales.push_back(request.pxScale);
            out = render(0.0);
            return true;
        };
        TitleArtChecks::run({TitleArtChecks::find("fallback_parity")}, context);
    }
    ASSERT_EQ(scales.size(), 4u) << "each call renders the float and the RGBA8 path";
    EXPECT_FLOAT_EQ(scales[0], 0.25f * scales[2] / 0.5f);
    EXPECT_NE(scales[0], scales[2]) << "the second call rendered at its own scale";
}
