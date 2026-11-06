// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// S80 R24-R26 (AC22): a layer's elements (faces, rims and their plates, per-character face
// splits, outline operations), the eleven shapes, Image and Vector (SVG) layers, the queries S81,
// S82, S86 and S87 use (outlines, rim bands, content and reach bounds), image traces.

#include "gtest/gtest.h"

#include "titleart/support/TitleTestAssets.h"

#include "titleart/TitleEffectRegistry.h"
#include "titleart/geometry/Offset.h"
#include "titleart/geometry/PathUtil.h"
#include "titleart/geometry/Plate.h"
#include "titleart/geometry/Shapes.h"
#include "titleart/geometry/TitleGeometry.h"
#include "titleart/geometry/Trace.h"

#include <cmath>

using namespace jasperengine;
using namespace jasperengine::titleart;
using namespace jasperengine::titleart::geometry;
using namespace jasperengine::titleart::test;

namespace {

    Json rim(double width, Json extra = Json::object()) {
        Json r = {{"width", width}};
        for (auto it = extra.begin(); it != extra.end(); ++it) r[it.key()] = it.value();
        return r;
    }

    // a filled square polygon centred at (cx, cy)
    Path square(double cx, double cy, double half, bool clockwise = false) {
        Path p;
        p.moveTo(cx - half, cy - half);
        if (clockwise) {
            p.lineTo(cx - half, cy + half);
            p.lineTo(cx + half, cy + half);
            p.lineTo(cx + half, cy - half);
        } else {
            p.lineTo(cx + half, cy - half);
            p.lineTo(cx + half, cy + half);
            p.lineTo(cx - half, cy + half);
        }
        p.close();
        return p;
    }

    double area(const Path& p) { return std::abs(p.signedArea(0.01)); }

    double sumArea(const std::vector<Path>& paths) {
        double a = 0;
        for (const Path& p : paths) a += p.signedArea(0.01);
        return std::abs(a);
    }

    // an RGBA image: an opaque disc of radius r (pixels) in a w x h transparent image
    TitleImageData disc(int w, int h, double r) {
        TitleImageData img;
        img.width = w;
        img.height = h;
        img.rgba.assign((size_t)w * h * 4, 0);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const double d = std::hypot(x + 0.5 - w / 2.0, y + 0.5 - h / 2.0);
                const double a = std::clamp(r - d + 0.5, 0.0, 1.0);
                uint8_t* p = &img.rgba[((size_t)y * w + x) * 4];
                p[0] = 200;
                p[1] = 30;
                p[2] = 30;
                p[3] = (uint8_t)std::lround(a * 255);
            }
        }
        return img;
    }

    class TitleGeometryTest : public ::testing::Test {
    protected:
        TestAssetResolver assets;
    };

}

TEST_F(TitleGeometryTest, TextElementsGrowRimsCumulatively) {
    const TitleArtDoc doc = documentWith({{{"type", "text"}, {"id", "0000000a"}, {"text", "確定"}, {"font", fontRef(DELA_GOTHIC)},
                                           {"style", {{"rims", Json::array({rim(3), rim(4, {{"enabled", false}}), rim(0), rim(5, {{"join", "round"}, {"fill_counters", true}, {"offset", {0, -2}}})})}}}}});
    TitleGeometry geometry(&assets);
    const Json& layer = doc.layers()[0];
    const LayerElements els = geometry.elements(layer, doc);
    EXPECT_TRUE(els.problems.empty());
    EXPECT_DOUBLE_EQ(els.unit, 2.0) << "size 200: 1 % = 2 layer units";
    ASSERT_EQ(els.elements.size(), 3u) << "the face and the two enabled rims with a width";
    const TitleElement& face = els.elements[0];
    EXPECT_EQ(face.kind, ElementKind::Face);
    EXPECT_EQ(face.outlines.size(), 2u) << "one fill group per glyph";
    EXPECT_EQ(face.material, &layer["style"]["face"]);
    const TitleElement& inner = els.elements[1];
    const TitleElement& outer = els.elements[2];
    EXPECT_EQ(inner.kind, ElementKind::Rim);
    EXPECT_EQ(inner.rimIndex, 0);
    EXPECT_DOUBLE_EQ(inner.grow, 6);
    EXPECT_EQ(inner.join, StrokeJoin::Miter);
    EXPECT_DOUBLE_EQ(inner.miterLimit, 4);
    EXPECT_EQ(inner.rimId, layer["style"]["rims"][0]["id"].get<std::string>());
    EXPECT_EQ(outer.rimIndex, 1);
    EXPECT_DOUBLE_EQ(outer.grow, 16) << "6 + 10: rims grow from the face by the widths inside them";
    EXPECT_EQ(outer.join, StrokeJoin::Round);
    EXPECT_TRUE(outer.fillCounters);
    EXPECT_DOUBLE_EQ(outer.offset.y, -4);
    EXPECT_EQ(outer.material, &layer["style"]["rims"][3]["material"]);
    EXPECT_EQ(geometry.rimBands(layer), (std::vector<double>{6, 16}));
    // the frame rule: mitred outlines pad by grow x 4
    const TitleRect b = elementBounds(inner);
    EXPECT_NEAR(b.x, els.baseBounds.x - 24, 1e-9);
    EXPECT_GT(els.spans.size(), 0u);
}

