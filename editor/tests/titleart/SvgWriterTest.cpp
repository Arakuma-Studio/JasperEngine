// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The SVG export of a title (docs/specs/S87-title-localisation-interchange.md R41): structure and
// ids, transforms baked into the coordinates (the SVG's raster equals the geometry's coverage of
// the same layer), rims as strokes, gradients with alpha stops and midpoints, masks and clips,
// blend modes, what is left out, strict, embedded rasters, deterministic bytes.

#include <gtest/gtest.h>

#include "titleart/TitleArtIO.h"
#include "titleart/geometry/TitleGeometry.h"
#include "titleart/interchange/svg/SvgWriter.h"
#include "util/LunaSVGRuntime.h"

#include <algorithm>
#include <cmath>
#include <regex>

using namespace jasperengine;
using namespace jasperengine::editor;
using titleart::Json;
using titleart::TitleArtDoc;

namespace {

    TitleArtDoc document(Json layers, int width = 512, int height = 256) {
        TitleArtDoc doc = titleart::makeNewTitleArt();
        doc.root["canvas"]["width"] = width;
        doc.root["canvas"]["height"] = height;
        doc.root["layers"] = std::move(layers);
        doc.root["exposed"] = Json::array();
        titleart::normalizeTitleArt(doc, nullptr);
        return doc;
    }

    Json star(const std::string& id, const std::string& name) {
        return Json{{"id", id}, {"type", "shape"}, {"name", name}, {"shape", "star"}, {"size", 120},
                    {"transform", {{"position", {60, -20}}, {"scale", 1.5}}},
                    {"style", {{"face", {{"fill", {{"kind", "solid"}, {"color", "#ffffff"}}}}},
                               {"rims", Json::array({Json{{"id", id.substr(0, 7) + "9"}, {"width", 8}, {"join", "round"},
                                                         {"material", {{"fill", {{"kind", "solid"}, {"color", "#000000"}}}}}}})}}}};
    }

    int count(const std::string& text, const std::string& what) {
        int n = 0;
        for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) n++;
        return n;
    }

    const titleart::TitleRectI CANVAS{0, 0, 512, 256};

}

TEST(SvgWriter, StructureAndIds) {
    Json group = {{"id", "000000a1"}, {"type", "group"}, {"name", "Logo Group"}, {"opacity", 0.5}, {"blend", "screen"},
                  {"children", Json::array({star("000000b1", "Star"), star("000000c1", "Star")})}};
    const TitleArtDoc doc = document(Json::array({group}));
    const TitleSvgResult r = writeTitleSvg(doc, {100, 20, 300, 200}, {}, nullptr);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_NE(r.text.find("viewBox=\"100 20 300 200\""), std::string::npos) << "the frame in canvas pixels";
    EXPECT_NE(r.text.find("width=\"300\" height=\"200\""), std::string::npos);
    EXPECT_NE(r.text.find("<g id=\"logo-group\" opacity=\"0.5\" style=\"mix-blend-mode:screen\">"), std::string::npos);
    EXPECT_NE(r.text.find("id=\"star\""), std::string::npos);
    EXPECT_NE(r.text.find("id=\"star-2\""), std::string::npos) << "ids are unique";
    EXPECT_EQ(r.layers, 3u);
    TitleSvgOptions twice;
    twice.scale = 2;
    const TitleSvgResult scaled = writeTitleSvg(doc, {100, 20, 300, 200}, twice, nullptr);
    EXPECT_NE(scaled.text.find("width=\"600\" height=\"400\""), std::string::npos);
    EXPECT_EQ(writeTitleSvg(doc, {100, 20, 300, 200}, {}, nullptr).text, r.text) << "deterministic";
    EXPECT_FALSE(writeTitleSvg(doc, {}, {}, nullptr).ok);
}