TEST_F(TitleGeometryTest, PerCharacterFacesSplitTheFaceButNotTheRims) {
    const Json red = {{"fill", {{"color", "#ff0000"}}}};
    const TitleArtDoc doc = documentWith({{{"type", "text"}, {"text", "確定確"}, {"font", fontRef(DELA_GOTHIC)},
                                           {"chars", {{"1", {{"face", red}}}}}, {"style", {{"rims", Json::array({rim(3)})}}}}});
    TitleGeometry geometry(&assets);
    const Json& layer = doc.layers()[0];
    const LayerElements els = geometry.elements(layer, doc);
    ASSERT_EQ(els.elements.size(), 3u);
    EXPECT_EQ(els.elements[0].faceGroup, 0);
    EXPECT_EQ(els.elements[0].outlines.size(), 2u) << "確 and 確 share the layer's face";
    EXPECT_EQ(els.elements[1].faceGroup, 1);
    EXPECT_EQ(els.elements[1].outlines.size(), 1u);
    EXPECT_EQ(els.elements[1].material, &layer["chars"]["1"]["face"]);
    EXPECT_EQ(els.elements[2].outlines.size(), 3u) << "the rim stays continuous around the word";
    // span boxes: each glyph's ink and its line's
    ASSERT_EQ(els.spans.size(), 3u);
    EXPECT_DOUBLE_EQ(els.spans[0].lineBox.x, els.spans[0].charBox.x);
    EXPECT_NEAR(els.spans[2].lineBox.x + els.spans[2].lineBox.width, els.spans[2].charBox.x + els.spans[2].charBox.width, 1e-9);
}

TEST_F(TitleGeometryTest, OutlineOperationsRunWarpDistortWeight) {
    TitleGeometry geometry(&assets);
    const auto face = [&](Json extra) {
        Json layer = {{"type", "text"}, {"text", "HI"}, {"font", fontRef(OSWALD)}};
        for (auto it = extra.begin(); it != extra.end(); ++it) layer[it.key()] = it.value();
        const TitleArtDoc doc = documentWith({layer});
        return geometry.elements(doc.layers()[0], doc).elements[0];
    };
    const TitleElement plain = face({});
    const TitleElement warped = face({{"warp", {{"style", "arc"}, {"bend", 50}}}});
    ASSERT_EQ(warped.outlines.size(), 2u);
    EXPECT_NE(warped.outlines[0].controlBounds().y, plain.outlines[0].controlBounds().y);
    for (PathVerb v : warped.outlines[0].verbList()) EXPECT_NE(v, PathVerb::Cubic);
    const TitleElement distorted = face({{"distort", {{"mode", "corners"}, {"corners", {{0.2, 0}, {0, 0}, {0, 0}, {0.2, 0}}}}}});
    EXPECT_GT(distorted.outlines[0].controlBounds().x, plain.outlines[0].controlBounds().x + 10) << "the left corners moved right";
    // weight grows with round joins; a large negative weight makes thin strokes vanish
    const TitleElement heavy = face({{"weight", 3}});
    EXPECT_GT(sumArea(heavy.outlines), sumArea(plain.outlines) * 1.1);
    const TitleElement light = face({{"weight", -1}});
    EXPECT_LT(sumArea(light.outlines), sumArea(plain.outlines));
    const TitleElement gone = face({{"weight", -20}});
    EXPECT_TRUE(gone.outlines.empty()) << "strokes thinner than 2 |delta| vanish";
}

TEST(TitleOffsetTest, WeightUnionsAndOffsetsWithRoundJoins) {
    // two overlapping squares: the non-zero union merges them; a reversed one is a counter
    Path p = square(0, 0, 50);
    p.append(square(60, 0, 50));
    const Path grown = weightPath(p, 10, 200);
    const TitleRect b = grown.controlBounds();
    EXPECT_NEAR(b.x, -60, 0.01);
    EXPECT_NEAR(b.x + b.width, 120, 0.01);
    // round joins: the grown corner is cut by the arc, so the area is the Minkowski sum's of the
    // 160 x 100 union
    const double expected = 160 * 100 + 2 * 160 * 10 + 2 * 100 * 10 + 3.14159265 * 100;
    EXPECT_NEAR(area(grown), expected, expected * 0.002);
    EXPECT_EQ(weightPath(p, 0, 200).pointList().size(), p.pointList().size()) << "weight 0 is the outline itself";
    Path counter = square(0, 0, 50);
    counter.append(square(0, 0, 20, true));
    const Path thicker = weightPath(counter, 5, 200);
    EXPECT_EQ(thicker.contourCount(), 2u);
    EXPECT_NEAR(thicker.signedArea(0.01), (110.0 * 110 - 4 * 25 + 3.14159265 * 25) - 30.0 * 30, 3) << "the counter shrinks";
}

TEST(TitlePlateTest, ContourClosingHullAndBox) {
    // two squares 20 apart: closing by 15 bridges the gap, closing by 5 does not
    Path pair = square(-40, 0, 30);
    pair.append(square(40, 0, 30));
    const Path closed = plateBase(pair, PlateParams{PlateKind::Contour, 15.0 / 2, 0, 0}, 200, 0);
    EXPECT_EQ(closed.contourCount(), 1u);
    // the bridge fills the gap but for the two shallow notches a closing disc leaves at its ends
    EXPECT_GT(area(closed), 140.0 * 60 - 200);
    EXPECT_LT(area(closed), 140.0 * 60 + 1);
    const Path open = plateBase(pair, PlateParams{PlateKind::Contour, 5.0 / 2, 0, 0}, 200, 0);
    EXPECT_EQ(open.contourCount(), 2u);
    EXPECT_EQ(plateBase(pair, PlateParams{}, 200, 0).pointList().size(), pair.pointList().size()) << "close 0: the outline itself";
    // the hull of an L is convex, counter-clockwise
    Path l;
    l.moveTo(0, 0);
    l.lineTo(100, 0);
    l.lineTo(100, 20);
    l.lineTo(20, 20);
    l.lineTo(20, 100);
    l.lineTo(0, 100);
    l.close();
    const Path hull = plateBase(l, PlateParams{PlateKind::Hull, 0, 0, 0}, 200, 0);
    EXPECT_GT(hull.signedArea(0.01), 0);
    EXPECT_NEAR(area(hull), 100 * 100 - 80 * 80 / 2.0, 1);
    // a box leaning 20 deg plus a text shear of 10, corners chamfered
    const Path box = plateBase(l, PlateParams{PlateKind::Box, 0, 5, 20}, 200, 10);
    EXPECT_EQ(box.pointList().size(), 8u) << "four corners, each cut in two";
    const TitleRect bb = box.controlBounds();
    EXPECT_NEAR(bb.height, 100, 1e-9);
    EXPECT_GT(bb.width, 100 + 100 * std::tan(30 * 3.14159265 / 180) - 1);
}