// the SVG raster of a moved and scaled star with a round rim covers what the geometry covers
TEST(SvgWriter, TransformsAndRimsMatchTheGeometry) {
    const TitleArtDoc doc = document(Json::array({star("000000b1", "Star")}));
    const TitleSvgResult r = writeTitleSvg(doc, CANVAS, {}, nullptr);
    ASSERT_TRUE(r.ok) << r.error;
    // the rim's stroke: 2 x its width (8 % of 120) x the layer scale 1.5
    EXPECT_NE(r.text.find("stroke-width=\"28.8\""), std::string::npos) << r.text;
    EXPECT_NE(r.text.find("stroke-linejoin=\"round\""), std::string::npos);
    EXPECT_NE(r.text.find("stroke-miterlimit=\"4\""), std::string::npos);
    // rim (black) before the face (white)
    EXPECT_LT(r.text.find("#000000"), r.text.find("#ffffff"));

    std::vector<uint8_t> px;
    int w = 0, h = 0;
    LunaSVGRuntime::ensureFonts();
    ASSERT_TRUE(LunaSVGRuntime::rasterize(r.text.data(), r.text.size(), 1.0f, false, px, w, h));
    ASSERT_EQ(w, 512);
    ASSERT_EQ(h, 256);
    titleart::geometry::TitleGeometry geometry(nullptr);
    const titleart::geometry::LayerElements els = geometry.elements(doc.layers()[0], doc);
    const titleart::geometry::TitleElement* rim = nullptr;
    for (const auto& e : els.elements) if (e.kind == titleart::geometry::ElementKind::Rim) rim = &e;
    ASSERT_NE(rim, nullptr);
    titleart::geometry::MaskFrame frame;
    frame.width = 512;
    frame.height = 256;
    frame.scale = 1.5;
    frame.origin = {(-256.0 - 60.0) / 1.5, (128.0 + 20.0) / 1.5};
    const titleart::geometry::CoverageMask mask = titleart::geometry::rasterizeElement(*rim, frame);
    ASSERT_EQ(mask.width, 512);
    size_t covered = 0, differ = 0;
    for (size_t i = 0; i < mask.pixels.size(); i++) {
        const int a = px[i * 4 + 3];
        covered += mask.pixels[i] > 127 ? 1 : 0;
        differ += std::abs(a - (int)mask.pixels[i]) > 64 ? 1 : 0;
    }
    EXPECT_GT(covered, 5000u);
    EXPECT_LT((double)differ / mask.pixels.size(), 0.005) << differ << " pixels differ (antialiasing only)";
    // the face is white where the geometry's face is
    const auto face = std::find_if(els.elements.begin(), els.elements.end(), [](const auto& e) { return e.kind == titleart::geometry::ElementKind::Face; });
    const titleart::geometry::CoverageMask faceMask = titleart::geometry::rasterizeElement(*face, frame);
    size_t white = 0, faceIn = 0;
    for (size_t i = 0; i < faceMask.pixels.size(); i++) {
        if (faceMask.pixels[i] < 250) continue;
        faceIn++;
        white += px[i * 4] > 200 ? 1 : 0;
    }
    EXPECT_GT(faceIn, 1000u);
    EXPECT_GT((double)white / faceIn, 0.99);
}

TEST(SvgWriter, RotationAndYDown) {
    Json bar = {{"id", "000000d1"}, {"type", "shape"}, {"name", "Bar"}, {"shape", "rect"}, {"size", 40}, {"aspect", 4},
                {"transform", {{"position", {0, 20}}, {"rotation", 90}}}};
    const TitleArtDoc doc = document(Json::array({bar}));
    const TitleSvgResult r = writeTitleSvg(doc, CANVAS, {}, nullptr);
    ASSERT_TRUE(r.ok);
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    ASSERT_TRUE(LunaSVGRuntime::rasterize(r.text.data(), r.text.size(), 1.0f, false, px, w, h));
    // a 160 x 40 bar turned 90 degrees, centred 20 px above the canvas centre: x 236..276, y 28..188
    int x0 = w, x1 = -1, y0 = h, y1 = -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (px[((size_t)y * w + x) * 4 + 3] < 128) continue;
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
    }
    const double longSide = (double)(y1 - y0 + 1), shortSide = (double)(x1 - x0 + 1);
    EXPECT_GT(longSide, shortSide * 3) << "turned upright";
    EXPECT_NEAR((x0 + x1 + 1) / 2.0, 256.0, 2.0);
    EXPECT_NEAR((y0 + y1 + 1) / 2.0, 128.0 - 20.0, 2.0) << "document y up is SVG y down";
}

TEST(SvgWriter, GradientsWithAlphaAndMidpoints) {
    Json layer = star("000000b1", "Star");
    layer["style"]["face"]["fill"] = {{"kind", "linear"}, {"angle", 0},
                                      {"gradient", {{"stops", Json::array({Json{{"pos", 0}, {"color", "#ff0000"}, {"alpha", 0.25}, {"mid", 0.3}},
                                                                           Json{{"pos", 1}, {"color", "#0000ff"}, {"alpha", 1}, {"mid", 0.5}}})}}}};
    Json radial = star("000000c1", "Radial");
    radial["style"]["face"]["fill"] = {{"kind", "radial"}};
    Json reflected = star("000000e1", "Reflected");
    reflected["style"]["face"]["fill"] = {{"kind", "reflected"}};
    Json angle = star("000000f1", "Angle");
    angle["style"]["face"]["fill"] = {{"kind", "angle"}};
    const TitleSvgResult r = writeTitleSvg(document(Json::array({layer, radial, reflected, angle})), CANVAS, {}, nullptr);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(count(r.text, "<linearGradient"), 2);
    EXPECT_EQ(count(r.text, "<radialGradient"), 1);
    EXPECT_NE(r.text.find("gradientUnits=\"userSpaceOnUse\""), std::string::npos);
    EXPECT_NE(r.text.find("stop-opacity=\"0.25\""), std::string::npos) << "stop alpha";
    // the midpoint at 0.3 as its own stop with the halfway colour (#800080) and alpha
    EXPECT_NE(r.text.find("<stop offset=\"0.3\" stop-color=\"#800080\" stop-opacity=\"0.625\"/>"), std::string::npos) << r.text;
    // reflected: the ramp mirrored about the centre
    EXPECT_NE(r.text.find("<stop offset=\"0.5\""), std::string::npos);
    bool warned = false;
    for (const std::string& w : r.warnings) warned = warned || w == "Angle: the angle gradient is drawn as its first colour";
    EXPECT_TRUE(warned);
}