TEST(TitleShapesTest, ElevenShapesWindClockwiseAndFitTheirBox) {
    const char* shapes[] = {"rect", "parallelogram", "ellipse", "orb", "polygon", "star", "burst", "shield", "speech", "shout", "ribbon"};
    for (const char* name : shapes) {
        SCOPED_TRACE(name);
        ShapeParams s;
        s.shape = name;
        s.size = 100;
        s.aspect = 2;
        s.corner = std::string(name) == "polygon" ? 0 : 10;
        s.jitter = 0.4;
        const Path p = shapePath(s);
        ASSERT_FALSE(p.empty());
        EXPECT_LT(p.signedArea(0.01), 0) << "clockwise in y up, like TrueType glyphs";
        const TitleRect b = p.bounds();
        if (std::string(name) != "speech" && std::string(name) != "shout") {
            EXPECT_NEAR(b.height, 100, 1.0);
            EXPECT_NEAR(b.x + b.width / 2, 0, 1.0) << "centred";
        }
        if (std::string(name) == "rect" || std::string(name) == "ellipse" || std::string(name) == "polygon") {
            EXPECT_NEAR(b.width, 200, 1.0);
        }
    }
    // rounded corners are circular arcs clamped to half an edge: a full corner is a capsule
    ShapeParams capsule;
    capsule.size = 100;
    capsule.aspect = 3;
    capsule.corner = 100;
    EXPECT_NEAR(area(shapePath(capsule)), 200 * 100 + 3.14159265 * 50 * 50, 30);
    // the speech tail is spliced into one contour reaching the tip
    ShapeParams speech;
    speech.shape = "speech";
    speech.size = 100;
    speech.tail = {30, -120};
    const Path bubble = shapePath(speech);
    EXPECT_EQ(bubble.contourCount(), 1u);
    EXPECT_NEAR(bubble.bounds().y, -120, 1e-6);
    // the ribbon is three same-winding contours
    ShapeParams ribbon;
    ribbon.shape = "ribbon";
    ribbon.size = 100;
    ribbon.aspect = 4;
    ribbon.tail = {60, 0};
    ribbon.inner = 0.5;
    const Path banner = shapePath(ribbon);
    EXPECT_EQ(banner.contourCount(), 3u);
    for (const Path& c : splitContours(banner)) EXPECT_LT(c.signedArea(0.01), 0);
}

TEST(TitleShapesTest, BurstJitterIsSeeded) {
    ShapeParams a;
    a.shape = "burst";
    a.points = 12;
    a.jitter = 0.8;
    a.seed = 7;
    const Path one = shapePath(a), two = shapePath(a);
    ASSERT_EQ(one.pointList().size(), two.pointList().size());
    for (size_t i = 0; i < one.pointList().size(); i++) EXPECT_EQ(one.pointList()[i].x, two.pointList()[i].x);
    ShapeParams b = a;
    b.seed = 8;
    EXPECT_NE(shapePath(b).pointList()[0].x, one.pointList()[0].x);
    // Pachislop's mulberry32: the first draws of seed 1
    Mulberry32 rng(1);
    const double first = rng.next();
    EXPECT_GE(first, 0.0);
    EXPECT_LT(first, 1.0);
    Mulberry32 again(1);
    EXPECT_EQ(again.next(), first);
}

TEST_F(TitleGeometryTest, ShapeLayersTakeTheDocumentSeed) {
    TitleGeometry geometry(&assets);
    Json root = {{"type", "shape"}, {"shape", "burst"}, {"jitter", 0.8}, {"points", 9}};
    TitleArtDoc one = documentWith({root});
    TitleArtDoc reseeded = one;
    reseeded.root["seed"] = 5;
    const Path a = geometry.elements(one.layers()[0], one).elements[0].outlines[0];
    const Path b = geometry.elements(reseeded.layers()[0], reseeded).elements[0].outlines[0];
    EXPECT_NE(a.pointList()[0].x, b.pointList()[0].x) << "Reseed on the document changes every random element";
    const Path again = geometry.elements(one.layers()[0], one).elements[0].outlines[0];
    EXPECT_EQ(again.pointList()[0].x, a.pointList()[0].x);
}

TEST_F(TitleGeometryTest, BoundsFollowTransformsGroupsAndReach) {
    const TitleArtDoc doc = documentWith({
        {{"type", "shape"}, {"id", "00000001"}, {"shape", "rect"}, {"size", 100}, {"aspect", 2}},
        {{"type", "shape"}, {"id", "00000002"}, {"shape", "rect"}, {"size", 100}, {"aspect", 2},
         {"transform", {{"position", {300, 100}}, {"rotation", 90}, {"scale", 0.5}}}},
        {{"type", "group"}, {"id", "00000003"}, {"transform", {{"position", {-200, 0}}}},
         {"children", Json::array({Json{{"type", "shape"}, {"id", "00000004"}, {"shape", "rect"}, {"size", 50}},
                                   Json{{"type", "shape"}, {"id", "00000005"}, {"shape", "rect"}, {"size", 50}, {"visible", false}}})}},
    });
    TitleGeometry geometry(&assets);
    const TitleRect a = geometry.contentBounds(doc.layers()[0], doc);
    EXPECT_NEAR(a.x, -100, 1e-9);
    EXPECT_NEAR(a.width, 200, 1e-9);
    EXPECT_NEAR(a.height, 100, 1e-9);
    // rotated a quarter turn and halved, at (300, 100)
    const TitleRect b = geometry.contentBounds(doc.layers()[1], doc);
    EXPECT_NEAR(b.width, 50, 1e-9);
    EXPECT_NEAR(b.height, 100, 1e-9);
    EXPECT_NEAR(b.x + b.width / 2, 300, 1e-9);
    EXPECT_NEAR(b.y + b.height / 2, 100, 1e-9);
    // a group: its visible children, in group space moved by the group's transform
    const TitleRect g = geometry.contentBounds(doc.layers()[2], doc);
    const Json& child = doc.layers()[2]["children"][0];
    const TitleRect c = geometry.contentBounds(child, doc);
    EXPECT_NEAR(c.x + c.width / 2, -200, 1e-9);
    EXPECT_NEAR(g.x, c.x - 1, 1e-9) << "the child's reach (one output pixel) is the group's content";
    // reach: the effects' reach and 1.5 x the feather of the masks
    EffectDef def;
    def.type = "test_geometry_reach";
    def.label = "Reach";
    def.category = "Test";
    def.description = "test";
    def.reach = [](const ParamValues&) { return 12.0f; };
    ASSERT_TRUE(TitleEffectRegistry::get().add(def));
    TitleArtDoc withFx = doc;
    withFx.layers()[0]["effects"] = Json::array({Json{{"id", "0000000f"}, {"type", "test_geometry_reach"}, {"enabled", true}, {"params", Json::object()}}});
    withFx.layers()[0]["masks"] = Json::array({Json{{"id", "0000000e"}, {"type", "gradient"}, {"feather", 4}}});
    const TitleRect r = geometry.reachBounds(withFx.layers()[0], withFx, 2.0);
    EXPECT_NEAR(r.x, -100 - 12 - 6 - 0.5, 1e-9);
    withFx.layers()[0]["effects"][0]["enabled"] = false;
    EXPECT_NEAR(geometry.reachBounds(withFx.layers()[0], withFx, 2.0).x, -100 - 6 - 0.5, 1e-9);
    TitleEffectRegistry::get().removeEffect("test_geometry_reach");
}

TEST_F(TitleGeometryTest, ImageLayersGrowFromTheirAlpha) {
    assets.addImage("textures/disc.png", disc(64, 32, 14));
    const TitleArtDoc doc = documentWith({{{"type", "image"}, {"image", fontRef("textures/disc.png")}, {"size", 200},
                                           {"style", {{"rims", Json::array({rim(5), rim(5, {{"plate", {{"kind", "hull"}}}})})}}}}});
    TitleGeometry geometry(&assets);
    const LayerElements els = geometry.elements(doc.layers()[0], doc);
    EXPECT_TRUE(els.fromAlpha);
    ASSERT_EQ(els.elements.size(), 3u);
    const TitleElement& face = els.elements[0];
    EXPECT_TRUE(face.outlines.empty());
    ASSERT_TRUE(face.source.image);
    EXPECT_NEAR(face.source.rect.width, 200, 1e-9) << "the larger side is the size";
    EXPECT_NEAR(face.source.rect.height, 100, 1e-9);
    // a contour rim grows through the alpha's distance field (the renderer's); a hull rim is an outline
    EXPECT_TRUE(els.elements[1].source.image);
    EXPECT_DOUBLE_EQ(els.elements[1].grow, 10);
    ASSERT_EQ(els.elements[2].outlines.size(), 1u);
    const TitleRect hull = els.elements[2].outlines[0].controlBounds();
    EXPECT_NEAR(hull.width, 2 * 14 * 200 / 64.0, 8) << "the hull of the opaque disc, not of the image";
    // outlines(): the traced alpha (S86's extrusion input)
    const std::vector<GlyphOutline> traced = geometry.outlines(doc.layers()[0], doc);
    ASSERT_EQ(traced.size(), 1u);
    EXPECT_NEAR(area(traced[0].path), 3.14159265 * std::pow(14 * 200 / 64.0, 2), 300);
    // a missing image is a problem, never a crash
    const TitleArtDoc missing = documentWith({{{"type", "image"}, {"image", fontRef("textures/none.png")}}});
    const LayerElements none = geometry.elements(missing.layers()[0], missing);
    ASSERT_EQ(none.problems.size(), 1u);
    EXPECT_EQ(none.problems[0].code, "asset_missing");
}