TEST(SvgWriter, MasksAndClips) {
    Json base = star("000000b1", "Base");
    Json clipped = {{"id", "000000c1"}, {"type", "shape"}, {"name", "Fill"}, {"shape", "rect"}, {"size", 300}, {"clip", true}};
    Json masked = {{"id", "000000d1"}, {"type", "shape"}, {"name", "Masked"}, {"shape", "ellipse"},
                   {"masks", Json::array({Json{{"id", "000000d2"}, {"type", "shape"}, {"invert", true}, {"density", 0.5}},
                                          Json{{"id", "000000d3"}, {"type", "gradient"}, {"shape", "radial"}, {"length", 80}, {"feather", 4}},
                                          Json{{"id", "000000d4"}, {"type", "image"}}})}};
    Json plain = {{"id", "000000e1"}, {"type", "shape"}, {"name", "Plain"}};
    Json onPlain = {{"id", "000000e2"}, {"type", "shape"}, {"name", "On Plain"}, {"clip", true}};
    const TitleSvgResult r = writeTitleSvg(document(Json::array({base, clipped, masked, plain, onPlain})), CANVAS, {}, nullptr);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(count(r.text, "<mask id="), 3) << "the clip to a rimmed base and two masks";
    EXPECT_EQ(count(r.text, "<clipPath"), 1) << "a face-only base clips with a clip path";
    EXPECT_NE(r.text.find("fill-opacity=\"0.5\""), std::string::npos) << "density over an inverted shape";
    std::vector<std::string> expected = {"Masked: gradient mask feather drawn hard", "Masked: image mask left out"};
    for (const std::string& e : expected) EXPECT_NE(std::find(r.warnings.begin(), r.warnings.end(), e), r.warnings.end()) << e;
}

TEST(SvgWriter, LeftOutStrictAndEmbedded) {
    Json text = {{"id", "000000b1"}, {"type", "text"}, {"name", "Title"}, {"text", "AB"}, {"size", 80},
                 {"effects", Json::array({Json{{"id", "000000b2"}, {"type", "outer_glow"}}})},
                 {"style", {{"face", {{"bevel", {{"enabled", true}}}}}}}};
    Json image = {{"id", "000000c1"}, {"type", "image"}, {"name", "Photo"}};
    Json gen = {{"id", "000000d1"}, {"type", "generator"}, {"name", "Rays"}, {"generator", {{"type", "fill"}}}};
    Json hidden = {{"id", "000000e1"}, {"type", "image"}, {"name", "Hidden"}, {"visible", false}};
    Json overlay = {{"id", "000000f1"}, {"type", "shape"}, {"name", "Burn"}, {"blend", "linear_burn"}};
    const TitleArtDoc doc = document(Json::array({text, image, gen, hidden, overlay}));
    TitleSvgResult r = writeTitleSvg(doc, CANVAS, {}, nullptr);
    ASSERT_TRUE(r.ok);
    EXPECT_GT(count(r.text, "<path"), 0) << "the text's outlines";
    for (const char* line : {"Title: outer_glow effect left out", "Title: face bevel left out", "Photo: image layer left out",
                             "Rays: generator layer left out"}) {
        EXPECT_NE(std::find(r.leftOut.begin(), r.leftOut.end(), line), r.leftOut.end()) << line;
    }
    EXPECT_EQ(std::find(r.leftOut.begin(), r.leftOut.end(), "Hidden: image layer left out"), r.leftOut.end()) << "hidden layers draw nothing";
    EXPECT_NE(std::find(r.warnings.begin(), r.warnings.end(), "Burn: blend linear_burn has no CSS mode: drawn normal"), r.warnings.end());

    TitleSvgOptions strict;
    strict.strict = true;
    r = writeTitleSvg(doc, CANVAS, strict, nullptr);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.code, "svg_unrepresentable");
    EXPECT_NE(r.error.find("Photo: image layer left out"), std::string::npos);

    TitleSvgOptions embed;
    embed.embedRaster = true;
    std::vector<std::string> asked;
    r = writeTitleSvg(doc, CANVAS, embed, nullptr, [&](const Json& layer, TitleSvgRaster& out) {
        asked.push_back(layer.value("name", std::string()));
        out.png = {0x89, 'P', 'N', 'G'};
        out.frame = {10, 20, 30, 40};
        return true;
    });
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(asked, std::vector<std::string>({"Photo", "Rays"}));
    EXPECT_NE(r.text.find("<image x=\"10\" y=\"20\" width=\"30\" height=\"40\" preserveAspectRatio=\"none\" href=\"data:image/png;base64,iVBORw==\"/>"),
              std::string::npos);
}