TEST_F(TitleGeometryTest, VectorShapeModeKeepsStrokesAndReportsWhatItIgnores) {
    assets.addText("art/logo.svg",
                   "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 50'>"
                   "<rect x='0' y='0' width='60' height='50' fill='#123456'/>"
                   "<path d='M70 10 L95 40' stroke='#000' stroke-width='6' stroke-linecap='round' fill='none'/>"
                   "<text x='0' y='10'>Hi</text></svg>");
    const TitleArtDoc doc = documentWith({{{"type", "vector"}, {"svg", fontRef("art/logo.svg")}, {"size", 100},
                                           {"style", {{"rims", Json::array({rim(4)})}}}}});
    TitleGeometry geometry(&assets);
    const LayerElements els = geometry.elements(doc.layers()[0], doc);
    ASSERT_EQ(els.elements.size(), 2u);
    EXPECT_EQ(els.elements[0].outlines.size(), 1u);
    ASSERT_EQ(els.elements[0].strokes.size(), 1u);
    EXPECT_EQ(els.elements[0].strokes[0].cap, StrokeCap::Round);
    // fitted: the larger side of the ink (stroke widths included) is the size, y flipped
    EXPECT_NEAR(els.baseBounds.width, 100, 1e-6);
    EXPECT_EQ(els.elements[1].strokes.size(), 1u) << "a contour rim widens the stroke by its grow";
    ASSERT_EQ(els.problems.size(), 1u);
    EXPECT_EQ(els.problems[0].code, "svg_feature_ignored");
    EXPECT_EQ(els.problems[0].detail["features"], Json::array({"text"}));
    // outlines(): the fills and the strokes as outlines
    const std::vector<GlyphOutline> outlines = geometry.outlines(doc.layers()[0], doc);
    ASSERT_EQ(outlines.size(), 2u);
    EXPECT_GT(area(outlines[1].path), 0);
    // artwork mode: the SVG's own picture, rims from its alpha
    TitleArtDoc art = doc;
    art.layers()[0]["mode"] = "artwork";
    const LayerElements pic = geometry.elements(art.layers()[0], art);
    EXPECT_TRUE(pic.fromAlpha);
    ASSERT_TRUE(pic.elements[0].source.svg);
    EXPECT_NEAR(pic.elements[0].source.rect.width, 100, 1e-9);
    EXPECT_NEAR(pic.elements[0].source.rect.height, 50, 1e-9);
}

TEST(TitleTraceTest, AlphaBecomesOutersAndHoles) {
    // a ring: an outer loop counter-clockwise, a hole clockwise
    TitleImageData ring = disc(80, 80, 30);
    const TitleImageData hole = disc(80, 80, 12);
    for (size_t i = 0; i < ring.rgba.size(); i += 4) ring.rgba[i + 3] = (uint8_t)std::max(0, ring.rgba[i + 3] - hole.rgba[i + 3]);
    const Path p = traceImage(ring, 80, TraceParams{});
    const std::vector<Path> loops = splitContours(p);
    ASSERT_EQ(loops.size(), 2u);
    int outers = 0, holes = 0;
    for (const Path& l : loops) (l.signedArea(0.01) > 0 ? outers : holes)++;
    EXPECT_EQ(outers, 1);
    EXPECT_EQ(holes, 1);
    EXPECT_NEAR(std::abs(p.signedArea(0.01)), 3.14159265 * (30 * 30 - 12 * 12), 40);
    // despeckle drops the hole (it is under 10 % of size^2), smoothing keeps the area
    const Path filled = traceImage(ring, 80, TraceParams{128, 0, 10});
    EXPECT_EQ(splitContours(filled).size(), 1u);
    const Path smooth = traceImage(ring, 80, TraceParams{128, 2, 0});
    EXPECT_NEAR(std::abs(smooth.signedArea(0.01)), std::abs(p.signedArea(0.01)), 60);
    // the same pixels give the same commands (canonical start vertex)
    const Path again = traceImage(ring, 80, TraceParams{});
    EXPECT_EQ(again.pointList().size(), p.pointList().size());
    EXPECT_EQ(again.pointList()[0].x, p.pointList()[0].x);
}
